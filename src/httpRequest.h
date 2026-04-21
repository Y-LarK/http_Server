#pragma once
#include "Buffer.h"
#include <algorithm>
#include <string>
#include <unordered_map>

class HttpRequest
{
  public:
    enum class ParseState
    {
        REQUEST_LINE,
        HEADERS,
        BODY,
        FINISH,
        ERROR
    };
    enum class Method
    {
        GET,
        POST,
        PUT,
        DELETE_,
        HEAD,
        UNKNOWN
    };

    HttpRequest()
    {
        reset();
    }

    void reset()
    {
        state_  = ParseState::REQUEST_LINE;
        method_ = Method::UNKNOWN;
        path_.clear();
        version_.clear();
        headers_.clear();
        body_.clear();
    }

    // 增量解析，返回 true 表示解析完成
    bool parse(Buffer& buf)
    {
        static const char* CRLF = "\r\n";

        while (state_ != ParseState::FINISH && state_ != ParseState::ERROR)
        {
            if (state_ == ParseState::REQUEST_LINE || state_ == ParseState::HEADERS)
            {
                // 找行结束符
                const char* start = buf.peek();
                const char* end   = buf.peek() + buf.readableBytes();
                const char* crlf  = std::search(start, end, CRLF, CRLF + 2);
                if (crlf == end)
                    return false; // 数据还没到

                std::string line(start, crlf);
                buf.retrieve(crlf + 2 - start);

                if (state_ == ParseState::REQUEST_LINE)
                {
                    if (!parseRequestLine(line))
                    {
                        state_ = ParseState::ERROR;
                        return false;
                    }
                    state_ = ParseState::HEADERS;
                }
                else
                {
                    if (line.empty())
                    {
                        // 空行，头部结束
                        auto it = headers_.find("content-length");
                        if (it != headers_.end() && std::stoul(it->second) > 0)
                            state_ = ParseState::BODY;
                        else
                            state_ = ParseState::FINISH;
                    }
                    else
                    {
                        parseHeaderLine(line);
                    }
                }
            }
            else if (state_ == ParseState::BODY)
            {
                size_t content_length = std::stoul(headers_["content-length"]);
                if (buf.readableBytes() < content_length)
                    return false;
                body_ = std::string(buf.peek(), content_length);
                buf.retrieve(content_length);
                state_ = ParseState::FINISH;
            }
        }
        return state_ == ParseState::FINISH;
    }

    bool isFinished() const
    {
        return state_ == ParseState::FINISH;
    }
    bool isError() const
    {
        return state_ == ParseState::ERROR;
    }
    bool isKeepAlive() const
    {
        auto it = headers_.find("connection");
        if (it != headers_.end())
        {
            std::string v = it->second;
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            if (v == "close")
                return false;
            if (v == "keep-alive")
                return true;
        }
        return version_ == "HTTP/1.1"; // 1.1 默认 keep-alive
    }

    Method getMethod() const
    {
        return method_;
    }
    std::string getPath() const
    {
        return path_;
    }
    std::string getVersion() const
    {
        return version_;
    }
    std::string getHeader(const std::string& key) const
    {
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = headers_.find(lower);
        return it != headers_.end() ? it->second : "";
    }
    const std::string& getBody() const
    {
        return body_;
    }

  private:
    bool parseRequestLine(const std::string& line)
    {
        // "GET /path HTTP/1.1"
        size_t s1 = line.find(' ');
        size_t s2 = line.find(' ', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos)
            return false;

        std::string method_str = line.substr(0, s1);
        path_                  = line.substr(s1 + 1, s2 - s1 - 1);
        version_               = line.substr(s2 + 1);

        if (method_str == "GET")
            method_ = Method::GET;
        else if (method_str == "POST")
            method_ = Method::POST;
        else if (method_str == "PUT")
            method_ = Method::PUT;
        else if (method_str == "DELETE")
            method_ = Method::DELETE_;
        else if (method_str == "HEAD")
            method_ = Method::HEAD;
        else
            method_ = Method::UNKNOWN;

        // 去掉 query string
        size_t qmark = path_.find('?');
        if (qmark != std::string::npos)
            path_ = path_.substr(0, qmark);

        return true;
    }

    void parseHeaderLine(const std::string& line)
    {
        // "Key: Value"
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            return;

        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // 去首尾空格，统一小写 key
        auto trim = [](std::string& s)
        {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key);
        trim(value);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        headers_[key] = value;
    }

    ParseState state_;
    Method method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};