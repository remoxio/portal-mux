#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace rdc {

// UI posts, worker runs: keeps protocol calls on the worker.
class TaskQueue {
public:
    void post(std::function<void()> task)
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    void drain()
    {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& t : tasks) t();
    }

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

} // namespace rdc
