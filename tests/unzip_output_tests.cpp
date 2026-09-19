#include "archive/UnzipOutput.h"

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

class Directory {
public:
  Directory() {
    static std::atomic_uint sequence = 0;
    path = std::filesystem::temp_directory_path() /
        ("asobmashow-unzip-output-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    require(std::filesystem::create_directory(path), "private output directory is created");
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::shared_ptr<std::ofstream> output(const std::filesystem::path &path) {
  auto result = std::make_shared<std::ofstream>(path, std::ios::binary);
  require(result->good(), "private output file opens");
  return result;
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "output can be read");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

archive_file::UnzipExecutionPlan asynchronousPlan() {
  return {.workersPerArchive = 2, .memoryPerArchive = 512 * 1024};
}

void testSharedByteBudgetAndPendingReservationLifetime() {
  Directory directory;
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = 4,
                                             .maximumTotalBytes = 6,
                                             .reservedFreeBytes = 0}};
  std::ostringstream bytes;
  {
    archive_file::UnzipWriteGuard first(budget, directory.path);
    require(first.admit(4) && budget.writtenBytes == 0 && budget.pendingWriteBytes == 0,
            "admission checks do not consume expanded-byte budget");
    require(first.write(bytes, "abc", 3), "first archive writes admitted bytes");
    require(first.write(bytes, "d", 1), "exact per-archive limit is allowed");
    require(budget.writtenBytes == 4 && budget.pendingWriteBytes == 1,
            "each write retires the prior pending disk reservation");
    {
      archive_file::UnzipWriteGuard second(budget, directory.path);
      require(second.write(bytes, "ef", 2), "second archive can reach exact total limit");
      require(budget.writtenBytes == 6 && budget.pendingWriteBytes == 3,
              "pending reservations are shared across archive guards");
      require(!second.write(bytes, "g", 1), "aggregate byte limit rejects later output");
      require(second.error() == "Unzip expanded-byte limit exceeded. Original archive kept.",
              "expanded-byte failure keeps its existing diagnostic");
      require(budget.pendingWriteBytes == 1, "failed consume retires only its own prior reservation");
    }
    require(!first.admit(0) && budget.exhausted, "budget exhaustion is shared and sticky");
  }
  require(budget.pendingWriteBytes == 0 && bytes.str() == "abcdef",
          "guard destruction releases reservations without undoing written-byte accounting");
}

void testEntryLimitsAndFirstFailureAreShared() {
  Directory directory;
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveEntries = 3, .maximumTotalEntries = 5}};
  archive_file::UnzipWriteGuard first(budget, directory.path), second(budget, directory.path);
  require(first.admitEntries(3) && second.admitEntries(2), "entry admission reaches exact shared limit");
  require(!second.admitEntries(1) && budget.admittedEntries == 5, "rejected entries do not consume budget");
  const auto failure = budget.failureMessage;
  require(failure == "Unzip entry-count limit exceeded. Original archive kept.", "entry diagnostic is preserved");
  require(!first.admit(1) && first.error() == failure, "later failures retain the first shared diagnostic");
  archive_file::UnzipBudget perArchive{.limits = {.maximumArchiveEntries = 2}};
  archive_file::UnzipWriteGuard limited(perArchive, directory.path);
  require(!limited.admitEntries(3) && perArchive.admittedEntries == 0, "per-archive entry limit is enforced");
  archive_file::UnzipBudget explicitBudget;
  archive_file::UnzipWriteGuard explicitGuard(explicitBudget, directory.path);
  require(!explicitGuard.rejectEntryLimit() && explicitBudget.exhausted, "backend entry-limit rejection exhausts the shared budget");
}

void testSpaceChecksAndFailedStreamAccounting() {
  Directory directory;
  archive_file::UnzipBudget unavailable;
  archive_file::UnzipWriteGuard missing(unavailable, directory.path / "missing");
  require(!missing.admit(1) && missing.error() == "Could not check unzip free-space. Original archive kept.",
          "an unavailable destination fails closed before writing");
  archive_file::UnzipBudget reserved{.limits = {.reservedFreeBytes = std::numeric_limits<std::uint64_t>::max()}};
  archive_file::UnzipWriteGuard full(reserved, directory.path);
  require(!full.admit(1) && full.error() == "Unzip reserved free-space limit reached. Original archive kept.",
          "free-space reservation failure is preserved");
  archive_file::UnzipBudget failed{.limits = {.reservedFreeBytes = 0}};
  {
    archive_file::UnzipWriteGuard guard(failed, directory.path);
    std::ofstream unopened;
    require(!guard.write(unopened, "x", 1), "an unwritable stream fails the write");
    require(failed.writtenBytes == 1 && failed.pendingWriteBytes == 1 && !failed.exhausted,
            "write accounting retains the existing admitted-byte semantics on stream failure");
  }
  require(failed.pendingWriteBytes == 0, "failed stream reservations are released by the guard");
}

void testCheckpointPreservesCancellationAndPauseOrder() {
  std::stop_source source;
  auto token = source.get_token();
  int pauses = 0;
  std::string error;
  const auto pause = [&] { ++pauses; return false; };
  require(archive_file::unzipCheckpoint(&token, {}, &error), "empty pause callback permits work");
  require(!archive_file::unzipCheckpoint(&token, pause, &error) && pauses == 1 && error == "Unzip cancelled",
          "pause rejection is reported as cancellation");
  source.request_stop();
  require(!archive_file::unzipCheckpoint(&token, pause, &error) && pauses == 1,
          "requested stop short-circuits the pause callback");
}

void testInlineThresholdAndAsynchronousFileOwnership() {
  Directory directory;
  archive_file::UnzipBudget budget{.limits = {.reservedFreeBytes = 0}};
  archive_file::UnzipWriteGuard guard(budget, directory.path);
  for (const auto plan : {archive_file::UnzipExecutionPlan{.workersPerArchive = 1, .memoryPerArchive = 1024 * 1024},
                          archive_file::UnzipExecutionPlan{.workersPerArchive = 2, .memoryPerArchive = 512 * 1024 - 1}}) {
    int checkpoints = 0;
    archive_file::UnzipOutputPipeline pipeline(guard, plan, nullptr, [&] { ++checkpoints; return true; });
    require(!pipeline.enabled() && pipeline.bufferBudget() == 0, "small plans use synchronous writes");
    auto file = output(directory.path / "inline");
    require(pipeline.write(file, "direct", 6) && read(directory.path / "inline") == "direct",
            "inline output is written and flushed immediately");
    require(pipeline.flush() && checkpoints == 0, "inline checkpoint ownership remains with the backend caller");
  }
  archive_file::UnzipOutputPipeline pipeline(guard, asynchronousPlan(), nullptr, {});
  require(pipeline.enabled() && pipeline.bufferBudget() == 64 * 1024, "parallel plan reserves bounded buffering");
  auto first = output(directory.path / "first");
  auto second = output(directory.path / "second");
  std::weak_ptr<std::ofstream> retained = first;
  require(pipeline.write(first, "one", 3) && pipeline.write(second, "two", 3),
          "switching output files stages each file's own bytes");
  first.reset();
  second.reset();
  require(pipeline.flush() && retained.expired(), "flush completes writes and releases queued output ownership");
  require(read(directory.path / "first") == "one" && read(directory.path / "second") == "two",
          "file switching preserves output identity and content");
}

void testWorkerFailureAndCancellationReleaseQueuedOutput() {
  Directory directory;
  for (int failure = 0; failure < 4; ++failure) {
    archive_file::UnzipBudget budget{.limits = {.reservedFreeBytes = 0}};
    archive_file::UnzipWriteGuard guard(budget, directory.path);
    std::stop_source source;
    if (failure == 0) source.request_stop();
    auto token = source.get_token();
    archive_file::UnzipOutputPipeline pipeline(guard, asynchronousPlan(), &token, [&] {
      if (failure == 2) throw std::runtime_error("pause failed");
      return failure != 1;
    });
    auto file = failure == 3 ? std::make_shared<std::ofstream>() : output(directory.path / "failed");
    std::weak_ptr<std::ofstream> retained = file;
    require(pipeline.write(file, "pending", 7), "small write is staged before worker admission");
    file.reset();
    require(!pipeline.flush() && !pipeline.flush(), "worker failure is sticky and repeat flush does not hang");
    require(pipeline.cancelled() == (failure <= 1), "stop/pause cancellation remains distinct from exceptions and write errors");
    require(retained.expired(), "failure releases retained output streams");
    require(budget.writtenBytes == (failure == 3 ? 7 : 0), "cancelled or throwing checkpoints never consume byte budget");
  }
}

void testBackpressureAndDestructorJoin() {
  Directory directory;
  archive_file::UnzipBudget budget{.limits = {.reservedFreeBytes = 0}};
  archive_file::UnzipWriteGuard guard(budget, directory.path);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  auto pipeline = std::make_unique<archive_file::UnzipOutputPipeline>(guard, asynchronousPlan(), nullptr, [&] {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    return changed.wait_for(lock, 5s, [&] { return release; });
  });
  auto file = output(directory.path / "bounded");
  const std::string chunk(32 * 1024, 'x');
  require(pipeline->write(file, chunk.data(), chunk.size()), "initial chunk reaches the output worker");
  {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 5s, [&] { return entered; }), "worker reaches its checkpoint");
  }
  std::promise<void> started;
  auto producer = std::async(std::launch::async, [&] {
    started.set_value();
    return pipeline->write(file, chunk.data(), chunk.size());
  });
  started.get_future().wait();
  const bool blocked = producer.wait_for(20ms) == std::future_status::timeout;
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  require(producer.get() && blocked && pipeline->flush(), "bounded output applies producer backpressure until the writer advances");
  require(read(directory.path / "bounded") == chunk + chunk, "backpressure preserves every byte");

  {
    std::lock_guard lock(mutex);
    entered = release = false;
  }
  require(pipeline->write(file, "tail", 4), "partial final chunk remains staged");
  file.reset();
  auto destroy = std::async(std::launch::async, [owned = std::move(pipeline)]() mutable { owned.reset(); });
  {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 5s, [&] { return entered; }), "destruction flushes the staged tail");
  }
  const bool joined = destroy.wait_for(20ms) == std::future_status::timeout;
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  destroy.get();
  require(joined && read(directory.path / "bounded") == chunk + chunk + "tail",
          "destruction retains output and joins the writer before returning");
}
} // namespace

int main() {
  try {
    testSharedByteBudgetAndPendingReservationLifetime();
    testEntryLimitsAndFirstFailureAreShared();
    testSpaceChecksAndFailedStreamAccounting();
    testCheckpointPreservesCancellationAndPauseOrder();
    testInlineThresholdAndAsynchronousFileOwnership();
    testWorkerFailureAndCancellationReleaseQueuedOutput();
    testBackpressureAndDestructorJoin();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
