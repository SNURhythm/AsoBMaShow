#include "archive/TemporaryCache.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iterator>

using namespace std::chrono_literals;

namespace {
class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint sequence = 0;
    path = std::filesystem::temp_directory_path() /
        ("asobmashow-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    assert(std::filesystem::create_directory(path));
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::string pathKey(const std::filesystem::path &path) {
  return path.lexically_normal().generic_string();
}

void write(const std::filesystem::path &path, const std::string &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << bytes;
  output.close();
  assert(output.good());
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  assert(input.good());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void testMaterializationReusesBySizeAndReplacesDifferentSize() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  const auto output = root / "entry.wav";
  archive_file::TemporaryCache cache;
  std::string error;
  const auto resolve = [&] {
    assert(std::filesystem::is_directory(root));
    return output;
  };
  auto result = cache.materialize(root, resolve, {'a', 'b'}, &error);
  assert(result == output && read(output) == "ab");
  result = cache.materialize(root, resolve, {'x', 'y'}, &error);
  assert(result == output && read(output) == "ab");
  result = cache.materialize(root, resolve, {'x', 'y', 'z'}, &error);
  assert(result == output && read(output) == "xyz");
  result = cache.materialize(root, resolve, {}, &error);
  assert(result == output && std::filesystem::file_size(output) == 0);
}

void testCancelledMaterializationDoesNotCreateCacheOrResolveIdentity() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  archive_file::TemporaryCache cache;
  bool resolved = false;
  std::atomic_bool cancelled = true;
  std::string error;
  const auto result = cache.materialize(root, [&] {
    resolved = true;
    return root / "entry";
  }, {'x'}, &error, &cancelled);
  assert(!result && !resolved && !std::filesystem::exists(root));
  assert(error == "Materialize cancelled.");
}

void testCancellationAfterIdentityResolutionDoesNotWrite() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  const auto output = root / "entry";
  archive_file::TemporaryCache cache;
  std::atomic_bool cancelled = false;
  std::string error;
  const auto resolve = [&] {
    cancelled = true;
    return output;
  };
  assert(!cache.materialize(root, resolve, {'x'}, &error, &cancelled));
  assert(error == "Materialize cancelled." && !std::filesystem::exists(output));
  write(output, "existing");
  cancelled = false;
  assert(!cache.materialize(root, resolve, {'x'}, &error, &cancelled));
  assert(read(output) == "existing");
}

void testFilesystemErrorsPreserveExistingEntries() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  write(root, "not a directory");
  archive_file::TemporaryCache cache;
  bool resolved = false;
  std::string error;
  auto result = cache.materialize(root, [&] {
    resolved = true;
    return root / "entry";
  }, {'x'}, &error);
  assert(!result && !resolved && error.starts_with("Could not create archive cache:"));
  assert(read(root) == "not a directory");
  archive_file::TemporaryCacheCleanupResult cleaned;
  assert(!cache.cleanup(root, cleaned, {}, pathKey, &error));
  assert(cleaned.cacheExisted && cleaned.removedEntries == 0);
  assert(error.starts_with("Could not read archive cache:"));
  std::filesystem::remove(root);
  std::filesystem::create_directories(root / "entry");
  result = cache.materialize(root, [&] { return root / "entry"; }, {'x'}, &error);
  assert(!result && error.starts_with("Could not read cached archive entry size:"));
  assert(std::filesystem::is_directory(root / "entry"));
}

void testMeasurementAndProtectedCleanupCountNestedEntries() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  write(root / "active" / "video.mp4", "123");
  write(root / "unused" / "sound.wav", "45");
  write(root / "plain", "6");
  archive_file::TemporaryCache cache;
  archive_file::TemporaryCacheUsageResult usage;
  assert(cache.measure(root, usage));
  assert(usage.path == root && usage.cacheExisted && usage.bytes == 6 && usage.entries == 5);
  archive_file::TemporaryCacheCleanupResult cleaned;
  assert(cache.cleanup(root, cleaned, {root / "." / "active"}, pathKey));
  assert(cleaned.path == root && cleaned.cacheExisted);
  assert(cleaned.removedBytes == 3 && cleaned.removedEntries == 3 && cleaned.skippedEntries == 1);
  assert(read(root / "active" / "video.mp4") == "123");
  assert(!std::filesystem::exists(root / "unused") && !std::filesystem::exists(root / "plain"));
  assert(cache.cleanup(root, cleaned, {}, pathKey));
  assert(cleaned.removedBytes == 3 && cleaned.removedEntries == 2 && cleaned.skippedEntries == 0);
  assert(!std::filesystem::exists(root));
  assert(cache.cleanup(root, cleaned, {}, pathKey));
  assert(!cleaned.cacheExisted && cleaned.removedBytes == 0 && cleaned.removedEntries == 0);
}

void testCleanupUsesCurrentPathIdentityForProtectedEntries() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  write(root / "active.mp4", "video");
  write(root / "unused.wav", "audio");
  archive_file::TemporaryCache cache;
  archive_file::TemporaryCacheCleanupResult cleaned;
  const auto identify = [&](const std::filesystem::path &path) {
    return path.filename() == "alias.mp4" ? pathKey(root / "active.mp4") : pathKey(path);
  };
  assert(cache.cleanup(root, cleaned, {temporary.path / "alias.mp4"}, identify));
  assert(cleaned.skippedEntries == 1 && cleaned.removedEntries == 1 && cleaned.removedBytes == 5);
  assert(read(root / "active.mp4") == "video" && !std::filesystem::exists(root / "unused.wav"));
}

#ifndef _WIN32
void testLinkedTargetsDoNotContributeToCacheBytes() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  const auto external = temporary.path / "external";
  write(external / "data", "external bytes");
  write(root / "owned", "123");
  write(root / "nested" / "owned", "4567");
  const auto protectedLink = root / "file-link";
  std::filesystem::create_symlink(external / "data", protectedLink);
  std::filesystem::create_directory_symlink(external, root / "directory-link");
  std::filesystem::create_symlink(external / "data", root / "nested" / "file-link");
  std::filesystem::create_directory_symlink(external, root / "nested" / "directory-link");
  std::filesystem::create_symlink(external / "missing", root / "dangling-link");

  archive_file::TemporaryCache cache;
  archive_file::TemporaryCacheUsageResult usage;
  assert(cache.measure(root, usage));
  assert(usage.bytes == 7 && usage.entries == 8);
  const auto rootAlias = temporary.path / "cache-alias";
  std::filesystem::create_directory_symlink(root, rootAlias);
  assert(cache.measure(rootAlias, usage));
  assert(usage.path == rootAlias && usage.bytes == 7 && usage.entries == 8);
  archive_file::TemporaryCacheCleanupResult cleaned;
  assert(cache.cleanup(root, cleaned, {protectedLink}, pathKey));
  assert(cleaned.removedBytes == 7 && cleaned.removedEntries == 7);
  assert(cleaned.skippedEntries == 1 && std::filesystem::is_symlink(protectedLink));
  assert(read(external / "data") == "external bytes");
  assert(cache.measure(root, usage));
  assert(usage.bytes == 0 && usage.entries == 1);
  assert(cache.cleanup(root, cleaned, {}, pathKey));
  assert(cleaned.removedBytes == 0 && cleaned.removedEntries == 1);
  assert(!std::filesystem::exists(root));
  assert(read(external / "data") == "external bytes");
}
#endif

void testMeasurementCancellationAndMissingCache() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  archive_file::TemporaryCache cache;
  archive_file::TemporaryCacheUsageResult usage;
  std::stop_source source;
  source.request_stop();
  const auto stop = source.get_token();
  assert(cache.measure(root, usage, nullptr, &stop));
  assert(!usage.cacheExisted && usage.bytes == 0 && usage.entries == 0);
  write(root / "entry", "data");
  std::string error;
  assert(!cache.measure(root, usage, &error, &stop));
  assert(usage.cacheExisted && error == "Archive cache measurement cancelled.");
  assert(cache.measure(root / "entry", usage));
  assert(usage.cacheExisted && usage.bytes == 4 && usage.entries == 1);
}

void testMutationIsSerializedButMeasurementRemainsIndependent() {
  TempDirectory temporary;
  const auto root = temporary.path / "cache";
  const auto active = root / "active.mp4";
  write(active, "video");
  archive_file::TemporaryCache cache;
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  const auto identify = [&](const std::filesystem::path &path) {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    assert(changed.wait_for(lock, 5s, [&] { return release; }));
    return pathKey(path);
  };
  auto cleanup = std::async(std::launch::async, [&] {
    archive_file::TemporaryCacheCleanupResult result;
    return cache.cleanup(root, result, {active}, identify);
  });
  {
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return entered; }));
  }
  auto measure = std::async(std::launch::async, [&] {
    archive_file::TemporaryCacheUsageResult result;
    return cache.measure(root, result) && result.bytes == 5 && result.entries == 1;
  });
  auto materialize = std::async(std::launch::async, [&] {
    return cache.materialize(root, [&] { return root / "new.wav"; }, {'a'});
  });
  const bool measuredPromptly = measure.wait_for(500ms) == std::future_status::ready;
  const bool writeWaited = materialize.wait_for(20ms) == std::future_status::timeout;
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  assert(measuredPromptly && measure.get());
  assert(writeWaited && cleanup.get());
  assert(materialize.get() == root / "new.wav" && read(active) == "video");
}
} // namespace

int main() {
  testMaterializationReusesBySizeAndReplacesDifferentSize();
  testCancelledMaterializationDoesNotCreateCacheOrResolveIdentity();
  testCancellationAfterIdentityResolutionDoesNotWrite();
  testFilesystemErrorsPreserveExistingEntries();
  testMeasurementAndProtectedCleanupCountNestedEntries();
  testCleanupUsesCurrentPathIdentityForProtectedEntries();
#ifndef _WIN32
  testLinkedTargetsDoNotContributeToCacheBytes();
#endif
  testMeasurementCancellationAndMissingCache();
  testMutationIsSerializedButMeasurementRemainsIndependent();
}
