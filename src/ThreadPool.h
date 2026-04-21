#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool
{
  public:
    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency()) : stop_(false)
    {
        if (thread_count == 0)
            thread_count = 4;
        threads_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i)
        {
            threads_.emplace_back([this] { workerThread(); });
        }
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_)
        {
            if (t.joinable())
                t.join();
        }
    }

    // 提交任务，不可拷贝的 lambda 用 std::function 包装
    void submit(std::function<void()> task)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_)
                throw std::runtime_error("ThreadPool is stopped");
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    size_t pendingTasks() const
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return tasks_.size();
    }

  private:
    void workerThread()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
};