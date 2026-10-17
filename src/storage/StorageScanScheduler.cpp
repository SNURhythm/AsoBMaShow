#include "StorageScanScheduler.h"
#include "../utils/ParallelWork.h"
#include "../ChartScanWorkScheduler.h"
#include "../Uuid.h"
#include "../sqlite3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace storage_management {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t batchRecords = 128, batchBytes = 256 * 1024;
std::size_t jobBytes(const StorageScanJob &job) {
  return sizeof(job) + (job.path.native().size() + job.accessRoot.native().size()) * sizeof(fs::path::value_type);
}
void add(std::uint64_t &to, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - to) throw std::overflow_error("Storage size overflow");
  to += value;
}
void add(StorageScanTotals &to, const StorageScanTotals &value) {
  add(to.bytes, value.bytes); add(to.audio, value.audio); add(to.video, value.video);
  add(to.image, value.image); add(to.files, value.files);
}
void add(StorageScanMetrics &to, const StorageScanMetrics &value) {
  add(to.entries, value.entries); add(to.metadataReads, value.metadataReads); add(to.rootIndexSteps, value.rootIndexSteps);
  add(to.archiveIndexVisits, value.archiveIndexVisits); add(to.archiveEntries, value.archiveEntries);
}
// The coordinator exclusively owns this expendable queue and its statements.
class PendingJobs {
public:
  explicit PendingJobs(const StorageScanLimits &limits) : limits_(limits) {}
  ~PendingJobs() {
    sqlite3_finalize(insert_); sqlite3_finalize(select_); sqlite3_finalize(remove_);
    if (database_) sqlite3_close(database_);
    if (!directory_.empty()) {
      std::error_code ignored;
      fs::remove(directory_ / "frontier.sqlite", ignored);
      fs::remove(directory_, ignored); // Keep unexpected files, even inside an owned directory.
    }
  }
  void push(StorageScanJob job) {
    const auto bytes = jobBytes(job);
    if (!database_ && (count_ >= limits_.frontierRecords || bytes > limits_.frontierBytes - std::min(bytes_, limits_.frontierBytes))) spill();
    if (database_) insert(job);
    else {
      bytes_ += bytes;
      memory_[static_cast<int>(job.kind)].push_back(std::move(job));
    }
    ++count_;
    if (limits_.metrics && !database_)
      limits_.metrics->peakFrontierRecords = std::max(limits_.metrics->peakFrontierRecords, count_);
  }
  bool empty() const { return count_ == 0; }
  StorageScanJob pop() {
    StorageScanJob job;
    if (database_) {
      bool found = false;
      for (int attempt = 0; attempt < 2 && !found; ++attempt) {
        sqlite3_reset(select_);
        sqlite3_bind_int(select_, 1, nextKind_);
        const int result = sqlite3_step(select_);
        if (result == SQLITE_ROW) {
          job.id = sqlite3_column_int64(select_, 0);
          job.parentId = sqlite3_column_int64(select_, 1);
          job.context = sqlite3_column_int64(select_, 2);
          job.kind = static_cast<StorageScanJobKind>(nextKind_);
          job.path = readPath(select_, 3); job.accessRoot = readPath(select_, 4);
          found = true;
        } else if (result != SQLITE_DONE) fail();
        nextKind_ = 1 - nextKind_;
      }
      sqlite3_reset(select_);
      if (!found) throw std::runtime_error("Missing pending scan job");
      sqlite3_reset(remove_);
      sqlite3_bind_int64(remove_, 1, static_cast<sqlite3_int64>(job.id));
      if (sqlite3_step(remove_) != SQLITE_DONE) fail();
      sqlite3_reset(remove_);
    } else {
      if (memory_[nextKind_].empty()) nextKind_ = 1 - nextKind_;
      job = std::move(memory_[nextKind_].front());
      memory_[nextKind_].pop_front();
      nextKind_ = 1 - nextKind_;
      bytes_ -= jobBytes(job);
    }
    --count_;
    return job;
  }
private:
  [[noreturn]] void fail() { throw std::runtime_error(database_ ? sqlite3_errmsg(database_) : "Cannot open scan frontier"); }
  static fs::path readPath(sqlite3_stmt *statement, int column) {
    const auto *data = static_cast<const char8_t *>(sqlite3_column_blob(statement, column));
    const auto size = sqlite3_column_bytes(statement, column);
    return size ? fs::path(std::u8string(data, data + size)) : fs::path{};
  }
  void bindPath(sqlite3_stmt *statement, int column, const fs::path &path) {
    const auto bytes = path.u8string();
    if (sqlite3_bind_blob64(statement, column, bytes.data(), bytes.size(), SQLITE_TRANSIENT) != SQLITE_OK) fail();
  }
  void insert(const StorageScanJob &job) {
    sqlite3_reset(insert_);
    sqlite3_bind_int64(insert_, 1, static_cast<sqlite3_int64>(job.id));
    sqlite3_bind_int64(insert_, 2, static_cast<sqlite3_int64>(job.parentId));
    sqlite3_bind_int64(insert_, 3, static_cast<sqlite3_int64>(job.context));
    sqlite3_bind_int(insert_, 4, static_cast<int>(job.kind));
    bindPath(insert_, 5, job.path); bindPath(insert_, 6, job.accessRoot);
    if (sqlite3_step(insert_) != SQLITE_DONE) fail();
    sqlite3_reset(insert_);
    if (limits_.metrics) ++limits_.metrics->spilledJobs;
  }
  void spill() {
    const auto root = limits_.scratchRoot.empty() ? fs::temp_directory_path() : limits_.scratchRoot;
    fs::create_directories(root);
    if (!fs::is_directory(fs::symlink_status(root))) throw std::runtime_error("Scan scratch directory is unavailable");
    const auto owned = root / ("storage-scan-" + uuid::generateV4());
    if (!fs::create_directory(owned)) throw std::runtime_error("Cannot create private scan directory");
    directory_ = owned;
    fs::permissions(directory_, fs::perms::owner_all);
    const auto path = (directory_ / "frontier.sqlite").u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char *>(path.c_str()), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) fail();
    if (sqlite3_exec(database_, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA cache_size=-8192;"
        "CREATE TABLE jobs(id INTEGER PRIMARY KEY,parent INTEGER,context INTEGER,kind INTEGER,path BLOB,root BLOB);"
        "CREATE INDEX jobs_kind ON jobs(kind,id); BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) fail();
    if (sqlite3_prepare_v2(database_, "INSERT INTO jobs VALUES(?1,?2,?3,?4,?5,?6)", -1, &insert_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(database_, "SELECT id,parent,context,path,root FROM jobs WHERE kind=?1 ORDER BY id LIMIT 1", -1, &select_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(database_, "DELETE FROM jobs WHERE id=?1", -1, &remove_, nullptr) != SQLITE_OK) fail();
    for (auto &queue : memory_) {
      while (!queue.empty()) { insert(queue.front()); queue.pop_front(); }
    }
    bytes_ = 0;
  }
  const StorageScanLimits &limits_;
  std::array<std::deque<StorageScanJob>, 2> memory_;
  std::size_t count_ = 0, bytes_ = 0;
  int nextKind_ = 0;
  fs::path directory_;
  sqlite3 *database_ = nullptr;
  sqlite3_stmt *insert_ = nullptr, *select_ = nullptr, *remove_ = nullptr;
};
struct Mailbox { std::size_t queued = 0; };
struct Event {
  StorageScanBatch batch;
  std::shared_ptr<Mailbox> mailbox;
  bool terminal = false;
};
struct Shared {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Event> events;
  std::atomic<bool> cancelled{false};
};
struct DirectoryState {
  StorageScanBatch own;
  StorageScanTotals descendants;
  StorageScanMetrics descendantMetrics;
  StorageDiagnostic descendantReason;
  std::size_t pending = 0;
  bool ended = false;
};
}

StorageScanLimits DefaultStorageScanLimits() {
#if defined(__ANDROID__) || TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const std::size_t workers = parallel_worker_count(std::numeric_limits<std::size_t>::max());
#else
  const auto hardware = std::thread::hardware_concurrency();
  const auto workers = std::min<std::size_t>(8, hardware ? hardware : 2);
#endif
  return {.workers = workers, .archiveBuilders = std::max<std::size_t>(1, workers / 2), .maxSubmitted = workers * 2};
}

bool RunStorageScan(std::span<const StorageScanJob> roots, StorageScanLimits limits,
                    ScanJobWork work, ScanBatchSink consume, Checkpoint check) {
  limits.workers = std::max<std::size_t>(1, limits.workers);
  limits.maxSubmitted = std::clamp(limits.maxSubmitted, limits.workers, 2 * limits.workers);
  Shared shared;
  const auto cancel = [&] { shared.cancelled.store(true); shared.changed.notify_all(); };
  std::stop_callback stop(limits.stopToken, cancel);
  const Checkpoint checkpoint = [&] {
    if (shared.cancelled.load()) return false;
    if (check && !check()) { cancel(); return false; }
    return !shared.cancelled.load();
  };
  std::unique_ptr<chart_scan::WorkScheduler> pool;
  try {
    if (!checkpoint()) return false;
    PendingJobs pending(limits);
    std::uint64_t nextId = 1;
    for (auto job : roots) { job.id = nextId++; job.parentId = 0; pending.push(std::move(job)); }
    pool = std::make_unique<chart_scan::WorkScheduler>(limits.workers, limits.archiveBuilders);
    std::unordered_map<std::uint64_t, DirectoryState> directories;
    std::size_t submitted = 0;
    const auto deliver = [&](StorageScanBatch batch) {
      if (consume && !consume(std::move(batch))) { cancel(); return false; }
      return true;
    };
    while (checkpoint() && (!pending.empty() || submitted)) {
      while (!pending.empty() && submitted < limits.maxSubmitted && checkpoint()) {
        auto job = pending.pop();
        directories[job.id].own.job = job;
        auto mailbox = std::make_shared<Mailbox>();
        const bool accepted = pool->enqueue([&, job, mailbox] {
          const auto send = [&](StorageScanBatch batch, bool terminal) {
            std::unique_lock lock(shared.mutex);
            shared.changed.wait(lock, [&] { return shared.cancelled.load() ||
                (mailbox->queued < 2 && shared.events.size() < 2 * limits.workers); });
            if (shared.cancelled.load()) return false;
            batch.job = job;
            shared.events.push_back({std::move(batch), mailbox, terminal});
            ++mailbox->queued;
            if (limits.metrics) limits.metrics->peakBufferedBatches =
                std::max(limits.metrics->peakBufferedBatches, shared.events.size());
            lock.unlock();
            shared.changed.notify_all();
            return true;
          };
          try {
            if (!checkpoint()) return;
            work(job, checkpoint, [&](StorageScanBatch batch) {
              // Bound each queued discovery batch even if a caller provides a larger vector.
              auto children = std::move(batch.children);
              std::size_t bytes = 0;
              for (auto &child : children) {
                const auto size = jobBytes(child);
                if (!batch.children.empty() && (batch.children.size() == batchRecords || bytes + size > batchBytes)) {
                  StorageScanBatch chunk;
                  chunk.children = std::move(batch.children);
                  if (!send(std::move(chunk), false)) return false;
                  bytes = 0;
                }
                bytes += size;
                batch.children.push_back(std::move(child));
              }
              return send(std::move(batch), false);
            });
            send({}, true);
          } catch (...) { cancel(); }
        }, job.kind == StorageScanJobKind::Archive ? chart_scan::WorkClass::ArchiveIndex : chart_scan::WorkClass::Cpu);
        if (!accepted) { cancel(); break; }
        ++submitted;
        if (limits.metrics) limits.metrics->peakSubmitted = std::max(limits.metrics->peakSubmitted, submitted);
      }
      if (!checkpoint()) break;
      Event event;
      {
        std::unique_lock lock(shared.mutex);
        shared.changed.wait(lock, [&] { return shared.cancelled.load() || !shared.events.empty(); });
        if (shared.cancelled.load()) break;
        event = std::move(shared.events.front()); shared.events.pop_front();
        --event.mailbox->queued;
      }
      shared.changed.notify_all();
      auto &state = directories.at(event.batch.job.id);
      for (auto &child : event.batch.children) {
        child.id = nextId++; child.parentId = event.batch.job.id;
        if (child.accessRoot.empty()) child.accessRoot = event.batch.job.accessRoot;
        child.context = event.batch.job.context;
        pending.push(std::move(child));
        ++state.pending;
      }
      event.batch.children.clear();
      add(state.own.totals, event.batch.totals);
      add(state.own.metrics, event.batch.metrics);
      if (event.batch.stamp.fileId) state.own.stamp = event.batch.stamp;
      if (event.batch.summary) state.own.summary = std::move(event.batch.summary);
      if (event.batch.reason.code != StorageReason::None) state.own.reason = std::move(event.batch.reason);
      if (!event.terminal) {
        if (!deliver(std::move(event.batch))) break;
        continue;
      }
      --submitted;
      state.ended = true;
      if (limits.metrics) ++limits.metrics->jobsFinished;
      auto own = state.own;
      own.directoryComplete = true;
      // Local metadata and recursive totals are separate events, regardless of
      // whether descendants happen to finish before their parent iterator.
      if (!deliver(std::move(own))) break;
      auto completed = event.batch.job.id;
      while (completed && checkpoint()) {
        auto found = directories.find(completed);
        if (found == directories.end() || !found->second.ended || found->second.pending) break;
        auto &done = found->second;
        auto result = done.own;
        result.directoryComplete = false;
        result.subtreeComplete = true;
        add(result.totals, done.descendants); add(result.metrics, done.descendantMetrics);
        if (result.reason.code == StorageReason::None) result.reason = done.descendantReason;
        const auto parent = result.job.parentId;
        if (parent) {
          auto &ancestor = directories.at(parent);
          add(ancestor.descendants, result.totals); add(ancestor.descendantMetrics, result.metrics);
          if (result.reason.code != StorageReason::None) ancestor.descendantReason = result.reason;
          --ancestor.pending;
        }
        if (!deliver(std::move(result))) break;
        directories.erase(found);
        completed = parent;
      }
    }
    if (shared.cancelled.load()) pool->cancel();
    else pool->finish();
    return !shared.cancelled.load() && pool->takeExceptions().empty();
  } catch (...) {
    cancel();
    if (pool) pool->cancel();
    return false;
  }
}
}
