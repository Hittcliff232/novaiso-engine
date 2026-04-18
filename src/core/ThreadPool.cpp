#include "core/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <latch>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace novaiso::core {

ThreadPool& ThreadPool::Shared() {
    static ThreadPool pool;
    return pool;
}

std::size_t ThreadPool::AutoWorkerCount() {
    const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
    return hardware_threads > 1 ? static_cast<std::size_t>(hardware_threads - 1) : 0u;
}

ThreadPool::ThreadPool() = default;

ThreadPool::~ThreadPool() {
    StopWorkers();
}

void ThreadPool::Configure(std::size_t worker_count) {
    if (worker_count == WorkerCount()) {
        return;
    }
    StopWorkers();
    if (worker_count > 0) {
        StartWorkers(worker_count);
    }
}

std::size_t ThreadPool::WorkerCount() const {
    std::scoped_lock lock(mutex_);
    return workers_.size();
}

bool ThreadPool::Enabled() const {
    return WorkerCount() > 0;
}

void ThreadPool::WaitIdle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [&] { return tasks_.empty() && active_tasks_ == 0; });
}

void ThreadPool::ParallelFor(std::size_t item_count,
                             const std::function<void(std::size_t begin, std::size_t end)>& fn,
                             std::size_t min_items_per_task) {
    if (item_count == 0) {
        return;
    }

    const std::size_t worker_count = WorkerCount();
    if (worker_count == 0 || item_count <= std::max<std::size_t>(min_items_per_task, 1)) {
        fn(0, item_count);
        return;
    }

    const std::size_t desired_tasks = std::min(item_count, worker_count + 1);
    const std::size_t chunk_size = std::max<std::size_t>(
        min_items_per_task,
        (item_count + desired_tasks - 1) / desired_tasks);

    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    ranges.reserve((item_count + chunk_size - 1) / chunk_size);
    for (std::size_t begin = 0; begin < item_count; begin += chunk_size) {
        ranges.emplace_back(begin, std::min(begin + chunk_size, item_count));
    }

    if (ranges.size() <= 1) {
        fn(0, item_count);
        return;
    }

    std::exception_ptr first_error;
    std::mutex error_mutex;
    std::latch done(static_cast<std::ptrdiff_t>(ranges.size() - 1));
    for (std::size_t index = 0; index + 1 < ranges.size(); ++index) {
        const auto [begin, end] = ranges[index];
        Enqueue([&, begin, end] {
            try {
                fn(begin, end);
            } catch (...) {
                std::scoped_lock error_lock(error_mutex);
                if (first_error == nullptr) {
                    first_error = std::current_exception();
                }
            }
            done.count_down();
        });
    }

    const auto [caller_begin, caller_end] = ranges.back();
    fn(caller_begin, caller_end);
    done.wait();

    if (first_error != nullptr) {
        std::rethrow_exception(first_error);
    }
}

void ThreadPool::StartWorkers(std::size_t worker_count) {
    std::scoped_lock lock(mutex_);
    stop_ = false;
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex_);
                    task_cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    ++active_tasks_;
                }

                task();

                {
                    std::scoped_lock lock(mutex_);
                    if (active_tasks_ > 0) {
                        --active_tasks_;
                    }
                    if (tasks_.empty() && active_tasks_ == 0) {
                        idle_cv_.notify_all();
                    }
                }
            }
        });
    }
}

void ThreadPool::StopWorkers() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    task_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    {
        std::scoped_lock lock(mutex_);
        workers_.clear();
        std::queue<std::function<void()>> empty;
        tasks_.swap(empty);
        active_tasks_ = 0;
        stop_ = false;
    }
    idle_cv_.notify_all();
}

void ThreadPool::Enqueue(std::function<void()> task) {
    {
        std::scoped_lock lock(mutex_);
        tasks_.push(std::move(task));
    }
    task_cv_.notify_one();
}

}  // namespace novaiso::core
