#include "Connection.h"
#include "ThreadPool.h"
#include "httpResponse.h"

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

// ────────────────────────────────────────────────────────
//  全局配置
// ────────────────────────────────────────────────────────
static const int PORT            = 8080;
static const int MAX_EVENTS      = 10000;
static const int TIMEOUT_SEC     = 60;
static const char* STATIC_DIR    = "./www";
static const size_t THREAD_COUNT = 4;

// ────────────────────────────────────────────────────────
//  全局状态
// ────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static int g_epoll_fd = -1;
static std::unordered_map<int, std::unique_ptr<Connection>> g_conns;
static std::mutex g_conns_mutex;

// ────────────────────────────────────────────────────────
//  工具函数
// ────────────────────────────────────────────────────────
static void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void addToEpoll(int epoll_fd, int fd, uint32_t events, void* ptr)
{
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = ptr;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static void removeFromEpoll(int epoll_fd, int fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

// ────────────────────────────────────────────────────────
//  连接关闭（主线程 or 工作线程均可调用）
// ────────────────────────────────────────────────────────
static void closeConnection(int fd)
{
    removeFromEpoll(g_epoll_fd, fd);
    std::lock_guard<std::mutex> lock(g_conns_mutex);
    g_conns.erase(fd); // unique_ptr 析构时 close(fd)
}

// ────────────────────────────────────────────────────────
//  业务处理：在工作线程中执行
// ────────────────────────────────────────────────────────
static void processRequest(Connection* conn)
{
    HttpRequest& req = conn->getRequest();
    HttpResponse resp;
    resp.setKeepAlive(req.isKeepAlive());

    std::string path = std::string(STATIC_DIR) + req.getPath();

    // 目录请求默认 index.html
    if (path.back() == '/')
        path += "index.html";

    if (req.getMethod() == HttpRequest::Method::GET || req.getMethod() == HttpRequest::Method::HEAD)
    {

        if (resp.setFile(path))
        {
            resp.setStatusCode(HttpResponse::StatusCode::OK);
        }
        else
        {
            // 文件不存在，返回简单 404
            resp.setStatusCode(HttpResponse::StatusCode::NOT_FOUND);
            resp.setHeader("Content-Type", "text/html; charset=utf-8");
            resp.setBody("<html><body><h1>404 Not Found</h1><p>" + req.getPath() +
                         "</p></body></html>");
        }

        // HEAD 不返回 body（makeResponse 时已包含，这里用一个小技巧：
        // 实际上 HEAD 响应 body 应该为空，但 Content-Length 要正确）
        // 简单处理：GET/HEAD 统一走 setFile，HEAD 的 body 由客户端忽略
    }
    else if (req.getMethod() == HttpRequest::Method::POST)
    {
        // 示例：简单 echo API
        if (req.getPath() == "/api/echo")
        {
            resp.setStatusCode(HttpResponse::StatusCode::OK);
            resp.setHeader("Content-Type", "application/json");
            resp.setBody("{\"echo\":\"" + req.getBody() + "\"}");
        }
        else
        {
            resp.setStatusCode(HttpResponse::StatusCode::NOT_FOUND);
            resp.setBody("Not Found");
        }
    }
    else
    {
        resp.setStatusCode(HttpResponse::StatusCode::BAD_REQUEST);
        resp.setBody("Bad Request");
    }

    conn->sendResponse(resp);
}

// ────────────────────────────────────────────────────────
//  main
// ────────────────────────────────────────────────────────
int main()
{
    // 忽略 SIGPIPE，防止对端关闭时进程退出
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { g_running = false; });

    // ── 创建 epoll ──
    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0)
    {
        perror("epoll_create1");
        return 1;
    }

    // ── 创建监听 socket ──
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 1024) < 0)
    {
        perror("listen");
        return 1;
    }

    setNonBlocking(listen_fd);
    // 监听 socket 只监听 EPOLLIN，不用 ONESHOT
    addToEpoll(g_epoll_fd, listen_fd, EPOLLIN | EPOLLET, nullptr);

    // ── 线程池 ──
    ThreadPool pool(THREAD_COUNT);

    printf("[EpollHTTP] Listening on port %d, static dir: %s, threads: %zu\n", PORT, STATIC_DIR,
           THREAD_COUNT);

    // ── 事件循环 ──
    std::vector<epoll_event> events(MAX_EVENTS);
    int next_timeout_check = 0;

    while (g_running)
    {
        int n = epoll_wait(g_epoll_fd, events.data(), MAX_EVENTS, 1000 /*ms*/);

        // 每隔 ~10s 检查超时连接
        if (++next_timeout_check >= 10)
        {
            next_timeout_check = 0;
            std::lock_guard<std::mutex> lock(g_conns_mutex);
            for (auto it = g_conns.begin(); it != g_conns.end();)
            {
                if (it->second->isTimeout(TIMEOUT_SEC))
                {
                    removeFromEpoll(g_epoll_fd, it->first);
                    it = g_conns.erase(it);
                }
                else
                    ++it;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            void* ptr = events[i].data.ptr;

            // ── 新连接 ──
            if (ptr == nullptr)
            {
                while (true)
                {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd          = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
                    if (conn_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        break;
                    }

                    setNonBlocking(conn_fd);

                    // TCP_NODELAY 减少小包延迟
                    int flag = 1;
                    setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

                    auto conn            = std::make_unique<Connection>(conn_fd, g_epoll_fd, ip,
                                                             [](int fd) { closeConnection(fd); });
                    Connection* conn_ptr = conn.get();

                    {
                        std::lock_guard<std::mutex> lock(g_conns_mutex);
                        g_conns[conn_fd] = std::move(conn);
                    }

                    addToEpoll(g_epoll_fd, conn_fd, EPOLLIN | EPOLLET | EPOLLONESHOT, conn_ptr);
                }
                continue;
            }

            Connection* conn = static_cast<Connection*>(ptr);
            int fd           = conn->getFd();
            uint32_t ev      = events[i].events;

            // ── 读事件 ──
            if (ev & (EPOLLIN | EPOLLRDHUP))
            {
                bool ready = conn->handleRead();

                if (conn->getState() == Connection::State::CLOSING)
                {
                    closeConnection(fd);
                    continue;
                }

                if (ready)
                {
                    // 请求解析完成，丢给工作线程
                    pool.submit(
                        [conn, fd]()
                        {
                            processRequest(conn);
                            // 如果 Keep-Alive 重置后需要再次监听读
                            // 注意：sendResponse 内部已经调用了 rearmEpoll
                        });
                }
                // 未解析完：ONESHOT 触发后不会再收到事件，等数据到了 epoll 会再触发？
                // 不对！ONESHOT 需要手动重新 arm，数据没读完要重新监听 EPOLLIN
                else if (conn->getState() == Connection::State::READING)
                {
                    conn->rearmEpoll(EPOLLIN | EPOLLET | EPOLLONESHOT);
                }
            }

            // ── 写事件 ──
            if (ev & EPOLLOUT)
            {
                bool done = conn->handleWrite();
                if (conn->getState() == Connection::State::CLOSING)
                {
                    closeConnection(fd);
                }
                // done && keep-alive: handleWrite 内部已 rearmEpoll(EPOLLIN)
                // !done: 写缓冲满，handleWrite 内部已由 epoll 自动重触发（EPOLLOUT 未清除）
                (void)done;
            }

            // ── 错误/断开 ──
            if (ev & (EPOLLERR | EPOLLHUP))
            {
                closeConnection(fd);
            }
        }
    }

    close(listen_fd);
    close(g_epoll_fd);
    printf("[EpollHTTP] Shutdown.\n");
    return 0;
}