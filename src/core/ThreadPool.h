#pragma once

#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace novaiso::core {

class ThreadPool {
public:
    static ThreadPool& Shared();
    static std::size_t AutoWorkerCount();

    ThreadPool();
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Configure(std::size_t worker_count);
    [[nodiscard]] std::size_t WorkerCount() const;
    [[nodiscard]] bool Enabled() const;
    void WaitIdle();
    void ParallelFor(std::size_t item_count,
                     const std::function<void(std::size_t begin, std::size_t end)>& fn,
                     std::size_t min_items_per_task = 1);

private:
    void StartWorkers(std::size_t worker_count);
    void StopWorkers();
    void Enqueue(std::function<void()> task);

    mutable std::mutex mutex_;
    std::condition_variable task_cv_;
    std::condition_variable idle_cv_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::size_t active_tasks_ = 0;
    bool stop_ = false;
};

}  // namespace novaiso::core
