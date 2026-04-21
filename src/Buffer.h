#pragma once
#include <cstring>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

// 简单的线性缓冲区，读写指针分离
// 布局: [已读区域 | 可读数据 | 可写空间]
//        0       read_pos_  write_pos_  capacity
class Buffer
{
  public:
    explicit Buffer(size_t initial_size = 4096) : buffer_(initial_size), read_pos_(0), write_pos_(0)
    {
    }

    // 可读字节数
    size_t readableBytes() const
    {
        return write_pos_ - read_pos_;
    }
    // 可写字节数
    size_t writableBytes() const
    {
        return buffer_.size() - write_pos_;
    }

    // 读指针起始位置
    const char* peek() const
    {
        return buffer_.data() + read_pos_;
    }
    char* beginWrite()
    {
        return buffer_.data() + write_pos_;
    }

    // 消费 len 字节（移动读指针）
    void retrieve(size_t len)
    {
        if (len >= readableBytes())
            retrieveAll();
        else
            read_pos_ += len;
    }

    void retrieveAll()
    {
        read_pos_  = 0;
        write_pos_ = 0;
    }

    void append(const char* data, size_t len)
    {
        ensureWritable(len);
        memcpy(beginWrite(), data, len);
        write_pos_ += len;
    }

    void append(const std::string& str)
    {
        append(str.data(), str.size());
    }

    // 从 fd 读数据到缓冲区，利用栈上 64KB 做溢出接收（减少 read 次数）
    ssize_t readFromFd(int fd)
    {
        char extra[65536];
        struct iovec iov[2];
        const size_t writable = writableBytes();

        iov[0].iov_base = beginWrite();
        iov[0].iov_len  = writable;
        iov[1].iov_base = extra;
        iov[1].iov_len  = sizeof(extra);

        ssize_t n = readv(fd, iov, 2);
        if (n < 0)
            return n;

        if (static_cast<size_t>(n) <= writable)
        {
            write_pos_ += n;
        }
        else
        {
            write_pos_ = buffer_.size();
            append(extra, n - writable);
        }
        return n;
    }

    // 把缓冲区数据写入 fd
    ssize_t writeToFd(int fd)
    {
        ssize_t n = write(fd, peek(), readableBytes());
        if (n > 0)
            retrieve(n);
        return n;
    }

    std::string toString() const
    {
        return std::string(peek(), readableBytes());
    }

  private:
    void ensureWritable(size_t len)
    {
        if (writableBytes() >= len)
            return;

        // 空间不足：先尝试整理（把已读区域腾出来）
        if (read_pos_ + writableBytes() >= len)
        {
            size_t readable = readableBytes();
            memmove(buffer_.data(), peek(), readable);
            read_pos_  = 0;
            write_pos_ = readable;
        }
        else
        {
            // 真的不够，扩容
            buffer_.resize(write_pos_ + len);
        }
    }

    std::vector<char> buffer_;
    size_t read_pos_;
    size_t write_pos_;
};