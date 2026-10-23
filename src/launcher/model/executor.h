// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Where the launcher's slow work runs. A view-model hands a job to its Executor and the job posts what it
// finds to a Mailbox; the UI thread drains the mailbox once a frame (the view-model's pump()), so view-model
// state is only ever touched on the UI thread. Tests use InlineExecutor: the job runs inside the call that
// starts it, and the next pump() sees everything it posted.
namespace dq8::launcher {

class Executor {
  public:
    virtual ~Executor() = default;
    // Starts `job`. The caller starts a job only when busy() is false.
    virtual void run(std::function<void()> job) = 0;
    virtual bool busy() const noexcept = 0;
};

// One worker thread per job; the destructor waits for a running job (cancel it first).
class ThreadExecutor final : public Executor {
  public:
    ThreadExecutor() = default;
    ~ThreadExecutor() override;
    ThreadExecutor(const ThreadExecutor&) = delete;
    ThreadExecutor& operator=(const ThreadExecutor&) = delete;

    void run(std::function<void()> job) override;
    bool busy() const noexcept override { return busy_.load(std::memory_order_acquire); }

  private:
    std::thread thread_;
    std::atomic<bool> busy_{false};
};

class InlineExecutor final : public Executor {
  public:
    void run(std::function<void()> job) override { job(); }
    bool busy() const noexcept override { return false; }
};

using ExecutorFactory = std::function<std::unique_ptr<Executor>()>;
ExecutorFactory thread_executors();
ExecutorFactory inline_executors();

// Messages from a worker to the UI thread, in order.
template <class T> class Mailbox {
  public:
    void post(T message) {
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::move(message));
    }
    std::vector<T> take() {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> out;
        out.swap(messages_);
        return out;
    }

  private:
    std::mutex mutex_;
    std::vector<T> messages_;
};

} // namespace dq8::launcher
