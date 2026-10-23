// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/executor.h"

namespace dq8::launcher {

ThreadExecutor::~ThreadExecutor() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ThreadExecutor::run(std::function<void()> job) {
    if (thread_.joinable()) {
        thread_.join(); // the previous job has finished (busy() was false) or is about to
    }
    busy_.store(true, std::memory_order_release);
    thread_ = std::thread([this, job = std::move(job)] {
        job();
        busy_.store(false, std::memory_order_release);
    });
}

ExecutorFactory thread_executors() {
    return [] { return std::make_unique<ThreadExecutor>(); };
}

ExecutorFactory inline_executors() {
    return [] { return std::make_unique<InlineExecutor>(); };
}

} // namespace dq8::launcher
