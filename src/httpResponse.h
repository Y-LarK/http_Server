#pragma once
#include "Buffer.h"
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

class HttpResponse
{
  public:
    enum class StatusCode
    {
        OK                    = 200,
        NOT_MODIFIED          = 304,
        BAD_REQUEST           = 400,
        FORBIDDEN             = 403,
        NOT_FOUND             = 404,
        INTERNAL_SERVER_ERROR = 500,
    };

    HttpResponse() : status_code_(StatusCode::OK), keep_alive_(false)
    {
    }

    void setStatusCode(StatusCode code)
    {
        status_code_ = code;
    }
    void setKeepAlive(bool v)
    {
        keep_alive_ = v;
    }
    void setHeader(const std::string& k, const std::string& v)
    {
        headers_[k] = v;
    }
    void setBody(const std::string& body)
    {
        body_                      = body;
        headers_["Content-Length"] = std::to_string(body_.size());
    }

    // 读取文件，设置 body 和 Content-Type
    bool setFile(const std::string& filepath)
    {
        struct stat st;
        if (stat(filepath.c_str(), &st) < 0 || S_ISDIR(st.st_mode))
            return false;

        std::ifstream f(filepath, std::ios::binary);
        if (!f)
            return false;

        std::ostringstream ss;
        ss << f.rdbuf();
        body_ = ss.str();

        headers_["Content-Length"] = std::to_string(body_.size());
        headers_["Content-Type"]   = getMimeType(filepath);
        return true;
    }

    // 序列化响应写入 Buffer
    void makeResponse(Buffer& buf)
    {
        // 状态行
        std::string resp = "HTTP/1.1 " + std::to_string(static_cast<int>(status_code_)) + " " +
                           statusMessage() + "\r\n";
        buf.append(resp);

        // 通用头
        buf.append("Server: EpollHTTP/1.0\r\n");
        if (keep_alive_)
        {
            buf.append("Connection: keep-alive\r\n");
            buf.append("Keep-Alive: timeout=60, max=1000\r\n");
        }
        else
        {
            buf.append("Connection: close\r\n");
        }

        // 自定义头
        for (auto& [k, v] : headers_)
        {
            buf.append(k + ": " + v + "\r\n");
        }

        buf.append("\r\n"); // 空行
        if (!body_.empty())
            buf.append(body_);
    }

  private:
    std::string statusMessage() const
    {
        switch (status_code_)
        {
        case StatusCode::OK:
            return "OK";
        case StatusCode::NOT_MODIFIED:
            return "Not Modified";
        case StatusCode::BAD_REQUEST:
            return "Bad Request";
        case StatusCode::FORBIDDEN:
            return "Forbidden";
        case StatusCode::NOT_FOUND:
            return "Not Found";
        case StatusCode::INTERNAL_SERVER_ERROR:
            return "Internal Server Error";
        default:
            return "Unknown";
        }
    }

    static std::string getMimeType(const std::string& path)
    {
        size_t dot = path.rfind('.');
        if (dot == std::string::npos)
            return "application/octet-stream";
        std::string ext = path.substr(dot + 1);
        if (ext == "html" || ext == "htm")
            return "text/html; charset=utf-8";
        if (ext == "css")
            return "text/css";
        if (ext == "js")
            return "application/javascript";
        if (ext == "json")
            return "application/json";
        if (ext == "png")
            return "image/png";
        if (ext == "jpg" || ext == "jpeg")
            return "image/jpeg";
        if (ext == "gif")
            return "image/gif";
        if (ext == "svg")
            return "image/svg+xml";
        if (ext == "ico")
            return "image/x-icon";
        if (ext == "txt")
            return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    StatusCode status_code_;
    bool keep_alive_;
    std::string body_;
    std::unordered_map<std::string, std::string> headers_;
};