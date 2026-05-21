#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace soff {

class ThreadPool {
public:
    explicit ThreadPool(unsigned int threads = 0)
    {
        const auto count = threads > 0 ? threads
            : std::max(1u, std::thread::hardware_concurrency() - 1);
        for (unsigned int i = 0; i < count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    template<typename F>
    auto post(F&& task) -> std::future<decltype(task())> {
        using R = decltype(task());
        auto p = std::make_shared<std::promise<R>>();
        auto future = p->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push([p, t = std::forward<F>(task)]() mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        t(); p->set_value();
                    } else {
                        p->set_value(t());
                    }
                } catch (...) {
                    p->set_exception(std::current_exception());
                }
            });
        }
        cv_.notify_one();
        return future;
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace soff
