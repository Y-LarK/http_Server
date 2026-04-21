#pragma once
#include "Buffer.h"
#include "httpRequest.h"
#include "httpResponse.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <sys/epoll.h>
#include <unistd.h>

class Connection
{
  public:
    enum class State
    {
        READING,
        PROCESSING,
        WRITING,
        CLOSING
    };

    // close_cb 在连接需要关闭时回调，传回 fd 供 epoll 注销
    Connection(int fd, int epoll_fd, const std::string& ip, std::function<void(int)> close_cb)
        : fd_(fd), epoll_fd_(epoll_fd), ip_(ip), state_(State::READING), keep_alive_(false),
          close_cb_(std::move(close_cb))
    {
        updateActiveTime();
    }

    ~Connection()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    int getFd() const
    {
        return fd_;
    }
    bool isTimeout(int seconds) const
    {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - last_active_).count() >
               seconds;
    }
    void updateActiveTime()
    {
        last_active_ = std::chrono::steady_clock::now();
    }
    State getState() const
    {
        return state_;
    }

    // ── 读事件处理 ──────────────────────────────────────────────────
    // ET 模式：循环读直到 EAGAIN
    // 返回 true  = 请求解析完成，可以处理
    // 返回 false = 数据没到齐 / 连接关闭（通过 state_ == CLOSING 判断）
    bool handleRead()
    {
        updateActiveTime();
        while (true)
        {
            ssize_t n = read_buf_.readFromFd(fd_);
            if (n > 0)
            {
                // 继续尝试读
            }
            else if (n == 0)
            {
                // 对端关闭
                state_ = State::CLOSING;
                return false;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // 读完了
                if (errno == EINTR)
                    continue;
                // 真正的错误
                state_ = State::CLOSING;
                return false;
            }
        }

        // 尝试解析
        bool done = request_.parse(read_buf_);
        if (request_.isError())
        {
            state_ = State::CLOSING;
            return false;
        }
        if (done)
        {
            keep_alive_ = request_.isKeepAlive();
            state_      = State::PROCESSING;
        }
        return done;
    }

    // ── 写事件处理 ──────────────────────────────────────────────────
    // ET 模式：循环写直到 EAGAIN 或写完
    // 返回 true = 写完了
    bool handleWrite()
    {
        while (write_buf_.readableBytes() > 0)
        {
            ssize_t n = write_buf_.writeToFd(fd_);
            if (n > 0)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return false; // 写缓冲满
            if (n < 0 && errno == EINTR)
                continue;
            state_ = State::CLOSING;
            return false;
        }
        // 写完
        if (!keep_alive_)
        {
            state_ = State::CLOSING;
        }
        else
        {
            // Keep-Alive：重置状态，等待下一个请求
            request_.reset();
            state_ = State::READING;
            // 重新监听 EPOLLIN，去掉 EPOLLOUT
            rearmEpoll(EPOLLIN | EPOLLET | EPOLLONESHOT);
        }
        return true;
    }

    // ── 业务处理完成后，把响应放入写缓冲 ───────────────────────────
    void sendResponse(HttpResponse& resp)
    {
        resp.makeResponse(write_buf_);
        state_ = State::WRITING;
        // 监听可写事件
        rearmEpoll(EPOLLOUT | EPOLLET | EPOLLONESHOT);
    }

    // 工作线程调用完 processRequest 后，通知 epoll 重新监听写事件
    // （如果在主线程直接写完，也可以不走这里）
    void rearmEpoll(uint32_t events)
    {
        epoll_event ev{};
        ev.events   = events;
        ev.data.ptr = this;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &ev);
    }

    HttpRequest& getRequest()
    {
        return request_;
    }
    Buffer& getWriteBuf()
    {
        return write_buf_;
    }

  private:
    int fd_;
    int epoll_fd_;
    std::string ip_;
    State state_;
    bool keep_alive_;

  public:
    std::function<void(int)> close_cb_;

    Buffer read_buf_;
    Buffer write_buf_;
    HttpRequest request_;

    std::chrono::steady_clock::time_point last_active_;
};