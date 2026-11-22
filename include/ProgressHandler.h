/*
 * Copyright (c) 2026 ohdiff contributors.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <mutex>

class PeriodicTask {
public:
    PeriodicTask(std::function<void()> task, std::chrono::milliseconds interval)
        : task_(task), interval_(interval), stopFlag_(false)
    {
        workerThread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopFlag_) {
                if (cv_.wait_for(lock, interval_, [this] { return stopFlag_.load(std::memory_order_relaxed); })) {
                    break;
                }
                task_();
            }
        });
    }

    ~PeriodicTask()
    {
        stopFlag_.store(true, std::memory_order_relaxed);
        cv_.notify_all();
        workerThread_.join();
    }

    PeriodicTask(const PeriodicTask&) = delete;
    PeriodicTask& operator=(const PeriodicTask&) = delete;

private:
    std::function<void()> task_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> stopFlag_;
    std::thread workerThread_;
    std::condition_variable cv_;
    std::mutex mutex_;
};

class ProgressHandlerInterface {
public:
    virtual void add_progress(size_t delta) = 0;
    virtual size_t get_progress() const = 0;
    virtual void reset() = 0;
};

class SimpleProgressHandler : public ProgressHandlerInterface {
private:
    std::atomic<size_t> progress;

public:
    SimpleProgressHandler() : progress(0) {}

    inline void add_progress(size_t delta) override
    {
        progress.fetch_add(delta, std::memory_order_relaxed);
    }

    inline size_t get_progress() const override
    {
        return progress.load(std::memory_order_relaxed);
    }

    inline void reset() override
    {
        progress.store(0, std::memory_order_relaxed);
    }
};

class PeriodicProgressHandler : public SimpleProgressHandler {
private:
    std::function<void(const PeriodicProgressHandler&)> task;
    PeriodicTask periodicTask;
    std::chrono::milliseconds interval;

public:
    PeriodicProgressHandler(decltype(task) task, std::chrono::milliseconds interval)
        : task(std::move(task)), interval(interval), periodicTask([this] { this->task(*this); }, interval)
    {
    }

    inline std::chrono::milliseconds get_interval() const
    {
        return interval;
    }
};
