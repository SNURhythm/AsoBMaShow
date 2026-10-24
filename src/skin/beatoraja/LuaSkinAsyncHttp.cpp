#include "LuaSkinAsyncHttp.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace skin {
struct LuaSkinAsyncHttp::State {
  std::unordered_map<int, std::shared_ptr<Job>> jobs;
  std::size_t reserved = 0;
  int nextId = 0;
  bool disposed = false;
};
struct LuaSkinAsyncHttp::Job {
  std::shared_ptr<State> owner;
  std::unique_ptr<LuaSkinHttpConnection> connection;
  std::stop_source stop;
  std::chrono::steady_clock::time_point deadline;
  std::optional<LuaSkinAsyncHttpResult> result;
  std::size_t reserved = 0;
  std::size_t maximumBytes = 0;
  bool started = false;
  bool finished = false;
  bool discarded = false;

  void shrink(std::size_t count) {
    owner->reserved -= reserved - count;
    reserved = count;
  }
  void expire() {
    if (result || finished) return;
    result = LuaSkinAsyncHttpResult{.status = "failed", .error = "HTTP request timed out"};
    stop.request_stop();
    if (!started) shrink(0);
  }
  void discard() {
    discarded = true;
    stop.request_stop();
    result.reset();
    if (!started || finished) shrink(0);
  }
};

struct LuaSkinAsyncHttp::Workers {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<Job>> queue;
  std::array<std::shared_ptr<Job>, 2> active;
  bool stopping = false;
  std::array<std::thread, 2> threads;
  std::thread timer;

  Workers() {
    try {
      for (std::size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([this, i] { work(i); });
      timer = std::thread([this] { deadlines(); });
    } catch (...) {
      { std::lock_guard lock(mutex); stopping = true; }
      changed.notify_all();
      for (auto &thread : threads) if (thread.joinable()) thread.join();
      throw;
    }
  }
  ~Workers() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      for (auto &job : queue) job->discard();
      queue.clear();
      for (auto &job : active) if (job) job->stop.request_stop();
    }
    changed.notify_all();
    for (auto &thread : threads) thread.join();
    timer.join();
  }
  void remove(const std::shared_ptr<Job> &job) {
    std::erase(queue, job);
    changed.notify_all();
  }
  void deadlines() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      const auto now = std::chrono::steady_clock::now();
      auto next = std::chrono::steady_clock::time_point::max();
      for (auto it = queue.begin(); it != queue.end();) {
        auto &job = *it;
        if (now >= job->deadline) {
          job->expire();
          it = queue.erase(it);
        } else { next = std::min(next, job->deadline); ++it; }
      }
      for (auto &job : active) {
        if (!job || job->result || job->discarded) continue;
        if (now >= job->deadline) job->expire();
        else next = std::min(next, job->deadline);
      }
      if (next == std::chrono::steady_clock::time_point::max()) changed.wait(lock);
      else changed.wait_until(lock, next);
    }
  }
  static LuaSkinAsyncHttpResult download(Job &job) {
    auto &connection = *job.connection;
    if (auto failure = connection.connect()) return {.status = "failed", .error = *failure};
    auto code = connection.responseCode();
    if (code.failure || !code.code)
      return {.status = "failed", .error = code.failure.value_or("HTTP response code unavailable")};
    if (*code.code < 200 || *code.code >= 300)
      return {.status = "failed", .statusCode = *code.code, .error = "HTTP status " + std::to_string(*code.code)};
    auto body = connection.readBody();
    if (body.failure || !body.body)
      return {.status = "failed", .error = body.failure.value_or("HTTP response body unavailable")};
    if (body.body->size() > job.maximumBytes)
      return {.status = "failed", .error = "response is too large"};
    return {.status = "completed", .statusCode = *code.code, .body = std::move(*body.body)};
  }
  void work(std::size_t slot) {
    for (;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping) return;
        job = std::move(queue.front()); queue.pop_front();
        if (std::chrono::steady_clock::now() >= job->deadline) { job->expire(); continue; }
        job->started = true;
        active[slot] = job;
        changed.notify_all();
      }
      LuaSkinAsyncHttpResult value;
      try { value = download(*job); }
      catch (const std::exception &error) { value = {.status = "failed", .error = error.what()}; }
      catch (...) { value = {.status = "failed", .error = "HTTP worker failed"}; }
      job->connection->disconnect();
      job->connection.reset();
      {
        std::lock_guard lock(mutex);
        if (std::chrono::steady_clock::now() >= job->deadline) job->expire();
        job->finished = true;
        if (!job->discarded && !job->result) {
          job->result = std::move(value);
          job->shrink(job->result->body.size());
        } else { job->shrink(0); }
        active[slot].reset();
      }
      changed.notify_all();
    }
  }
};

LuaSkinAsyncHttp::Workers &LuaSkinAsyncHttp::workers() {
  static Workers value;
  return value;
}
LuaSkinAsyncHttp::LuaSkinAsyncHttp(LuaSkinHttpTransport *transport)
    : transport_(transport), state_(std::make_shared<State>()) { (void)workers(); }
LuaSkinAsyncHttp::~LuaSkinAsyncHttp() { dispose(); }

LuaSkinAsyncHttpSubmitResult LuaSkinAsyncHttp::submit(
    std::string_view url, int timeoutMilliseconds, std::size_t maxBytes) noexcept {
  try {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(LuaSkinHttpClient::clampTimeout(timeoutMilliseconds));
    if (auto failure = LuaSkinHttpClient::validateAsyncUrl(url)) return {.failure = *failure};
    if (maxBytes < 1 || maxBytes > LuaSkinHttpClient::maximumBytes)
      return {.failure = "HTTP max_bytes must be between 1 and 8388608"};
    auto &pool = workers();
    std::lock_guard lock(pool.mutex);
    if (state_->disposed || state_->jobs.size() >= 16)
      return {.failure = "HTTP request limit reached or skin disposed"};
    if (maxBytes > 16 * 1024 * 1024 - state_->reserved)
      return {.failure = "HTTP retained response capacity exceeded"};
    if (pool.queue.size() >= 32) return {.failure = "HTTP queue full"};
    if (!transport_) return {.failure = "HTTP transport is unavailable"};
    auto job = std::make_shared<Job>();
    job->owner = state_;
    job->deadline = deadline;
    job->maximumBytes = maxBytes;
    auto opened = transport_->open(url, LuaSkinHttpClient::clampTimeout(timeoutMilliseconds),
        {.maximumBytes = maxBytes, .requestStop = job->stop.get_token(),
         .deadline = deadline, .restrictedDestinations = true});
    if (opened.failure || !opened.connection)
      return {.failure = opened.failure.value_or("HTTP transport returned no connection")};
    job->connection = std::move(opened.connection);
    do { state_->nextId = state_->nextId == std::numeric_limits<int>::max() ? 1 : state_->nextId + 1; }
    while (state_->jobs.contains(state_->nextId));
    const int id = state_->nextId;
    state_->jobs.emplace(id, job);
    try { pool.queue.push_back(job); }
    catch (...) { state_->jobs.erase(id); throw; }
    job->reserved = maxBytes;
    state_->reserved += maxBytes;
    pool.changed.notify_all();
    return {.id = id};
  } catch (const std::exception &error) { return {.failure = error.what()}; }
  catch (...) { return {.failure = "HTTP submission failed"}; }
}
LuaSkinAsyncHttpResult LuaSkinAsyncHttp::poll(int id) noexcept {
  auto &pool = workers();
  std::lock_guard lock(pool.mutex);
  const auto found = state_->jobs.find(id);
  if (found == state_->jobs.end()) return {.status = "failed", .error = "Unknown or consumed HTTP request"};
  const auto job = found->second;
  if (!job->result && std::chrono::steady_clock::now() >= job->deadline) {
    job->expire(); pool.remove(job);
  }
  if (!job->result) return {};
  auto result = std::move(*job->result);
  job->discard();
  state_->jobs.erase(found);
  return result;
}
bool LuaSkinAsyncHttp::cancel(int id) noexcept {
  auto &pool = workers();
  std::lock_guard lock(pool.mutex);
  const auto found = state_->jobs.find(id);
  if (found == state_->jobs.end()) return false;
  auto job = found->second;
  job->discard(); pool.remove(job); state_->jobs.erase(found);
  return true;
}
void LuaSkinAsyncHttp::dispose() noexcept {
  auto &pool = workers();
  std::lock_guard lock(pool.mutex);
  state_->disposed = true;
  for (auto &[id, job] : state_->jobs) { job->discard(); pool.remove(job); }
  state_->jobs.clear();
}
std::size_t LuaSkinAsyncHttp::reservedBytes() const noexcept {
  std::lock_guard lock(workers().mutex);
  return state_->reserved;
}
} // namespace skin
