#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chart_scan {

class WorkScheduler {
public:
  using Work = std::function<void()>;

  explicit WorkScheduler(std::size_t workerCount);
  ~WorkScheduler();
  WorkScheduler(const WorkScheduler &) = delete;
  WorkScheduler &operator=(const WorkScheduler &) = delete;

  bool enqueue(Work work);
  void finish();
  void cancel();
  std::vector<std::exception_ptr> takeExceptions();

private:
  void workerLoop();
  void joinWorkers();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Work> queue_;
  std::vector<std::thread> workers_;
  std::vector<std::exception_ptr> exceptions_;
  bool closed_ = false;
  bool cancelled_ = false;
};

} // namespace chart_scan
