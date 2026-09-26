#include "server/thread_pool.h"

#include <stdexcept>

ThreadPool::ThreadPool(size_t worker_count) {
  if (worker_count == 0) {
    throw std::invalid_argument("ThreadPool requires at least one worker.");
  }

  workers_.reserve(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back(&ThreadPool::WorkerLoop, this);
  }
}

ThreadPool::~ThreadPool() {
  Shutdown();
}

bool ThreadPool::Submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return false;
    }
    tasks_.push(std::move(task));
  }
  condition_.notify_one();
  return true;
}

void ThreadPool::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      // 即使已经停止，也继续执行下面的 join 检查，保证 Shutdown 在
      // 多次调用时仍然能够等待尚未结束的 worker。
    }
    stopping_ = true;
  }
  condition_.notify_all();

  for (std::thread &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void ThreadPool::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

      // stopping_ 后仍然先清空任务队列，保证已经接收的客户端任务不会
      // 被静默丢弃；队列为空时该 worker 才退出。
      if (tasks_.empty() && stopping_) {
        return;
      }

      task = std::move(tasks_.front());
      tasks_.pop();
    }

    try {
      task();
    } catch (...) {
      // 单个客户端任务不能让 worker 线程退出，否则线程池容量会不断
      // 缩小。任务内部负责处理可预期错误，这里隔离未处理异常。
    }
  }
}
