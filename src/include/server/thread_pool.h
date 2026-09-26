#ifndef ARGONSQL_THREAD_POOL_H
#define ARGONSQL_THREAD_POOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * 一个固定大小的通用线程池。
 *
 * accept 线程只负责接收连接并把任务放入队列，工作线程负责执行任务。
 * 这样可以避免每个连接都创建一个不可控的 detached 线程，也便于 Server
 * 关闭时等待所有任务完成。
 */
class ThreadPool {
 public:
  explicit ThreadPool(size_t worker_count);
  ~ThreadPool();

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  /**
   * 投递任务。
   *
   * 返回 false 表示线程池已经停止，调用方仍然拥有未投递成功的资源，
   * 需要自行关闭对应的 client fd。
   */
  bool Submit(std::function<void()> task);

  /**
   * 停止接收新任务，等待队列中的任务和工作线程全部退出。
   * Shutdown 可以重复调用。
   */
  void Shutdown();

 private:
  void WorkerLoop();

  std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_{false};
};

#endif  // ARGONSQL_THREAD_POOL_H
