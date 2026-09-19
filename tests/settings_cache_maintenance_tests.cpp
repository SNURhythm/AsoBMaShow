#include "scene/SettingsCacheMaintenance.h"
#include "archive/TemporaryCache.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <stdexcept>

using namespace std::chrono_literals;
using Maintenance = SettingsCacheMaintenance;

namespace {
struct Gate {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  void block() {
    entered.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
  }
  void wait() { assert(entered.get_future().wait_for(5s) == std::future_status::ready); }
};

struct CacheFixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("asobmashow-settings-cache-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  archive_file::TemporaryCache cache;
  CacheFixture() { assert(std::filesystem::create_directory(root)); }
  ~CacheFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  void write(const char *name, const char *bytes) {
    std::ofstream file(root / name); file << bytes; file.close(); assert(file.good());
  }
  Maintenance::Cleanup cleanup() {
    return [this](auto &result, auto &error) {
      return cache.cleanup(root, result, {root / "active.mov"},
          [](const auto &path) { return path.lexically_normal().generic_string(); }, &error);
    };
  }
  Maintenance::Measure measure() {
    return [this](auto &result, auto &error, const auto &token) {
      return cache.measure(root, result, &error, &token);
    };
  }
};

void waitIdle(Maintenance &jobs) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (jobs.running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(!jobs.running());
}

void testRealCacheMeasurementCleanupAndRepeatedRequests() {
  CacheFixture fixture;
  fixture.write("active.mov", "keep");
  fixture.write("unused.mov", "remove");
  Maintenance jobs(fixture.cleanup(), fixture.measure());
  assert(jobs.startMeasure());
  waitIdle(jobs);
  auto result = jobs.takeCompletion();
  assert(result && result->operation == Maintenance::Operation::Measure && result->succeeded);
  assert(result->usage.entries == 2 && result->usage.bytes == 10);
  assert(!jobs.takeCompletion());
  assert(jobs.startCleanup());
  waitIdle(jobs);
  result = jobs.takeCompletion();
  assert(result && result->operation == Maintenance::Operation::Cleanup && result->succeeded);
  assert(result->cleanup.removedEntries == 1 && result->cleanup.removedBytes == 6);
  assert(result->cleanup.skippedEntries == 1);
  assert(std::filesystem::exists(fixture.root / "active.mov"));
  assert(!std::filesystem::exists(fixture.root / "unused.mov"));
  assert(jobs.startCleanup());
  waitIdle(jobs);
  result = jobs.takeCompletion();
  assert(result && result->cleanup.removedEntries == 0 && result->cleanup.skippedEntries == 1);
}

// Removing admission checks would start duplicate jobs or let measurement
// supersede an active cleanup. Removing the generation guard publishes stale usage.
void testOverlapRejectsDuplicatesAndDiscardsLateMeasurement() {
  CacheFixture fixture;
  fixture.write("unused.mov", "remove");
  Gate measure, cleanup;
  Maintenance jobs([&](auto &result, auto &error) {
    cleanup.block(); return fixture.cleanup()(result, error);
  }, [&](auto &result, auto &error, const auto &token) {
    measure.block(); return fixture.measure()(result, error, token);
  });
  assert(jobs.startMeasure());
  measure.wait();
  assert(!jobs.startMeasure());
  assert(jobs.startCleanup());
  cleanup.wait();
  assert(jobs.cleanupRunning() && jobs.running());
  assert(!jobs.startCleanup() && !jobs.startMeasure());
  cleanup.release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (jobs.cleanupRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(!jobs.cleanupRunning() && jobs.running());
  // The old measurement must finish after cleanup has queued its result.
  measure.release.set_value();
  waitIdle(jobs);
  const auto result = jobs.takeCompletion();
  assert(result && result->operation == Maintenance::Operation::Cleanup);
  assert(result->succeeded && result->cleanup.removedEntries == 1);
  assert(!jobs.takeCompletion());
}

void testNewRequestDiscardsAnAlreadyQueuedCompletion() {
  CacheFixture fixture;
  Gate cleanup;
  Maintenance jobs([&](auto &result, auto &error) {
    cleanup.block(); return fixture.cleanup()(result, error);
  }, fixture.measure());
  assert(jobs.startMeasure());
  waitIdle(jobs);
  assert(jobs.startCleanup());
  cleanup.wait();
  assert(!jobs.takeCompletion());
  cleanup.release.set_value();
  waitIdle(jobs);
  assert(jobs.takeCompletion()->operation == Maintenance::Operation::Cleanup);
}

void testFilesystemFailuresAreDeliveredForBothOperations() {
  CacheFixture fixture;
  // An oversized single path component fails without requiring filesystem privileges.
  const auto invalidRoot = fixture.root / std::string(4096, 'x');
  Maintenance jobs([&](auto &result, auto &error) {
    return fixture.cache.cleanup(invalidRoot, result, {},
        [](const auto &path) { return path.generic_string(); }, &error);
  }, [&](auto &result, auto &error, const auto &token) {
    return fixture.cache.measure(invalidRoot, result, &error, &token);
  });
  assert(jobs.startMeasure());
  waitIdle(jobs);
  auto result = jobs.takeCompletion();
  assert(result && !result->succeeded && !result->error.empty());
  assert(result->operation == Maintenance::Operation::Measure);
  assert(jobs.startCleanup());
  waitIdle(jobs);
  result = jobs.takeCompletion();
  assert(result && !result->succeeded && !result->error.empty());
  assert(result->operation == Maintenance::Operation::Cleanup);
}

void testOperationExceptionsAreDeliveredAndAllowRetry() {
  for (const bool cleanup : {false, true}) {
    for (int failure = 0; failure < 3; ++failure) {
      CacheFixture fixture;
      fixture.write("unused.mov", "remove");
      bool fail = true;
      auto throwFailure = [&] {
        if (!fail) return;
        if (failure == 0) throw std::runtime_error("cache callback failure");
        if (failure == 1) throw std::runtime_error("");
        throw 7;
      };
      Maintenance jobs([&](auto &result, auto &error) {
        throwFailure();
        return fixture.cleanup()(result, error);
      }, [&](auto &result, auto &error, const auto &token) {
        throwFailure();
        return fixture.measure()(result, error, token);
      });
      assert(cleanup ? jobs.startCleanup() : jobs.startMeasure());
      waitIdle(jobs);
      auto result = jobs.takeCompletion();
      assert(result && !result->succeeded);
      assert(result->operation == (cleanup ? Maintenance::Operation::Cleanup
                                          : Maintenance::Operation::Measure));
      assert(result->error == (failure == 0 ? "cache callback failure"
                                           : "Unknown archive cache error"));
      assert(!jobs.cleanupRunning() && !jobs.takeCompletion());
      fail = false;
      assert(cleanup ? jobs.startCleanup() : jobs.startMeasure());
      waitIdle(jobs);
      result = jobs.takeCompletion();
      assert(result && result->succeeded && result->error.empty());
      assert(cleanup ? result->cleanup.removedBytes == 6
                     : result->usage.bytes == 6);
    }
  }
}

void testSupersededAndStoppedExceptionsAreDiscarded() {
  CacheFixture fixture;
  Gate measure;
  Maintenance superseded(fixture.cleanup(), [&](auto &, auto &, const auto &) -> bool {
    measure.block();
    throw std::runtime_error("stale measurement");
  });
  assert(superseded.startMeasure());
  measure.wait();
  assert(superseded.startCleanup());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (superseded.cleanupRunning() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(!superseded.cleanupRunning());
  measure.release.set_value();
  waitIdle(superseded);
  const auto result = superseded.takeCompletion();
  assert(result && result->operation == Maintenance::Operation::Cleanup);
  assert(result->succeeded && !superseded.takeCompletion());

  std::promise<void> entered;
  Maintenance stopped(fixture.cleanup(), [&](auto &, auto &, const auto &token) -> bool {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    entered.set_value();
    changed.wait(lock, token, [] { return false; });
    throw std::runtime_error("stopped measurement");
  });
  assert(stopped.startMeasure());
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  stopped.stopAndWait();
  assert(!stopped.running() && !stopped.takeCompletion());
}

void testStopSignalsMeasurementAndAllowsRestart() {
  CacheFixture fixture;
  std::promise<void> entered;
  bool sawStop = false;
  Maintenance jobs(fixture.cleanup(), [&](auto &result, auto &error, const auto &token) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    entered.set_value();
    changed.wait(lock, token, [] { return false; });
    sawStop = token.stop_requested();
    return fixture.measure()(result, error, token);
  });
  assert(jobs.startMeasure());
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  jobs.stopAndWait();
  assert(sawStop && !jobs.running() && !jobs.takeCompletion());
  jobs.stopAndWait();
  assert(jobs.startCleanup());
  waitIdle(jobs);
  assert(jobs.takeCompletion()->succeeded);
}

void testDestructionJoinsCleanupBeforeItsDependenciesDie() {
  CacheFixture fixture;
  fixture.write("unused.mov", "remove");
  Gate cleanup;
  auto jobs = std::make_unique<Maintenance>([&](auto &result, auto &error) {
    cleanup.block(); return fixture.cleanup()(result, error);
  }, fixture.measure());
  assert(jobs->startCleanup());
  cleanup.wait();
  std::promise<void> destroying;
  auto destroyed = std::async(std::launch::async, [&] {
    destroying.set_value(); jobs.reset();
  });
  assert(destroying.get_future().wait_for(5s) == std::future_status::ready);
  assert(destroyed.wait_for(30ms) == std::future_status::timeout);
  cleanup.release.set_value();
  assert(destroyed.wait_for(5s) == std::future_status::ready);
  destroyed.get();
  assert(!std::filesystem::exists(fixture.root / "unused.mov"));
}
} // namespace

#include "settings_cache_scene_fixture.h"

int main() {
  testSceneAppliesTypedResultsOnTheApplicationThread();
  testSceneShowsOperationErrorsAndHandlesAbsentViews();
  testSceneShowsThrownOperationErrorsAndAllowsRetry();
  testRealCacheMeasurementCleanupAndRepeatedRequests();
  testOverlapRejectsDuplicatesAndDiscardsLateMeasurement();
  testNewRequestDiscardsAnAlreadyQueuedCompletion();
  testFilesystemFailuresAreDeliveredForBothOperations();
  testOperationExceptionsAreDeliveredAndAllowRetry();
  testSupersededAndStoppedExceptionsAreDiscarded();
  testStopSignalsMeasurementAndAllowsRestart();
  testDestructionJoinsCleanupBeforeItsDependenciesDie();
}
