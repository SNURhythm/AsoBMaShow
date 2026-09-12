#include "../src/ArchiveFile.h"
#include "../src/ArchiveRAII.h"
#include "../src/scene/play/GameplayBmsResourceAvailability.h"
#include "fixtures/archive/rar_fixtures.h"
#include "fixtures/archive/sevenzip_block_fixtures.h"

#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bounded_allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t largest = 0;
thread_local std::stop_source *cancelOnChunk = nullptr;
std::atomic_size_t observedEntrySize{0};
thread_local unsigned char *observedEntry = nullptr;
}

void *operator new(std::size_t size) {
  if (bounded_allocation_probe::enabled) {
    bounded_allocation_probe::largest =
        std::max(bounded_allocation_probe::largest, size);
  }
  if (bounded_allocation_probe::cancelOnChunk != nullptr && size >= 64 * 1024) {
    bounded_allocation_probe::cancelOnChunk->request_stop();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    if (size == bounded_allocation_probe::observedEntrySize.load()) {
      bounded_allocation_probe::observedEntry =
          static_cast<unsigned char *>(memory);
    }
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace std::chrono_literals;
using ArchiveEntryHandle =
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

// Generated with 7zz using `-m0=Delta:4 -m1=LZMA2 -mb0:1`. Embedding the
// tiny fixture keeps the regression independent of a system 7zz executable.
constexpr std::array<unsigned char, 226> kDeltaLzma2SevenZip = {
    0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x72, 0xa2, 0x57, 0xbc,
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0xcb, 0x4c, 0xd4, 0xe0, 0x00, 0xa8, 0x00,
    0x50, 0x5d, 0x00, 0x29, 0x12, 0x44, 0xeb, 0x89, 0x95, 0xd3, 0x41, 0x39,
    0x7f, 0x7e, 0xf0, 0x0a, 0x59, 0xf7, 0x56, 0x24, 0xc9, 0x9d, 0x5a, 0x1c,
    0x85, 0xb0, 0x38, 0x2f, 0xba, 0xd9, 0xcf, 0xf2, 0x74, 0xe8, 0x51, 0x65,
    0xe6, 0x62, 0x17, 0x4b, 0x8c, 0x7c, 0xc8, 0xd5, 0x6e, 0x77, 0x32, 0x73,
    0x65, 0x28, 0x64, 0x53, 0xd8, 0x39, 0x2d, 0x84, 0x45, 0xd4, 0x06, 0x7b,
    0xbd, 0x17, 0x30, 0x95, 0xdf, 0x9b, 0xc5, 0x50, 0xf7, 0x30, 0xb4, 0xf2,
    0x53, 0xf5, 0xc7, 0xed, 0x0e, 0x91, 0xad, 0xa6, 0xf4, 0xc0, 0x00, 0x00,
    0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x58, 0x00, 0x07, 0x0b, 0x01, 0x00,
    0x02, 0x21, 0x21, 0x01, 0x00, 0x21, 0x03, 0x01, 0x03, 0x01, 0x00, 0x0c,
    0x80, 0xa9, 0x80, 0xa9, 0x00, 0x08, 0x0a, 0x01, 0x5c, 0xea, 0xe2, 0xe7,
    0x00, 0x00, 0x05, 0x01, 0x19, 0x03, 0x00, 0x00, 0x00, 0x11, 0x21, 0x00,
    0x64, 0x00, 0x65, 0x00, 0x6c, 0x00, 0x74, 0x00, 0x61, 0x00, 0x2d, 0x00,
    0x73, 0x00, 0x6f, 0x00, 0x75, 0x00, 0x6e, 0x00, 0x64, 0x00, 0x2e, 0x00,
    0x77, 0x00, 0x61, 0x00, 0x76, 0x00, 0x00, 0x00, 0x19, 0x02, 0x00, 0x00,
    0x14, 0x0a, 0x01, 0x00, 0x85, 0xb9, 0x6b, 0x50, 0xc7, 0x20, 0xdd, 0x01,
    0x15, 0x06, 0x01, 0x00, 0x20, 0x80, 0xa4, 0x81, 0x00, 0x00,
};

constexpr std::string_view kDeltaSoundPayload =
    "RIFF delta-filter regression payload\n"
    "0123456789abcdef0123456789abcdef\n"
    "fedcba9876543210fedcba9876543210\n"
    "0123456789abcdef0123456789abcdef\n"
    "fedcba9876543210fedcba9876543210\n";
static_assert(kDeltaSoundPayload.size() == 169);

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-archive-concurrency-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void writeSevenZip(const std::filesystem::path &path,
                   const std::string &contents, bool multipleEntries = false) {
  auto writer = makeArchiveWriteHandle();
  assert(writer);
  assert(archive_write_set_format_7zip(writer.get()) == ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) ==
         ARCHIVE_OK);

  ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
  assert(entry);
  archive_entry_set_pathname(entry.get(), "readme.txt");
  archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0644);
  archive_entry_set_size(entry.get(),
                         static_cast<la_int64_t>(contents.size()));
  assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
  assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
         static_cast<la_ssize_t>(contents.size()));
  assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  if (multipleEntries) {
    archive_entry_set_pathname(entry.get(), "song/chart.bms");
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
}

void testSevenZipArchiveLevelSolidDetection() {
  TempDirectory temporary;
  const auto solidPath = temporary.path() / "solid.7z";
  const auto plainPath = temporary.path() / "plain.7z";
  const std::string contents = "#TITLE Solid archive\n#BPM 120\n";
  writeSevenZip(solidPath, contents, true);
  writeSevenZip(plainPath, contents);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(solidPath, entries, &error));
  assert(entries.size() == 2);
  assert(std::all_of(entries.begin(), entries.end(),
                     [](const auto &entry) { return entry.solid; }));
  const auto chartPath = archive_file::makeVirtualPath(solidPath, "song/chart.bms");
  assert(archive_file::isInSolidArchiveFolder(chartPath));
  assert(archive_file::sourcePreferenceForPath(chartPath).priority == 2);
  const std::vector<std::filesystem::path> paths{"readme.txt", "song/chart.bms"};
  std::vector<archive_file::FileData> files;
  assert(archive_file::readArchiveEntries(solidPath, paths, files, &error));
  assert(files.size() == 2);
  std::size_t streamed = 0;
  assert(archive_file::readArchiveEntriesStreaming(
      solidPath, paths, [&](archive_file::FileData &&file) {
        assert(std::string(file.bytes.begin(), file.bytes.end()) == contents);
        ++streamed;
        return true;
      }, &error));
  assert(streamed == 2);
  assert(archive_file::listEntriesBounded(solidPath, entries, 10, &error));
  assert(std::all_of(entries.begin(), entries.end(),
                     [](const auto &entry) { return entry.solid; }));
  assert(archive_file::listEntries(plainPath, entries, &error));
  assert(entries.size() == 1 && !entries.front().solid);
}

void testLegacySolidIndexIsRebuilt() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "legacy.7z";
  writeSevenZip(archivePath, "#TITLE Legacy\n", true);
  const auto cacheDir = temporary.path() / "idx";
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  const auto cacheFile = std::filesystem::directory_iterator(cacheDir)->path();
  {
    std::fstream cache(cacheFile, std::ios::binary | std::ios::in | std::ios::out);
    auto readSize = [&] {
      std::uint64_t value = 0;
      cache.read(reinterpret_cast<char *>(&value), sizeof(value));
      assert(cache.good());
      return value;
    };
    cache.put(2);
    cache.seekg(1);
    const auto keySize = readSize();
    cache.seekg(static_cast<std::streamoff>(keySize) + 8 + 8 + 1 + 1,
                 std::ios::cur);
    const auto count = readSize();
    for (std::uint64_t index = 0; index < count; ++index) {
      const auto pathSize = readSize();
      cache.seekg(static_cast<std::streamoff>(pathSize) + 1 + 8 + 8 + 8,
                   std::ios::cur);
      const auto solidOffset = cache.tellg();
      cache.seekp(solidOffset);
      cache.put(0);
      cache.seekg(solidOffset + std::streamoff(1));
    }
    assert(cache.good());
  }
  for (int restart = 0; restart < 2; ++restart) {
    archive_file::clearArchiveIndexCacheForTesting();
    archive_file::setArchiveIndexCacheDirectory(cacheDir);
    assert(archive_file::listEntries(archivePath, entries, &error));
    assert(entries.size() == 2);
    assert(std::all_of(entries.begin(), entries.end(),
                       [](const auto &entry) { return entry.solid; }));
  }
  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void writeStoredZip(const std::filesystem::path &path,
                    const std::vector<std::string> &entryPaths) {
  auto writer = makeArchiveWriteHandle();
  assert(writer);
  assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
  assert(archive_write_set_options(writer.get(), "zip:compression=store") ==
         ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) ==
         ARCHIVE_OK);

  constexpr std::string_view contents = "entry";
  for (const auto &entryPath : entryPaths) {
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    assert(entry);
    archive_entry_set_pathname(entry.get(), entryPath.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(),
                           static_cast<la_int64_t>(contents.size()));
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
}

void testZipIndexAmortizesPausePolling() {
  constexpr int kEntryCount = 513;
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "many-entries.zip";
  std::vector<std::string> entryPaths;
  entryPaths.reserve(kEntryCount);
  for (int index = 0; index < kEntryCount; ++index) {
    entryPaths.push_back("folder/entry-" + std::to_string(index) + ".txt");
  }
  writeStoredZip(archivePath, entryPaths);

  int pauseCalls = 0;
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(
      archivePath, entries, &error, [&] {
        ++pauseCalls;
        return true;
      }));
  assert(entries.size() == kEntryCount);
  assert(pauseCalls < 16);
}

void testZipIndexPreservesFilenameBeyondEmbeddedStatBuffer() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "long-filename.zip";
  const std::string entryPath =
      "folder/" + std::string(600, 'x') + ".bms";
  writeStoredZip(archivePath, {entryPath});

  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 1);
  assert(entries.front().path.generic_string() == entryPath);
}

bool waitForBmsResourceProbe(
    const gameplay::BmsResourceImageAvailabilityProbe &probe) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!probe.complete() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return probe.complete();
}

void testGameplayBmsResourceAvailabilityPublishesLoaderResult() {
  TempDirectory temporary;
  const auto chartPath = temporary.path() / "chart.bms";
  const auto stageFilePath = temporary.path() / "stage.webp";
  std::ofstream(chartPath) << "#TITLE probe\n";
  std::ofstream(stageFilePath) << "not an encoded image";
  bms_parser::ChartMeta meta;
  meta.BmsPath = chartPath;

  const auto exists = [](const std::filesystem::path &path, std::stop_token) {
    return archive_file::exists(path);
  };
  gameplay::BmsResourceImageAvailabilityProbe existing;
  existing.start(meta, "stage.webp", exists);
  gameplay::BmsResourceImageAvailabilityProbe missing;
  missing.start(meta, "missing.webp", exists);
  std::atomic_bool emptyLoaderCalled{false};
  gameplay::BmsResourceImageAvailabilityProbe empty;
  empty.start(meta, {}, [&](const std::filesystem::path &, std::stop_token) {
    emptyLoaderCalled.store(true, std::memory_order_release);
    return true;
  });

  assert(waitForBmsResourceProbe(existing) && existing.available());
  assert(waitForBmsResourceProbe(missing) && !missing.available());
  assert(empty.complete() && !empty.available() &&
         !emptyLoaderCalled.load(std::memory_order_acquire));
}

void testGameplayBmsResourceAvailabilityResolvesVirtualChartNeighbors() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "charts.zip";
  writeStoredZip(
      archivePath,
      {"folder/chart.bms", "folder/stage.webp", "folder/back.bmp"});
  bms_parser::ChartMeta meta;
  meta.BmsPath =
      archive_file::makeVirtualPath(archivePath, "folder/chart.bms");

  const auto exists = [](const std::filesystem::path &path, std::stop_token) {
    return archive_file::exists(path);
  };
  gameplay::BmsResourceImageAvailabilityProbe stage;
  stage.start(meta, "stage.webp", exists);
  gameplay::BmsResourceImageAvailabilityProbe back;
  back.start(meta, "back.bmp", exists);
  gameplay::BmsResourceImageAvailabilityProbe missing;
  missing.start(meta, "missing.png", exists);

  assert(waitForBmsResourceProbe(stage) && stage.available());
  assert(waitForBmsResourceProbe(back) && back.available());
  assert(waitForBmsResourceProbe(missing) && !missing.available());
}

void testZipIndexRejectsEmbeddedNulInShortFilename() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "nul-filename.zip";
  const std::string entryPath = "safe-name.bms";
  writeStoredZip(archivePath, {entryPath});

  std::ifstream input(archivePath, std::ios::binary);
  assert(input);
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  input.close();
  std::size_t patchedNames = 0;
  for (std::size_t offset = bytes.find(entryPath); offset != std::string::npos;
       offset = bytes.find(entryPath, offset + entryPath.size())) {
    bytes[offset + 4] = '\0';
    ++patchedNames;
  }
  assert(patchedNames == 2);
  std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
  assert(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();

  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.empty());
}

void testZipIndexPausePollingStillCancelsDuringLargeDirectory() {
  constexpr int kEntryCount = 513;
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "cancel-index.zip";
  std::vector<std::string> entryPaths;
  entryPaths.reserve(kEntryCount);
  for (int index = 0; index < kEntryCount; ++index) {
    entryPaths.push_back("entry-" + std::to_string(index) + ".txt");
  }
  writeStoredZip(archivePath, entryPaths);

  int pauseCalls = 0;
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(!archive_file::listEntries(
      archivePath, entries, &error, [&] { return ++pauseCalls < 4; }));
  assert(entries.empty());
  assert(error == "Operation cancelled");
  assert(pauseCalls >= 4);
  assert(pauseCalls < 8);
}

void testZipIndexUsesCommonSystemEntryFilter() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "system-entries.zip";
  writeStoredZip(archivePath,
                 {"__MACOSX/._chart.bms", "music/chart.bms"});

  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 1);
  assert(entries.front().path.generic_string() == "music/chart.bms");
}

void testBoundedReadRejectsOversizedIndexedEntryBeforeExtraction() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "bounded-entry.zip";
  writeStoredZip(archivePath, {"artwork.png"});

  std::vector<unsigned char> bytes;
  std::string error;
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "artwork.png"), bytes, 4,
      &error));
  assert(bytes.empty());
  assert(error.find("exceeds bounded read limit") != std::string::npos);
}

void testBoundedReadStreamsOrdinaryPlatformPathExactlyOnce() {
  TempDirectory temporary;
  const auto path = temporary.path() / "platform-resource.bin";
  constexpr std::string_view payload = "0123456789";
  std::ofstream(path, std::ios::binary)
      .write(payload.data(), static_cast<std::streamsize>(payload.size()));

  std::vector<unsigned char> bytes;
  std::string error;
  assert(archive_file::readFileBounded(path, bytes, payload.size(), &error));
  assert(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()) == payload);

  bytes.assign(1, 0xff);
  error.clear();
  assert(!archive_file::readFileBounded(path, bytes, payload.size() - 1U,
                                        &error));
  assert(bytes.empty());
  assert(error.find("exceeds bounded read limit") != std::string::npos);

  std::stop_source stopped;
  stopped.request_stop();
  bytes.assign(1, 0xff);
  assert(!archive_file::readFileBounded(path, bytes, payload.size(), nullptr,
                                        stopped.get_token()));
  assert(bytes.empty());
}

void writeStoredZipContents(const std::filesystem::path &path,
                            const std::string &entryPath,
                            const std::string &contents,
                            bool deflated = false) {
  auto writer = makeArchiveWriteHandle();
  assert(writer);
  assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
  assert(archive_write_set_options(writer.get(), deflated
                                                   ? "zip:compression=deflate"
                                                   : "zip:compression=store") ==
         ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) ==
         ARCHIVE_OK);

  ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
  assert(entry);
  archive_entry_set_pathname(entry.get(), entryPath.c_str());
  archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0644);
  archive_entry_set_size(entry.get(),
                         static_cast<la_int64_t>(contents.size()));
  assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
  assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
         static_cast<la_ssize_t>(contents.size()));
  assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
}

void testLargeZipCancellation(bool deflated, bool concurrent,
                              bool afterOutputProduced) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "large-cancellable.zip";
  const std::string payload(8 * 1024 * 1024, 'x');
  writeStoredZipContents(archivePath, "large.bin", payload, deflated);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));

  std::atomic_int checkpoints{0};
  std::atomic_int emitted{0};
  bounded_allocation_probe::observedEntry = nullptr;
  bounded_allocation_probe::observedEntrySize = payload.size();
  auto checkpoint = [&] {
    const auto *entry = bounded_allocation_probe::observedEntry;
    if (entry == nullptr ||
        (afterOutputProduced && entry[payload.size() - 1] != 'x')) {
      return true;
    }
    return ++checkpoints < 4;
  };
  auto consume = [&](archive_file::FileData &&) {
    ++emitted;
    return true;
  };
  const bool read = concurrent
      ? archive_file::readArchiveEntriesConcurrently(
            archivePath, {"large.bin"}, consume, 2, payload.size() * 2,
            &error, checkpoint)
      : archive_file::readArchiveEntriesStreaming(
            archivePath, {"large.bin"}, consume, &error, checkpoint);
  bounded_allocation_probe::observedEntrySize = 0;
  bounded_allocation_probe::observedEntry = nullptr;
  assert(!read);
  assert(checkpoints >= 4);
  assert(emitted == 0);
  assert(error == "Operation cancelled");

  error.clear();
  auto verify = [&](archive_file::FileData &&file) {
    assert(file.path == "large.bin");
    assert(std::string_view(reinterpret_cast<const char *>(file.bytes.data()),
                            file.bytes.size()) == payload);
    ++emitted;
    return true;
  };
  assert(archive_file::readArchiveEntriesConcurrently(
      archivePath, {"large.bin"}, verify, 2, payload.size() * 2, &error));
  assert(emitted == 1);
}

void testBatchCancellationDoesNotRestartFallback(bool sevenZip, bool ranged) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() /
      (sevenZip ? "terminal-cancellation.7z" : "terminal-cancellation.zip");
  if (sevenZip) {
    writeSevenZip(archivePath, "entry");
  } else {
    writeStoredZip(archivePath, {"readme.txt"});
  }
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  for (bool reportError : {false, true}) {
    int checkpoints = 0;
    std::vector<archive_file::FileData> files;
    error.clear();
    auto checkpoint = [&] { return ++checkpoints != 1; };
    const bool read = ranged
        ? archive_file::readArchiveEntriesInRange(
              archivePath, {"readme.txt"}, {0, 0}, files,
              reportError ? &error : nullptr, checkpoint)
        : archive_file::readArchiveEntries(
              archivePath, {"readme.txt"}, files,
              reportError ? &error : nullptr, checkpoint);
    assert(!read);
    assert(files.empty());
    assert(checkpoints == 1);
    if (reportError) {
      assert(error == "Operation cancelled");
    }
  }
}

void testCachedSevenZipHandleWaitCancellation(int operation) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "held-handle.7z";
  writeSevenZip(archivePath, "entry");
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));

  std::promise<void> holderEntered;
  auto holderEnteredFuture = holderEntered.get_future();
  std::promise<void> releaseHolder;
  auto releaseHolderFuture = releaseHolder.get_future().share();
  auto holder = std::async(std::launch::async, [&] {
    std::string holderError;
    return archive_file::readArchiveEntriesStreaming(
        archivePath, {"readme.txt"}, [&](archive_file::FileData &&file) {
          assert(file.bytes.size() == 5);
          holderEntered.set_value();
          assert(releaseHolderFuture.wait_for(30s) ==
                 std::future_status::ready);
          assert(file.bytes.front() == 'e');
          return true;
        }, &holderError);
  });
  assert(holderEnteredFuture.wait_for(10s) == std::future_status::ready);
  if (operation == 4) {
    archive_file::clearArchiveIndexCacheForTesting();
  }
  std::atomic_int checkpoints{0};
  std::vector<archive_file::FileData> files;
  std::vector<unsigned char> bytes;
  auto waiter = std::async(std::launch::async, [&] {
    auto checkpoint = [&] { return ++checkpoints != 32 || operation == 5; };
    switch (operation) {
    case 0:
      return archive_file::readArchiveEntries(
          archivePath, {"readme.txt"}, files, &error, checkpoint);
    case 1:
      return archive_file::readArchiveEntriesStreaming(
          archivePath, {"readme.txt"}, [&](archive_file::FileData &&file) {
            files.push_back(std::move(file));
            return true;
          }, &error, checkpoint);
    case 2:
      return archive_file::readArchiveEntriesInRange(
          archivePath, {"readme.txt"}, {0, 0}, files, &error, checkpoint);
    case 3:
      return archive_file::readFileBoundedWithCheckpoint(
          archive_file::makeVirtualPath(archivePath, "readme.txt"), bytes,
          1024, &error, {}, checkpoint);
    case 4:
      return archive_file::listEntries(archivePath, entries, &error, checkpoint);
    default:
      return archive_file::unzipArchiveFully(
          archivePath, temporary.path() / "output", &error, nullptr,
          nullptr, checkpoint).has_value();
    }
  });
  const bool cancelledWhileHeld =
      waiter.wait_for(10s) == std::future_status::ready;
  assert(holder.wait_for(0s) == std::future_status::timeout);
  releaseHolder.set_value();
  assert(holder.wait_for(10s) == std::future_status::ready);
  assert(holder.get());
  assert(waiter.wait_for(10s) == std::future_status::ready);
  const bool read = waiter.get();
  assert(cancelledWhileHeld);
  assert(read == (operation == 5));
  if (operation != 5) assert(checkpoints == 32);
  assert(files.empty());
  assert(bytes.empty());
  assert(error == (operation == 5 ? "" : "Operation cancelled"));

  error.clear();
  assert(archive_file::readArchiveEntries(
      archivePath, {"readme.txt"}, files, &error));
  assert(files.size() == 1);
  assert(std::string(files.front().bytes.begin(), files.front().bytes.end()) ==
         "entry");
}

std::uint32_t readLeU32(const unsigned char *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void writeLeU32(unsigned char *bytes, std::uint32_t value) {
  bytes[0] = static_cast<unsigned char>(value & 0xffu);
  bytes[1] = static_cast<unsigned char>((value >> 8u) & 0xffu);
  bytes[2] = static_cast<unsigned char>((value >> 16u) & 0xffu);
  bytes[3] = static_cast<unsigned char>((value >> 24u) & 0xffu);
}

bool readZipFixture(const std::filesystem::path &archivePath, int operation,
                    std::vector<archive_file::FileData> &files,
                    std::string &error,
                    archive_file::PauseCallback checkpoint) {
  auto consume = [&](archive_file::FileData &&file) {
    files.push_back(std::move(file));
    return true;
  };
  switch (operation) {
  case 0:
    return archive_file::readArchiveEntriesConcurrently(
        archivePath, {"payload.bin", "missing.bin", "payload.bin"},
        consume, 2, 4 * 1024 * 1024, &error, checkpoint);
  case 1:
    return archive_file::readArchiveEntriesStreaming(
        archivePath, {"payload.bin"}, consume, &error, checkpoint);
  case 2:
    return archive_file::readArchiveEntries(
        archivePath, {"payload.bin"}, files, &error, checkpoint);
  default:
    return archive_file::readArchiveEntriesInRange(
        archivePath, {"payload.bin"}, {0, 0}, files, &error, checkpoint);
  }
}

void writeZipWithEmptyDeflateBlocks(const std::filesystem::path &archivePath,
                                    const std::string &payload,
                                    bool emptyPrefix) {
  constexpr std::string_view name = "payload.bin";
  const std::string emptyBlock("\0\0\0\xff\xff", 5);
  std::string emptyBlocks;
  for (std::size_t block = 0; block < 1024 * 1024; ++block) {
    emptyBlocks += emptyBlock;
  }
  std::string compressed;
  if (emptyPrefix) {
    compressed += emptyBlocks;
  }
  for (std::size_t offset = 0; offset < payload.size();) {
    const auto count = std::min<std::size_t>(65535, payload.size() - offset);
    compressed.push_back('\0');
    compressed.push_back(static_cast<char>(count & 0xffu));
    compressed.push_back(static_cast<char>(count >> 8u));
    compressed.push_back(static_cast<char>(~count & 0xffu));
    compressed.push_back(static_cast<char>((~count >> 8u) & 0xffu));
    compressed.append(payload, offset, count);
    offset += count;
  }
  if (!emptyPrefix) {
    compressed += emptyBlocks;
  }
  compressed.append("\1\0\0\xff\xff", 5);

  std::uint32_t crc = 0xffffffffu;
  for (unsigned char byte : payload) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
  }
  crc ^= 0xffffffffu;

  std::string localHeader(30, '\0');
  auto *local = reinterpret_cast<unsigned char *>(localHeader.data());
  writeLeU32(local, 0x04034b50u);
  local[4] = 20;
  local[8] = 8;
  writeLeU32(local + 14, crc);
  writeLeU32(local + 18, static_cast<std::uint32_t>(compressed.size()));
  writeLeU32(local + 22, static_cast<std::uint32_t>(payload.size()));
  local[26] = static_cast<unsigned char>(name.size());

  std::string centralHeader(46, '\0');
  auto *central = reinterpret_cast<unsigned char *>(centralHeader.data());
  writeLeU32(central, 0x02014b50u);
  central[4] = 20;
  central[6] = 20;
  central[10] = 8;
  writeLeU32(central + 16, crc);
  writeLeU32(central + 20, static_cast<std::uint32_t>(compressed.size()));
  writeLeU32(central + 24, static_cast<std::uint32_t>(payload.size()));
  central[28] = static_cast<unsigned char>(name.size());

  std::string endRecord(22, '\0');
  auto *end = reinterpret_cast<unsigned char *>(endRecord.data());
  writeLeU32(end, 0x06054b50u);
  end[8] = 1;
  end[10] = 1;
  writeLeU32(end + 12,
              static_cast<std::uint32_t>(centralHeader.size() + name.size()));
  writeLeU32(end + 16, static_cast<std::uint32_t>(
                           localHeader.size() + name.size() + compressed.size()));
  std::ofstream output(archivePath, std::ios::binary);
  assert(output);
  output << localHeader << name << compressed << centralHeader << name
         << endRecord;
  output.close();
  assert(output);
}

void testZipCancellationDuringEmptyDeflateBlocks(bool emptyPrefix,
                                                int operation) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "empty-blocks.zip";
  const std::string payload(65537, 'x');
  writeZipWithEmptyDeflateBlocks(archivePath, payload, emptyPrefix);
  std::vector<archive_file::FileData> files;
  std::string error;
  assert(readZipFixture(archivePath, operation, files, error,
                         [] { return true; }));
  assert(files.size() == 1);
  assert(std::string(files.front().bytes.begin(), files.front().bytes.end()) ==
         payload);
  files.clear();

  int checkpoints = 0;
  bounded_allocation_probe::observedEntry = nullptr;
  bounded_allocation_probe::observedEntrySize = payload.size();
  const bool read = readZipFixture(archivePath, operation, files, error, [&] {
    const auto *entry = bounded_allocation_probe::observedEntry;
    if (entry == nullptr) {
      return true;
    }
    if (++checkpoints != 16) {
      return true;
    }
    assert(entry[0] == (emptyPrefix ? '\0' : 'x'));
    return false;
  });
  bounded_allocation_probe::observedEntrySize = 0;
  bounded_allocation_probe::observedEntry = nullptr;
  assert(!read);
  assert(files.empty());
  assert(checkpoints == 16);
  assert(error == "Operation cancelled");

  error.clear();
  assert(readZipFixture(archivePath, operation, files, error,
                         [] { return true; }));
  assert(files.size() == 1);
  assert(std::string(files.front().bytes.begin(), files.front().bytes.end()) ==
         payload);
}

void testZipInputCallbackRestoresAcrossEntries() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "restore-read-callback.zip";
  writeStoredZip(archivePath, {"first.bin", "second.bin"});
  std::vector<archive_file::FileData> files;
  std::string error;
  assert(archive_file::readArchiveEntriesStreaming(
      archivePath, {"first.bin", "second.bin"}, [&](archive_file::FileData &&file) {
        files.push_back(std::move(file));
        return true;
      }, &error, [] { return true; }));
  assert(files.size() == 2);
  assert(files[0].path == "first.bin");
  assert(files[1].path == "second.bin");
  for (const auto &file : files) {
    assert(std::string(file.bytes.begin(), file.bytes.end()) == "entry");
  }
}

void testZipChunkedReadBoundaryIntegrity(bool deflated) {
  TempDirectory temporary;
  for (std::size_t size : {0U, 1U, 65535U, 65536U, 65537U, 1048576U}) {
    std::string payload(size, '\0');
    std::uint32_t randomState = 0x12345678u;
    for (char &byte : payload) {
      randomState ^= randomState << 13;
      randomState ^= randomState >> 17;
      randomState ^= randomState << 5;
      byte = static_cast<char>(randomState & 0xffu);
    }
    const auto archivePath =
        temporary.path() / (std::to_string(size) + ".zip");
    writeStoredZipContents(archivePath, "payload.bin", payload, deflated);

    for (int operation = 0; operation < 4; ++operation) {
      std::string error;
      std::vector<archive_file::FileData> files;
      assert(readZipFixture(archivePath, operation, files, error,
                             [] { return true; }));
      assert(error.empty());
      assert(files.size() == 1);
      assert(files.front().path == "payload.bin");
      assert(std::string(files.front().bytes.begin(),
                         files.front().bytes.end()) == payload);
    }

    std::ifstream input(archivePath, std::ios::binary);
    assert(input);
    std::string archiveBytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    const auto centralOffset = archiveBytes.rfind(std::string("PK\1\2", 4));
    assert(centralOffset != std::string::npos);
    assert(centralOffset + 46 <= archiveBytes.size());
    archiveBytes[centralOffset + 16] ^= 1;
    archiveBytes[14] ^= 1;
    const auto *header =
        reinterpret_cast<const unsigned char *>(archiveBytes.data());
    const auto dataOffset = 30 + (readLeU32(header + 26) & 0xffffu) +
                            (readLeU32(header + 28) & 0xffffu);
    const auto descriptorOffset =
        dataOffset + readLeU32(header + centralOffset + 20);
    if ((header[6] & 8u) != 0) {
      assert(readLeU32(header + descriptorOffset) == 0x08074b50u);
      archiveBytes[descriptorOffset + 4] ^= 1;
    }
    const auto corruptPath =
        temporary.path() / (std::to_string(size) + "-corrupt.zip");
    std::ofstream output(corruptPath, std::ios::binary);
    assert(output);
    output.write(archiveBytes.data(),
                 static_cast<std::streamsize>(archiveBytes.size()));
    output.close();
    assert(output);

    for (bool checkpointEnabled : {false, true}) {
      archive_file::PauseCallback checkpoint;
      if (checkpointEnabled) {
        checkpoint = [] { return true; };
      }
      for (int operation = 0; operation < 4; ++operation) {
        std::vector<archive_file::FileData> files;
        std::string error;
        assert(!readZipFixture(corruptPath, operation, files, error, checkpoint));
        assert(files.empty());
        assert(error.find("CRC") != std::string::npos);
      }
    }
  }
}

// Rewrites the declared uncompressed size in the central directory of a stored
// ZIP entry to a value smaller than the actual content, simulating a lying
// central directory that under-reports an entry's real uncompressed output.
// Both the archive index and the bounded extraction stat are built from the
// central directory, so this is the field that must lie to trip the
// streaming-bound enforcement rather than the pre-gate size check.
bool understateZipUncompressedSizes(const std::filesystem::path &path,
                                    const std::string &entryPath,
                                    std::uint32_t newUncompressedSize) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
  input.close();
  const std::vector<unsigned char> name(entryPath.begin(), entryPath.end());

  constexpr std::uint32_t kCentralDirectorySignature = 0x02014b50u;
  std::size_t patchedEntries = 0;
  for (std::size_t offset = 0;
       (offset = static_cast<std::size_t>(std::search(
                     bytes.begin() + offset, bytes.end(), name.begin(),
                     name.end()) -
                 bytes.begin())) < bytes.size();
       ++offset) {
    if (offset < 46 ||
        readLeU32(&bytes[offset - 46]) != kCentralDirectorySignature ||
        offset - 46 + 28 > bytes.size()) {
      // The local header and other name occurrences are skipped; only the
      // central directory entry carries the declared uncompressed size.
      if (offset + 1 >= bytes.size()) {
        break;
      }
      continue;
    }
    writeLeU32(&bytes[offset - 46 + 24], newUncompressedSize);
    ++patchedEntries;
  }
  if (patchedEntries != 1) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  return true;
}

void testZipBoundedReadStreamsFullInBoundsEntry() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "streamed-full.zip";
  const std::string payload = "0123456789abcdef-golden-bounded-payload";
  writeStoredZipContents(archivePath, "content.bin", payload);

  std::vector<unsigned char> bytes;
  std::string error;
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "content.bin"), bytes,
      payload.size(), &error));
  assert(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()) == payload);

  bytes.clear();
  error.clear();
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "content.bin"), bytes, 4096,
      &error));
  assert(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()) == payload);
}

void testZipBoundedReadRejectsCorruptStoredPayloadWithUnchangedCrc() {
  TempDirectory temporary;
  const auto validPath = temporary.path() / "valid-pcm.zip";
  const auto corruptPath = temporary.path() / "corrupt-pcm.zip";
  std::string payload(44 + 128 * 1024, '\0');
  payload.replace(0, 4, "RIFF");
  payload.replace(8, 8, "WAVEfmt ");
  payload.replace(36, 4, "data");
  auto *header = reinterpret_cast<unsigned char *>(payload.data());
  writeLeU32(header + 4, static_cast<std::uint32_t>(payload.size() - 8));
  writeLeU32(header + 16, 16);
  payload[20] = 1;
  payload[22] = 1;
  writeLeU32(header + 24, 44100);
  writeLeU32(header + 28, 88200);
  payload[32] = 2;
  payload[34] = 16;
  writeLeU32(header + 40, static_cast<std::uint32_t>(payload.size() - 44));
  writeStoredZipContents(validPath, "preview.wav", payload);

  std::ifstream input(validPath, std::ios::binary);
  assert(input);
  std::string archiveBytes((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  const auto payloadOffset = archiveBytes.find(payload);
  assert(payloadOffset != std::string::npos);
  archiveBytes[payloadOffset + 44 + 65536] ^= 1;
  std::ofstream output(corruptPath, std::ios::binary);
  assert(output);
  output.write(archiveBytes.data(),
               static_cast<std::streamsize>(archiveBytes.size()));
  output.close();
  assert(std::filesystem::file_size(validPath) ==
         std::filesystem::file_size(corruptPath));

  std::vector<unsigned char> bytes;
  std::string error;
  const auto validVirtualPath =
      archive_file::makeVirtualPath(validPath, "preview.wav");
  assert(archive_file::readFileBounded(validVirtualPath, bytes,
                                      payload.size(), &error));
  assert(std::string(bytes.begin(), bytes.end()) == payload);
  std::vector<archive_file::Entry> entries;
  assert(archive_file::listEntries(corruptPath, entries, &error));
  assert(entries.size() == 1 && entries.front().size == payload.size());
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(corruptPath, "preview.wav"), bytes,
      payload.size(), &error));
  assert(bytes.empty());
  assert(error.find("CRC") != std::string::npos);

  error.clear();
  assert(!archive_file::readFileBounded(validVirtualPath, bytes,
                                       payload.size() - 1, &error));
  assert(bytes.empty());
  assert(error.find("exceeds bounded read limit") != std::string::npos);
  std::stop_source stopped;
  stopped.request_stop();
  bytes.assign(1, 0xff);
  error.clear();
  assert(!archive_file::readFileBounded(validVirtualPath, bytes,
                                       payload.size(), &error,
                                       stopped.get_token()));
  assert(bytes.empty());
  assert(error.empty());
  assert(archive_file::readFileBounded(validVirtualPath, bytes,
                                      payload.size(), &error));
  assert(std::string(bytes.begin(), bytes.end()) == payload);
}

void testZipBoundedReadAcceptsEmptyStoredEntry() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "empty-entry.zip";
  writeStoredZipContents(archivePath, "empty.bin", "");
  std::vector<unsigned char> bytes{0xff};
  std::string error;
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "empty.bin"), bytes, 0,
      &error));
  assert(bytes.empty());
  assert(error.empty());
}

void testBoundedReadFallsBackToAlternativeAudioExtension() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "preview-extension.zip";
  const std::string payload = "ogg-lives-here";
  // The archive stores the audio as .ogg, but the chart references it as .wav
  // (BMS #PREVIEW quirk: the extension in the chart need not match the file).
  writeStoredZipContents(archivePath, "folder/music.ogg", payload);

  std::vector<unsigned char> bytes;
  std::string error;
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "folder/music.wav"), bytes,
      payload.size(), &error));
  assert(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()) == payload);
  assert(bytes == std::vector<unsigned char>(payload.begin(), payload.end()));
}

void testZipBoundedReadRejectsCentralDirectoryUnderstatedSize() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "lied-central-dir.zip";
  const std::string content = "abcdefghijklmnopqrstuvwxyz012345";
  writeStoredZipContents(archivePath, "lied.bin", content);
  assert(content.size() > 7);
  assert(understateZipUncompressedSizes(archivePath, "lied.bin", 7));

  std::vector<unsigned char> bytes;
  std::string error;
  // The indexed size (7) is within the limit, so the pre-gate passes and the
  // bound must be enforced during bounded extraction; a truncated or oversized
  // buffer must not be handed back as a successful read.
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "lied.bin"), bytes, 7,
      &error));
  assert(bytes.empty());
  assert(!error.empty());
}

void testBoundedReadRejectsOversizedSevenZipEntry() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "bounded.7z";
  const std::string payload = "seven zip bounded read regression payload 12345";
  writeSevenZip(archivePath, payload);

  std::vector<unsigned char> bytes;
  std::string error;
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "readme.txt"), bytes,
      payload.size(), &error));
  assert(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                          bytes.size()) == payload);

  bytes.clear();
  error.clear();
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(archivePath, "readme.txt"), bytes,
      payload.size() - 1U, &error));
  assert(bytes.empty());
  assert(error.find("exceeds bounded read limit") != std::string::npos);
}

void testBzipZipFallbackStopsBeforeOversizedAllocation() {
  constexpr unsigned char fixture[] = {
      0x50,0x4b,0x03,0x04,0x2e,0x00,0x00,0x00,0x0c,0x00,0x61,0xae,0x28,0x5d,0xc9,0xbe,
      0xf6,0x81,0x30,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x0b,0x00,0x00,0x00,0x61,0x72,
      0x74,0x77,0x6f,0x72,0x6b,0x2e,0x70,0x6e,0x67,0x42,0x5a,0x68,0x39,0x31,0x41,0x59,
      0x26,0x53,0x59,0x6d,0xc2,0x25,0x57,0x00,0x08,0x0a,0x44,0x00,0x80,0x04,0x20,0x00,
      0x00,0x08,0x20,0x00,0x30,0xcc,0x05,0x49,0xea,0x71,0x06,0x01,0x40,0x60,0x1e,0x2e,
      0xe4,0x8a,0x70,0xa1,0x20,0xdb,0x84,0x4a,0xae,0x50,0x4b,0x01,0x02,0x2e,0x03,0x2e,
      0x00,0x00,0x00,0x0c,0x00,0x61,0xae,0x28,0x5d,0xc9,0xbe,0xf6,0x81,0x30,0x00,0x00,
      0x00,0x00,0x00,0x10,0x00,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
      0x00,0x80,0x01,0x00,0x00,0x00,0x00,0x61,0x72,0x74,0x77,0x6f,0x72,0x6b,0x2e,0x70,
      0x6e,0x67,0x50,0x4b,0x05,0x06,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x39,0x00,
      0x00,0x00,0x59,0x00,0x00,0x00,0x00,0x00};
  TempDirectory temporary;
  const auto valid = temporary.path() / "valid-bzip.zip";
  const auto forged = temporary.path() / "forged-bzip.zip";
  for (const auto &path : {valid, forged}) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(fixture), sizeof(fixture));
  }
  assert(understateZipUncompressedSizes(forged, "artwork.png", 1));
  {
    std::fstream output(forged, std::ios::binary | std::ios::in | std::ios::out);
    const unsigned char declaredSize[] = {1, 0, 0, 0};
    output.seekp(22);
    output.write(reinterpret_cast<const char *>(declaredSize), sizeof(declaredSize));
  }
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(forged, entries, &error));
  assert(entries.size() == 1 && entries.front().size == 1);
  for (const std::size_t budget : {128u * 1024u, 350000u}) {
    std::vector<unsigned char> bytes;
    bounded_allocation_probe::largest = 0;
    bounded_allocation_probe::enabled = true;
    const bool succeeded = archive_file::readFileBounded(
        archive_file::makeVirtualPath(forged, "artwork.png"), bytes, budget, &error);
    bounded_allocation_probe::enabled = false;
    std::cerr << "BZIP2 fallback largest allocation: "
              << bounded_allocation_probe::largest << " budget: " << budget << '\n';
    assert(!succeeded && bytes.empty());
    assert(bounded_allocation_probe::largest <= budget);
    assert(error.find("exceeds bounded read limit") != std::string::npos);
    bool consumed = false;
    bounded_allocation_probe::largest = 0;
    bounded_allocation_probe::enabled = true;
    const bool streamed = archive_file::readArchiveEntriesStreamingBounded(
        forged, {"artwork.png"}, [&](archive_file::FileData &&) { consumed = true; return true; }, budget, &error);
    bounded_allocation_probe::enabled = false;
    assert(!streamed && !consumed && bounded_allocation_probe::largest <= budget);
    archive_file::UnzipBudget unzipBudget{.limits = {.maximumWorkers = 1, .maximumMemoryBytes = budget}};
    bounded_allocation_probe::largest = 0;
    bounded_allocation_probe::enabled = true;
    const auto unzipped = archive_file::unzipArchiveFully(forged, temporary.path() / std::to_string(budget),
        &error, nullptr, nullptr, nullptr, false, nullptr, &unzipBudget);
    bounded_allocation_probe::enabled = false;
    assert(!unzipped && unzipBudget.writtenBytes == 0 && bounded_allocation_probe::largest <= budget);
  }
  std::vector<unsigned char> bytes;
  assert(archive_file::readFileBounded(
      archive_file::makeVirtualPath(valid, "artwork.png"), bytes, 1048576, &error));
  assert(bytes.size() == 1048576);
  assert(std::ranges::all_of(bytes, [](unsigned char value) { return value == 'A'; }));
  const auto corrupt = temporary.path() / "corrupt-bzip.zip";
  {
    auto corruptedFixture = std::to_array(fixture);
    corruptedFixture[14] ^= 1;
    corruptedFixture[105] ^= 1;
    std::ofstream output(corrupt, std::ios::binary);
    output.write(reinterpret_cast<const char *>(corruptedFixture.data()),
                 corruptedFixture.size());
  }
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(corrupt, "artwork.png"), bytes, 1048576, &error));
  assert(bytes.empty());
  assert(!archive_file::readFileBounded(
      archive_file::makeVirtualPath(forged, "artwork.png"), bytes, 1048576, &error));
  assert(bytes.empty());
  assert(error.find("exceeds bounded read limit") == std::string::npos);
  std::vector<unsigned char> cancelledBytes;
  std::stop_source stop;
  bounded_allocation_probe::cancelOnChunk = &stop;
  const bool cancelledRead = archive_file::readFileBounded(
      archive_file::makeVirtualPath(forged, "artwork.png"), cancelledBytes,
      1048576, &error, stop.get_token());
  bounded_allocation_probe::cancelOnChunk = nullptr;
  assert(stop.stop_requested());
  assert(!cancelledRead && cancelledBytes.empty());
}

void testIndependentSevenZipCacheMissesOpenConcurrently() {
  TempDirectory temporary;
  const auto firstPath = temporary.path() / "first.7z";
  const auto secondPath = temporary.path() / "second.7z";
  writeSevenZip(firstPath, "first");
  writeSevenZip(secondPath, "second");

  std::mutex barrierMutex;
  std::condition_variable barrierCv;
  int arrived = 0;
  bool timedOut = false;
  std::atomic_bool firstListed{false};
  std::atomic_bool secondListed{false};

  auto listOne = [&](const std::filesystem::path &path,
                     std::atomic_bool &listedResult) {
    int pauseCalls = 0;
    std::vector<archive_file::Entry> entries;
    std::string error;
    const bool listed = archive_file::listEntries(
        path, entries, &error, [&] {
          if (++pauseCalls != 3) {
            return true;
          }
          std::unique_lock lock(barrierMutex);
          ++arrived;
          barrierCv.notify_all();
          if (!barrierCv.wait_for(lock, 2s, [&] { return arrived == 2; })) {
            timedOut = true;
            barrierCv.notify_all();
          }
          return true;
        });
    listedResult.store(listed && entries.size() == 1,
                       std::memory_order_release);
  };

  std::thread first(listOne, std::cref(firstPath), std::ref(firstListed));
  std::thread second(listOne, std::cref(secondPath), std::ref(secondListed));
  first.join();
  second.join();

  assert(firstListed.load(std::memory_order_acquire));
  assert(secondListed.load(std::memory_order_acquire));
  assert(arrived == 2);
  assert(!timedOut);

  const auto logLines = archive_file::debugLogLines();
  const auto sevenZipIndexes = std::count_if(
      logLines.begin(), logLines.end(), [](const std::string &line) {
        return line.find("Indexed archive with 7-Zip SDK:") !=
               std::string::npos;
      });
  assert(sevenZipIndexes == 2);
}

void testSevenZipReadUsesCurrentOperationPauseCallback() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "sound.7z";
  std::string soundData(3 * 1024 * 1024, '\0');
  std::uint32_t randomState = 0x9e3779b9u;
  for (char &byte : soundData) {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    byte = static_cast<char>(randomState & 0xffu);
  }
  writeSevenZip(archivePath, soundData);

  std::atomic_bool indexingActive{true};
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error, [&] {
    return indexingActive.load(std::memory_order_acquire);
  }));
  assert(entries.size() == 1);

  indexingActive.store(false, std::memory_order_release);
  std::vector<archive_file::FileData> files;
  const archive_file::EntryRange range{.start = entries.front().order,
                                       .end = entries.front().order};
  assert(archive_file::readArchiveEntriesInRange(
      archivePath, {"readme.txt"}, range, files, &error,
      [] { return true; }));
  assert(files.size() == 1);
  const std::string contents(files.front().bytes.begin(),
                             files.front().bytes.end());
  assert(contents == soundData);

  const auto logLines = archive_file::debugLogLines();
  assert(std::any_of(logLines.begin(), logLines.end(), [&](const auto &line) {
    return line.find("Read archive range via 7-Zip SDK:") !=
               std::string::npos &&
           line.find(archivePath.filename().string()) != std::string::npos;
  }));
}

void testEncodedHeaderSevenZipUsesSdk() {
  const std::filesystem::path payloadPath = "encoded-header-payload.txt";
  for (std::string_view fixtureName : {"encoded-header-lzma.7z",
                                       "encoded-header-lzma2.7z"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/archive" / fixtureName;

    std::vector<archive_file::Entry> entries;
    std::string error;
    assert(archive_file::listEntries(fixture, entries, &error));
    assert(entries.size() == 1);
    assert(entries.front().path == payloadPath);

    std::vector<archive_file::FileData> files;
    assert(archive_file::readArchiveEntries(fixture, {payloadPath}, files,
                                            &error));
    assert(files.size() == 1);
    assert(files.front().path == payloadPath);
    const std::string payload(files.front().bytes.begin(),
                              files.front().bytes.end());
    assert(payload == "compressed-header-payload\n");

    const auto logLines = archive_file::debugLogLines();
    const auto hasFixtureLog = [&](std::string_view prefix) {
      return std::any_of(logLines.begin(), logLines.end(),
                         [&](const auto &line) {
                           return line.find(prefix) != std::string::npos &&
                                  line.find(fixtureName) != std::string::npos;
                         });
    };
    assert(hasFixtureLog("Indexed archive with 7-Zip SDK:"));
    assert(hasFixtureLog("Read archive batch via 7-Zip SDK:"));
  }
}

void testDeltaFilteredSevenZipUsesSdk() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "delta-lzma2.7z";
  std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
  assert(output);
  output.write(reinterpret_cast<const char *>(kDeltaLzma2SevenZip.data()),
               static_cast<std::streamsize>(kDeltaLzma2SevenZip.size()));
  output.close();

  const std::filesystem::path payloadPath = "delta-sound.wav";
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 1);
  assert(entries.front().path == payloadPath);

  const archive_file::EntryRange range{.start = entries.front().order,
                                       .end = entries.front().order};
  std::vector<archive_file::FileData> files;
  assert(archive_file::readArchiveEntriesInRange(
      archivePath, {payloadPath}, range, files, &error));
  assert(files.size() == 1);
  const std::string_view payload(
      reinterpret_cast<const char *>(files.front().bytes.data()),
      files.front().bytes.size());
  assert(payload == kDeltaSoundPayload);

  const auto logLines = archive_file::debugLogLines();
  assert(std::any_of(logLines.begin(), logLines.end(), [&](const auto &line) {
    return line.find("Read archive range via 7-Zip SDK:") !=
               std::string::npos &&
           line.find(archivePath.filename().string()) != std::string::npos;
  }));
}

void testBoundedStreamingRejectsOversizedPayload() {
  TempDirectory temporary;
  const auto path = temporary.path() / "bounded.zip";
  writeStoredZipContents(path, "payload.bin", std::string(128 * 1024, 'x'), true);
  std::string error;
  std::size_t received = 0;
  const auto consume = [&](archive_file::FileData &&file) {
    ++received;
    assert(file.bytes.size() <= 128 * 1024);
    return true;
  };
  assert(!archive_file::readArchiveEntriesStreamingBounded(path, {"payload.bin"}, consume, 1024, &error));
  assert(received == 0 && !error.empty());
  assert(archive_file::readArchiveEntriesStreamingBounded(path, {"payload.bin"}, consume, 128 * 1024, &error));
  assert(received == 1);
  archive_file::clearArchiveIndexCacheForTesting();
  assert(understateZipUncompressedSizes(path, "payload.bin", 1));
  assert(!archive_file::readArchiveEntriesStreamingBounded(path, {"payload.bin"}, consume, 1024, &error));
  assert(received == 1);
}

void testConcurrentReaderRejectsOversizedPayload() {
  TempDirectory temporary;
  const auto path = temporary.path() / "bounded-concurrent.zip";
  writeStoredZipContents(path, "payload.bin", std::string(128 * 1024, 'x'), true);
  std::atomic_size_t received = 0;
  std::string error;
  assert(!archive_file::readArchiveEntriesConcurrently(path, {"payload.bin"},
      [&](archive_file::FileData &&) { ++received; return true; }, 2, 1024, &error));
  assert(received == 0 && !error.empty());
}

void testSerialZipUnzipDoesNotMaterializeLargeMembers() {
  TempDirectory temporary;
  const auto path = temporary.path() / "large.zip";
  writeStoredZipContents(path, "payload.bin", std::string(4 * 1024 * 1024, 'x'), true);
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 1, .maximumMemoryBytes = 1024 * 1024}};
  bounded_allocation_probe::largest = 0;
  bounded_allocation_probe::enabled = true;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, nullptr, nullptr, true, nullptr, &budget);
  bounded_allocation_probe::enabled = false;
  assert(result && std::filesystem::file_size(result->outputFolder / "payload.bin") == 4 * 1024 * 1024);
  assert(bounded_allocation_probe::largest < 1024 * 1024);
}

void testFullUnzipCountsEntriesAndImplicitDirectories() {
  TempDirectory temporary;
  const auto path = temporary.path() / "deep.zip";
  writeStoredZipContents(path, "first/second/empty.bin", "");
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveEntries = 2}};
  std::string error;
  bool prepared = false;
  const auto rejected = archive_file::unzipArchiveFully(path, temporary.path() / "rejected", &error,
      nullptr, nullptr, nullptr, true, [&](const auto &, const auto &) { prepared = true; return true; }, &budget);
  assert(!rejected && !prepared && error.find("entry-count") != std::string::npos);
  archive_file::UnzipBudget shared{.limits = {.maximumArchiveEntries = 3, .maximumTotalEntries = 5}};
  const auto first = archive_file::unzipArchiveFully(path, temporary.path() / "accepted", &error,
      nullptr, nullptr, nullptr, true, nullptr, &shared);
  assert(first && shared.admittedEntries == 3);
  const auto reused = archive_file::unzipArchiveFully(path, temporary.path() / "accepted", &error,
      nullptr, nullptr, nullptr, true, nullptr, &shared);
  assert(reused && shared.admittedEntries == 3);
  const auto second = archive_file::unzipArchiveFully(path, temporary.path() / "second", &error,
      nullptr, nullptr, nullptr, true, nullptr, &shared);
  assert(!second && shared.admittedEntries == 3 && shared.exhausted);
  assert(std::filesystem::exists(path));
}

void testFullUnzipCountsExplicitDirectoriesAndEmptyFiles(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("empty-entries" + extension);
  auto writer = makeArchiveWriteHandle();
  assert((extension == ".zip" ? archive_write_set_format_zip(writer.get()) :
      extension == ".7z" ? archive_write_set_format_7zip(writer.get()) :
      archive_write_set_format_pax_restricted(writer.get())) == ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK);
  for (const auto *name : {"dir/", "dir/empty.bin", "other.bin"}) {
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name);
    archive_entry_set_filetype(entry.get(), std::string_view(name).ends_with('/') ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(entry.get(), 0755);
    archive_entry_set_size(entry.get(), 0);
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
  std::string error;
  archive_file::UnzipBudget exact{.limits = {.maximumArchiveEntries = 3, .maximumTotalEntries = 3}};
  const auto accepted = archive_file::unzipArchiveFully(path, temporary.path() / "exact", &error,
      nullptr, nullptr, nullptr, true, nullptr, &exact);
  assert(accepted && exact.admittedEntries == 3 && accepted->fileCount == 2);
  archive_file::UnzipBudget small{.limits = {.maximumArchiveEntries = 2}};
  bool prepared = false;
  assert(!archive_file::unzipArchiveFully(path, temporary.path() / "small", &error,
      nullptr, nullptr, nullptr, true, [&](const auto &, const auto &) { prepared = true; return true; }, &small));
  assert(!prepared && error.find("entry-count") != std::string::npos);
}

void testFullUnzipPreservesUnownedHashedFallback() {
  TempDirectory temporary;
  const auto path = temporary.path() / "packed.zip";
  const auto root = temporary.path() / "output";
  writeStoredZipContents(path, "payload.bin", "archive payload");
  for (int candidate = 1; candidate <= 100; ++candidate) {
    std::filesystem::create_directories(root / (candidate == 1 ? "packed" : "packed " + std::to_string(candidate)));
  }
  std::string error;
  const auto first = archive_file::unzipArchiveFully(path, root, &error);
  assert(first);
  std::ofstream(first->outputFolder / "precious.txt") << "unrelated user data";
  std::filesystem::remove(first->outputFolder / ".asobmashow_unzip_complete");
  const auto second = archive_file::unzipArchiveFully(path, root, &error);
  assert(!second || second->outputFolder != first->outputFolder);
  std::ifstream preserved(first->outputFolder / "precious.txt");
  assert(std::string((std::istreambuf_iterator<char>(preserved)), {}) == "unrelated user data");
}

void testFullUnzipRejectsReservedRootNames() {
  for (const std::string name : {".asobmashow_unzip_incomplete", ".asobmashow_unzip_complete",
       ".asobmashow_unzip_complete.tmp", ".ASOBMASHOW_UNZIP_COMPLETE",
       ".asobmashow_unzip_incomplete/child.bin", ".asobmashow_unzip_complete. "}) {
    TempDirectory temporary;
    const auto path = temporary.path() / "reserved.zip";
    writeStoredZipContents(path, name, "archive payload");
    bool prepared = false;
    std::string error;
    const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
        nullptr, nullptr, nullptr, true, [&](const auto &, const auto &) { prepared = true; return true; });
    assert(!result && !prepared && error.find("reserved") != std::string::npos);
    assert(std::filesystem::exists(path));
  }
  TempDirectory temporary;
  const auto path = temporary.path() / "nested.zip";
  writeStoredZipContents(path, "song/.asobmashow_unzip_complete", "ordinary nested file");
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error);
  assert(result);
  std::ifstream nested(result->outputFolder / "song/.asobmashow_unzip_complete");
  assert(std::string((std::istreambuf_iterator<char>(nested)), {}) == "ordinary nested file");
}

void testFullUnzipIncompleteMarkerRecordsOwnership() {
  TempDirectory temporary;
  const auto path = temporary.path() / "cancelled.zip";
  writeStoredZipContents(path, "payload.bin", "archive payload");
  std::stop_source stop;
  const auto token = stop.get_token();
  std::filesystem::path folder;
  std::string key;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      &token, [&](const archive_file::UnzipProgress &progress) {
        if (progress.current > 0) stop.request_stop();
      }, nullptr, true, [&](const auto &destination, const auto &identity) {
        folder = destination;
        key = identity;
        return true;
      });
  assert(!result && stop.stop_requested());
  std::ifstream marker(folder / ".asobmashow_unzip_incomplete");
  std::string actualKey, actualPath;
  assert(std::getline(marker, actualKey) && std::getline(marker, actualPath));
  assert(actualKey == key && std::filesystem::path(actualPath) == path);
}

void testUnzipMarkerMatchesRelocatedContainer() {
  TempDirectory temporary;
  const auto previous = temporary.path() / "old-container/Documents/archive.zip";
  const auto current = temporary.path() / "new-container/Documents/archive.zip";
  const auto folder = temporary.path() / "output";
  std::filesystem::create_directory(folder);
  std::ofstream(folder / ".asobmashow_unzip_incomplete") << "identity\n" << previous.string() << '\n';
  archive_file::setCachePathNormalizer([&](std::filesystem::path &path) {
    if (path == previous || path == current) path = "Documents/archive.zip";
  });
  const bool matches = archive_file::unzipFolderHasMatchingIncompleteMarker(folder, current, "identity");
  archive_file::setCachePathNormalizer({});
  assert(matches);
}

void testFullUnzipRejectsBudgetBeforePreparingOutput() {
  TempDirectory temporary;
  const auto path = temporary.path() / "oversized.zip";
  writeStoredZipContents(path, "payload.bin", std::string(1024, 'x'), true);
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = 1023}};
  bool prepared = false;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, nullptr, true,
      [&](const auto &, const auto &) { prepared = true; return true; }, &budget);
  assert(!result && !prepared && budget.exhausted && budget.writtenBytes == 0);
  assert(error.find("expanded-byte limit") != std::string::npos);
}

void writeFullUnzipBudgetFixture(const std::filesystem::path &path, std::size_t bytes = 1024) {
  auto writer = makeArchiveWriteHandle();
  const auto extension = path.extension();
  const int format = extension == ".7z" ? archive_write_set_format_7zip(writer.get()) :
      extension == ".zip" ? archive_write_set_format_zip(writer.get()) :
                            archive_write_set_format_pax_restricted(writer.get());
  assert(format == ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK);
  const std::string payload(bytes, 'x');
  for (const auto *name : {"first.bin", "second.bin"}) {
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name);
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), payload.size());
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(bytes));
    assert(archive_write_finish_entry(writer.get()) == ARCHIVE_OK);
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
}

void testSingleArchiveOverlapsDecodingAndWriting(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("pipeline" + extension);
  writeFullUnzipBudgetFixture(path, 1024 * 1024);
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 2}};
  std::set<std::thread::id> threads;
  std::mutex mutex;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, [&] {
        std::lock_guard lock(mutex);
        threads.insert(std::this_thread::get_id());
        return true;
      }, false, nullptr, &budget);
  assert(result && threads.size() == 2 && budget.writtenBytes == 2 * 1024 * 1024);
  for (const auto *name : {"first.bin", "second.bin"}) {
    std::ifstream input(result->outputFolder / name, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    assert(bytes == std::string(1024 * 1024, 'x'));
  }
}

void testParallelZipPreservesUnsupportedCompressionFallback() {
  constexpr unsigned char fixture[] = {
      0x50,0x4b,0x03,0x04,0x2e,0x00,0x00,0x00,0x0c,0x00,0x23,0x8f,0x2b,0x5d,0xb9,0x97,
      0x55,0x7c,0x2d,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x09,0x00,0x00,0x00,0x66,0x69,
      0x72,0x73,0x74,0x2e,0x62,0x69,0x6e,0x42,0x5a,0x68,0x39,0x31,0x41,0x59,0x26,0x53,
      0x59,0x51,0xd4,0xf6,0x50,0x00,0x00,0x04,0x41,0x00,0xc0,0x00,0x20,0x00,0x00,0x08,
      0x20,0x00,0x30,0xcc,0x05,0x53,0x6a,0x62,0x28,0x3c,0x5d,0xc9,0x14,0xe1,0x42,0x41,
      0x47,0x53,0xd9,0x40,0x50,0x4b,0x03,0x04,0x14,0x00,0x00,0x00,0x08,0x00,0x23,0x8f,
      0x2b,0x5d,0xec,0x0a,0x0d,0x43,0x0b,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x0a,0x00,
      0x00,0x00,0x73,0x65,0x63,0x6f,0x6e,0x64,0x2e,0x62,0x69,0x6e,0x4b,0x4a,0x1a,0x05,
      0xa3,0x60,0x14,0x8c,0x54,0x00,0x00,0x50,0x4b,0x01,0x02,0x2e,0x03,0x2e,0x00,0x00,
      0x00,0x0c,0x00,0x23,0x8f,0x2b,0x5d,0xb9,0x97,0x55,0x7c,0x2d,0x00,0x00,0x00,0x00,
      0x04,0x00,0x00,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,
      0x01,0x00,0x00,0x00,0x00,0x66,0x69,0x72,0x73,0x74,0x2e,0x62,0x69,0x6e,0x50,0x4b,
      0x01,0x02,0x14,0x03,0x14,0x00,0x00,0x00,0x08,0x00,0x23,0x8f,0x2b,0x5d,0xec,0x0a,
      0x0d,0x43,0x0b,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x0a,0x00,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x54,0x00,0x00,0x00,0x73,0x65,0x63,0x6f,
      0x6e,0x64,0x2e,0x62,0x69,0x6e,0x50,0x4b,0x05,0x06,0x00,0x00,0x00,0x00,0x02,0x00,
      0x02,0x00,0x6f,0x00,0x00,0x00,0x87,0x00,0x00,0x00,0x00,0x00};
  TempDirectory temporary;
  for (const bool reversed : {false, true}) {
    const auto path = temporary.path() / (reversed ? "reversed.zip" : "mixed.zip");
    std::vector<unsigned char> contents(std::begin(fixture), std::end(fixture));
    if (reversed) {
      contents.clear();
      contents.insert(contents.end(), fixture + 84, fixture + 135);
      contents.insert(contents.end(), fixture, fixture + 84);
      contents.insert(contents.end(), fixture + 190, fixture + 246);
      contents.insert(contents.end(), fixture + 135, fixture + 190);
      contents.insert(contents.end(), fixture + 246, std::end(fixture));
      writeLeU32(contents.data() + 135 + 42, 0);
      writeLeU32(contents.data() + 191 + 42, 51);
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(contents.data()), contents.size());
    output.close();
    for (std::size_t workers : {1, 4}) {
      archive_file::UnzipBudget budget{.limits = {.maximumWorkers = workers}};
      std::string error;
      const auto result = archive_file::unzipArchiveFully(
          path, temporary.path() / (std::to_string(workers) + (reversed ? "-reversed" : "")), &error, nullptr,
          nullptr, nullptr, false, nullptr, &budget);
      assert(result && budget.writtenBytes == 2048);
      for (const auto &[name, value] : {std::pair{"first.bin", 'a'}, {"second.bin", 'b'}}) {
        std::ifstream input(result->outputFolder / name, std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(input)), {});
        assert(actual == std::string(1024, value));
      }
    }
  }
}

void testSingleEntryZipPreservesIndexedFilename() {
  static constexpr unsigned char fixture[] = {
      0x50,0x4b,0x03,0x04,0x14,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x21,0x00,0x63,0xf0,
      0xd7,0x48,0x0b,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x0a,0x00,0x13,0x00,0x6c,0x69,
      0x73,0x74,0x65,0x64,0x2e,0x62,0x69,0x6e,0x75,0x70,0x0f,0x00,0x01,0xaa,0xcd,0x69,
      0xc3,0x6d,0x61,0x70,0x70,0x65,0x64,0x2e,0x62,0x69,0x6e,0xab,0xa8,0x18,0x05,0xa3,
      0x60,0x14,0x8c,0x54,0x00,0x00,0x50,0x4b,0x01,0x02,0x14,0x03,0x14,0x00,0x00,0x00,
      0x08,0x00,0x00,0x00,0x21,0x00,0x63,0xf0,0xd7,0x48,0x0b,0x00,0x00,0x00,0x00,0x04,
      0x00,0x00,0x0a,0x00,0x13,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,
      0x00,0x00,0x00,0x00,0x6c,0x69,0x73,0x74,0x65,0x64,0x2e,0x62,0x69,0x6e,0x75,0x70,
      0x0f,0x00,0x01,0xaa,0xcd,0x69,0xc3,0x6d,0x61,0x70,0x70,0x65,0x64,0x2e,0x62,0x69,
      0x6e,0x50,0x4b,0x05,0x06,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x4b,0x00,0x00,
      0x00,0x46,0x00,0x00,0x00,0x00,0x00};
  TempDirectory temporary;
  const auto path = temporary.path() / "unicode-path.zip";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(fixture), sizeof(fixture));
  output.close();
  for (std::size_t workers : {1, 4}) {
    archive_file::UnzipBudget budget{.limits = {.maximumWorkers = workers}};
    std::string error;
    const auto result = archive_file::unzipArchiveFully(
        path, temporary.path() / std::to_string(workers), &error, nullptr,
        nullptr, nullptr, false, nullptr, &budget);
    assert(result && budget.writtenBytes == 1024);
    std::ifstream input(result->outputFolder / "listed.bin", std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)), {});
    assert(actual == std::string(1024, 'x'));
    assert(!std::filesystem::exists(result->outputFolder / "mapped.bin"));
  }
}

void testFullSevenZipKeepsCompressionBlocksTogether(const unsigned char *fixture,
                                                    std::size_t fixtureSize,
                                                    std::size_t workers,
                                                    std::size_t expectedThreads,
                                                    std::uint64_t memory = 256ull * 1024 * 1024,
                                                    bool verifyPreparedHeaders = false) {
  TempDirectory temporary;
  const auto path = temporary.path() / "blocks.7z";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(fixture), fixtureSize);
  output.close();
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = 2 * 1024 * 1024,
      .maximumTotalBytes = 2 * 1024 * 1024, .maximumWorkers = workers,
      .maximumMemoryBytes = memory}};
  std::set<std::thread::id> threads;
  std::uint64_t completed = 0;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, [&](const archive_file::UnzipProgress &progress) {
        if (progress.fraction > 0.08 && progress.fraction < 0.98 && progress.current > 0) {
          if (verifyPreparedHeaders && completed == 0) {
            std::size_t prepared = 0;
            for (const auto &line : archive_file::debugLogLines()) {
              if (line.find("Prepared full 7-Zip extraction worker: " + path.string()) != std::string::npos) ++prepared;
            }
            assert(prepared == expectedThreads);
          }
          assert(progress.current >= completed && progress.total == 9);
          completed = progress.current;
          threads.insert(std::this_thread::get_id());
        }
      }, nullptr, false, nullptr, &budget);
  assert(result && result->fileCount == 9 && completed == 9);
  assert(budget.writtenBytes == 2 * 1024 * 1024 && budget.pendingWriteBytes == 0);
  assert(std::filesystem::is_directory(result->outputFolder / "emptydir"));
  assert(std::filesystem::file_size(result->outputFolder / "empty.bin") == 0);
  for (std::size_t index = 0; index < 8; ++index) {
    std::ifstream input(result->outputFolder / ("file" + std::to_string(index) + ".bin"), std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)), {});
    assert(actual == std::string(256 * 1024, static_cast<char>('a' + index / 2)));
  }
  std::cerr << "7z extraction progress threads: " << threads.size() << " expected=" << expectedThreads << '\n';
  assert(threads.size() == expectedThreads);
}

void testFullSevenZipDictionaryAdmission(unsigned char dictionary, std::size_t expectedThreads) {
  std::vector<unsigned char> bytes(std::begin(archive_sevenzip_fixtures::blocksLzma2),
                                   std::end(archive_sevenzip_fixtures::blocksLzma2));
  for (std::size_t offset : {0x293, 0x298, 0x29d, 0x2a2}) {
    assert(bytes[offset] == 0x10);
    bytes[offset] = dictionary;
  }
  const auto checksum = [&](std::size_t begin, std::size_t end) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t offset = begin; offset < end; ++offset) {
      crc ^= bytes[offset];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
  };
  writeLeU32(bytes.data() + 28, checksum(32 + readLeU32(bytes.data() + 12), bytes.size()));
  writeLeU32(bytes.data() + 8, checksum(12, 32));
  testFullSevenZipKeepsCompressionBlocksTogether(bytes.data(), bytes.size(), 4, expectedThreads);
}

void testFullRarUsesIndependentEntryWorkers(bool rar4, bool solid, std::size_t workers,
                                           std::uint64_t memory = 256ull * 1024 * 1024,
                                           bool mixedVersions = false) {
  TempDirectory temporary;
  const auto path = temporary.path() / "parallel.rar";
  const auto *bytes = rar4 ? archive_rar_fixtures::rar4 :
      solid ? archive_rar_fixtures::solid : archive_rar_fixtures::nonSolid;
  const auto size = rar4 ? sizeof(archive_rar_fixtures::rar4) : sizeof(archive_rar_fixtures::nonSolid);
  std::vector<unsigned char> fixture(bytes, bytes + size);
  if (mixedVersions) {
    const std::size_t header = 90;
    fixture[header + 24] = 29;
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t offset = header + 2; offset < header + 50; ++offset) {
      crc ^= fixture[offset];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    crc ^= 0xffffffffu;
    fixture[header] = static_cast<unsigned char>(crc);
    fixture[header + 1] = static_cast<unsigned char>(crc >> 8);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(fixture.data()), fixture.size());
  output.close();
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = rar4 ? 48ull : 4ull * 1024 * 1024,
      .maximumTotalBytes = rar4 ? 48ull : 4ull * 1024 * 1024, .maximumWorkers = workers,
      .maximumMemoryBytes = memory}};
  std::set<std::thread::id> threads;
  std::uint64_t completed = 0;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, [&](const archive_file::UnzipProgress &progress) {
        if (progress.fraction > 0.08 && progress.fraction < 0.98 && progress.current > 0) {
          assert(progress.current >= completed);
          completed = progress.current;
          threads.insert(std::this_thread::get_id());
        }
      }, nullptr, false, nullptr, &budget);
  assert(result);
  std::cerr << "RAR workers: version=" << (rar4 ? 4 : 5) << " solid=" << solid
            << " requested=" << workers << " observed=" << threads.size() << '\n';
  assert(threads.size() == (solid || mixedVersions || workers == 1 || (rar4 && memory < 640ull * 1024 * 1024)
      ? 1 : rar4 ? 3 : workers - 1));
  const std::size_t fileCount = rar4 ? 3 : 4;
  assert(completed == fileCount);
  assert(budget.writtenBytes == (rar4 ? 48 : 4 * 1024 * 1024));
  assert(budget.pendingWriteBytes == 0);
  for (std::size_t index = 0; index < fileCount; ++index) {
    const std::string name = rar4 ? index == 0 ? "test.txt" :
        index == 1 ? "testlink" : "testdir/test.txt" : "file" + std::to_string(index) + ".bin";
    std::ifstream input(result->outputFolder / name, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(input)), {});
    const std::string expected = rar4 ? index == 1 ? "test.txt" : "test text document\r\n" :
        std::string(1024 * 1024, static_cast<char>('a' + index));
    assert(actual == expected);
  }
}

void testFullRarSerializesLargeDictionariesAndAliases(bool alias) {
  TempDirectory temporary;
  const auto path = temporary.path() / "constrained.rar";
  std::vector<unsigned char> bytes(std::begin(archive_rar_fixtures::nonSolid),
                                   std::end(archive_rar_fixtures::nonSolid));
  for (std::size_t header : {25, 123, 221, 319}) {
    if (alias && header != 123) continue;
    if (alias) {
      const std::string name = "file0.bin";
      std::copy(name.begin(), name.end(), bytes.begin() + header + 29);
    } else {
      bytes[header + 26] = 0x5b;
    }
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t offset = header + 4; offset < header + 38; ++offset) {
      crc ^= bytes[offset];
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    writeLeU32(bytes.data() + header, crc ^ 0xffffffffu);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  output.close();
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 4,
      .maximumMemoryBytes = 256ull * 1024 * 1024}};
  std::set<std::thread::id> threads;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, [&](const archive_file::UnzipProgress &progress) {
        if (progress.fraction > 0.08 && progress.fraction < 0.98 && progress.current > 0) {
          threads.insert(std::this_thread::get_id());
        }
      }, nullptr, false, nullptr, &budget);
  assert(result && threads.size() == 1 && budget.writtenBytes == 4 * 1024 * 1024);
  std::ifstream input(result->outputFolder / "file0.bin", std::ios::binary);
  const std::string actual((std::istreambuf_iterator<char>(input)), {});
  assert(actual == std::string(1024 * 1024, alias ? 'b' : 'a'));
}

void testParallelSdkFailurePreservesOriginal(int failureKind, bool sevenZip = false) {
  TempDirectory temporary;
  const auto path = temporary.path() / (sevenZip ? "failure.7z" : "failure.rar");
  const auto *fixture = sevenZip ? archive_sevenzip_fixtures::blocksLzma2 : archive_rar_fixtures::nonSolid;
  const auto size = sevenZip ? sizeof(archive_sevenzip_fixtures::blocksLzma2) : sizeof(archive_rar_fixtures::nonSolid);
  std::vector<unsigned char> bytes(fixture, fixture + size);
  if (failureKind == 2) bytes[sevenZip ? 50 : 70] ^= 0x10;
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  output.close();
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 4,
      .maximumMemoryBytes = 256ull * 1024 * 1024}};
  std::stop_source stop;
  std::atomic_bool pauseCancelled{false};
  const auto token = stop.get_token();
  std::filesystem::path folder;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      &token, [&](const archive_file::UnzipProgress &progress) {
        if (failureKind == 0 && progress.current > 0) stop.request_stop();
        if (failureKind == 4 && progress.current > 0) pauseCancelled = true;
      }, [&] { return !pauseCancelled; }, false, [&](const auto &destination, const auto &) {
        folder = destination;
        if (failureKind == 1) budget.limits.maximumTotalBytes = 1024 * 1024;
        if (failureKind == 3) budget.limits.reservedFreeBytes = std::numeric_limits<std::uint64_t>::max();
        return true;
      }, &budget);
  assert(!result && !error.empty());
  assert(budget.pendingWriteBytes == 0);
  assert(std::filesystem::exists(path));
  assert(std::filesystem::exists(folder / ".asobmashow_unzip_incomplete"));
  assert(!std::filesystem::exists(folder / ".asobmashow_unzip_complete"));
  bool parallelStarted = false;
  for (const auto &line : archive_file::debugLogLines()) {
    parallelStarted = parallelStarted || line.find(std::string("Starting parallel full ") +
        (sevenZip ? "7-Zip" : "RAR") + " unzip: " + path.string()) != std::string::npos;
    assert(line.find("Starting batched full unzip: " + path.string()) == std::string::npos);
  }
  assert(parallelStarted);
  if (sevenZip) assert(budget.writtenBytes <= 2 * 1024 * 1024);
  if (failureKind == 0) assert(error == "Unzip cancelled" && stop.stop_requested());
  if (failureKind == 4) assert(error == "Unzip cancelled" && pauseCancelled);
  if (failureKind == 1) assert(budget.exhausted && budget.writtenBytes <= 1024 * 1024 &&
      error.find("expanded-byte limit") != std::string::npos);
  if (failureKind == 3) assert(budget.exhausted && budget.writtenBytes == 0 &&
      error.find("free-space") != std::string::npos);
}

void testParallelRarDeclinesMismatchedCachedPath() {
  TempDirectory temporary;
  const auto path = temporary.path() / "mismatched.rar";
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(archive_rar_fixtures::rar4), sizeof(archive_rar_fixtures::rar4));
  output.close();
  const auto cacheDirectory = temporary.path() / "index";
  archive_file::setArchiveIndexCacheDirectory(cacheDirectory);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(path, entries, &error) && entries.size() == 3);
  assert(entries.front().path == "test.txt");
  const auto cachePath = std::filesystem::directory_iterator(cacheDirectory)->path();
  {
    std::fstream cache(cachePath, std::ios::binary | std::ios::in | std::ios::out);
    cache.seekg(1);
    std::uint64_t keySize = 0;
    cache.read(reinterpret_cast<char *>(&keySize), sizeof(keySize));
    cache.seekg(static_cast<std::streamoff>(keySize) + 8 + 8 + 1 + 1 + 8 + 8, std::ios::cur);
    cache.seekp(cache.tellg());
    cache.put('r');
    assert(cache.good());
  }
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDirectory);
  assert(archive_file::listEntries(path, entries, &error) && entries.front().path == "rest.txt");
  archive_file::UnzipBudget serialBudget{.limits = {.maximumWorkers = 1}};
  const auto serial = archive_file::unzipArchiveFully(path, temporary.path() / "serial", &error,
      nullptr, nullptr, nullptr, false, nullptr, &serialBudget);
  assert(!serial && std::filesystem::exists(path));
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 4,
      .maximumMemoryBytes = 1024ull * 1024 * 1024}};
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, nullptr, nullptr, false, nullptr, &budget);
  assert(!result && std::filesystem::exists(path));
  assert(!std::filesystem::exists(temporary.path() / "serial/mismatched/.asobmashow_unzip_complete"));
  assert(!std::filesystem::exists(temporary.path() / "output/mismatched/.asobmashow_unzip_complete"));
  for (const auto &line : archive_file::debugLogLines()) {
    assert(line.find("Starting parallel full RAR unzip: " + path.string()) == std::string::npos);
  }
  archive_file::clearArchiveIndexCacheForTesting();
}

void testParallelZipSerializesFilesystemAliases(const std::string &extension) {
  TempDirectory temporary;
  const std::string composed = "\xc3\xa9.bin";
  const std::string decomposed = "e\xcc\x81.bin";
  std::ofstream(temporary.path() / composed).put('x');
  if (!std::filesystem::exists(temporary.path() / decomposed)) return;
  const auto path = temporary.path() / ("aliases" + extension);
  auto writer = makeArchiveWriteHandle();
  assert((extension == ".7z" ? archive_write_set_format_7zip(writer.get()) :
          extension == ".tar" ? archive_write_set_format_pax_restricted(writer.get()) :
                                archive_write_set_format_zip(writer.get())) == ARCHIVE_OK);
  assert(archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK);
  char value = 'a';
  for (const auto &name : {composed, decomposed}) {
    const std::string payload(1024 * 1024, value++);
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), payload.size());
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), payload.data(), payload.size()) == static_cast<la_ssize_t>(payload.size()));
  }
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 2}};
  std::set<std::thread::id> threads;
  std::mutex threadsMutex;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(path, temporary.path() / "output", &error,
      nullptr, [&](const archive_file::UnzipProgress &progress) {
        std::lock_guard lock(threadsMutex);
        if (progress.current > 0) threads.insert(std::this_thread::get_id());
      }, [&] {
        std::lock_guard lock(threadsMutex);
        threads.insert(std::this_thread::get_id());
        return true;
      }, false, nullptr, &budget);
  assert(result && threads.size() == 1);
  std::ifstream input(result->outputFolder / composed, std::ios::binary);
  const std::string actual((std::istreambuf_iterator<char>(input)), {});
  assert(actual == std::string(1024 * 1024, 'b'));
}

void testPipelinedUnzipCancellationDrainsWithoutCompleting(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("cancel-pipeline" + extension);
  writeFullUnzipBudgetFixture(path, 1024 * 1024);
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 2}};
  std::stop_source stop;
  const auto token = stop.get_token();
  const auto caller = std::this_thread::get_id();
  std::filesystem::path output;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, &token, nullptr, [&] {
        if (std::this_thread::get_id() != caller) stop.request_stop();
        return !stop.stop_requested();
      }, false, [&](const auto &folder, const auto &) { output = folder; return true; }, &budget);
  assert(!result && stop.stop_requested() && budget.writtenBytes == 0);
  assert(error == "Unzip cancelled");
  assert(std::filesystem::exists(path));
  assert(std::filesystem::exists(output / ".asobmashow_unzip_incomplete"));
  assert(!std::filesystem::exists(output / ".asobmashow_unzip_complete"));
}

void testFullUnzipRuntimeSpaceCheckPreservesIncompleteOutput(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("space" + extension);
  writeFullUnzipBudgetFixture(path);
  archive_file::UnzipBudget budget{.limits = {.maximumWorkers = 1}};
  std::filesystem::path output;
  bool wroteFile = false;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr,
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.current > 0 && progress.fraction < 0.98) {
          wroteFile = true;
          budget.limits.reservedFreeBytes = std::numeric_limits<std::uint64_t>::max();
        }
      }, nullptr, true,
      [&](const auto &folder, const auto &) { output = folder; return true; }, &budget);
  assert(wroteFile && !result && budget.exhausted && budget.writtenBytes == 1024);
  assert(error.find("free-space") != std::string::npos);
  assert(std::filesystem::exists(path));
  assert(std::filesystem::exists(output / ".asobmashow_unzip_incomplete"));
  assert(!std::filesystem::exists(output / ".asobmashow_unzip_complete"));
}

void testFullUnzipRuntimeByteLimitCannotRestartFallback(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("runtime" + extension);
  writeFullUnzipBudgetFixture(path);
  const auto cacheDirectory = temporary.path() / "index";
  archive_file::setArchiveIndexCacheDirectory(cacheDirectory);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(path, entries, &error) && entries.size() == 2);
  const auto cachePath = std::filesystem::directory_iterator(cacheDirectory)->path();
  {
    std::fstream cache(cachePath, std::ios::binary | std::ios::in | std::ios::out);
    const auto readSize = [&] {
      std::uint64_t size = 0;
      cache.read(reinterpret_cast<char *>(&size), sizeof(size));
      assert(cache.good());
      return size;
    };
    cache.seekg(1);
    const auto keySize = readSize();
    cache.seekg(static_cast<std::streamoff>(keySize) + 8 + 8 + 1 + 1, std::ios::cur);
    assert(readSize() == 2);
    for (int entryIndex = 0; entryIndex < 2; ++entryIndex) {
      const auto pathSize = readSize();
      cache.seekg(static_cast<std::streamoff>(pathSize) + 1, std::ios::cur);
      const auto sizeOffset = cache.tellg();
      cache.seekp(sizeOffset);
      const std::uint64_t understatedSize = 1;
      cache.write(reinterpret_cast<const char *>(&understatedSize), sizeof(understatedSize));
      cache.seekg(sizeOffset + std::streamoff(8 + 8 + 8 + 1));
    }
    assert(cache.good());
  }
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDirectory);
  assert(archive_file::listEntries(path, entries, &error) && entries.size() == 2);
  assert(entries[0].size == 1 && entries[1].size == 1);
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = 1024, .maximumWorkers = 1}};
  std::filesystem::path output;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, nullptr, true,
      [&](const auto &folder, const auto &) {
        output = folder;
        return true;
      }, &budget);
  assert(!result && budget.exhausted && budget.writtenBytes == 1024);
  assert(error.find("expanded-byte limit") != std::string::npos);
  assert(std::filesystem::file_size(output / "first.bin") == 1024);
  assert(!std::filesystem::exists(output / "second.bin") ||
         std::filesystem::file_size(output / "second.bin") == 0);
  assert(std::filesystem::exists(output / ".asobmashow_unzip_incomplete"));
  assert(!std::filesystem::exists(output / ".asobmashow_unzip_complete"));
  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void testFullUnzipExactBudgetAndReuseDoNotChargeEstimatedBytes(const std::string &extension) {
  TempDirectory temporary;
  const auto path = temporary.path() / ("exact" + extension);
  writeFullUnzipBudgetFixture(path);
  archive_file::UnzipBudget budget{
      .limits = {.maximumArchiveBytes = 2048, .maximumTotalBytes = 2048}};
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, nullptr, true,
      nullptr, &budget);
  assert(result && !budget.exhausted && budget.writtenBytes == 2048);
  assert(std::filesystem::file_size(result->outputFolder / "first.bin") == 1024);
  assert(std::filesystem::file_size(result->outputFolder / "second.bin") == 1024);
  const auto reused = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, nullptr, true,
      nullptr, &budget);
  assert(reused && reused->outputFolder == result->outputFolder);
  assert(!budget.exhausted && budget.writtenBytes == 2048);
}

void testFullUnzipUnderstatedSizeCannotExceedRuntimeBudget() {
  TempDirectory temporary;
  const auto path = temporary.path() / "understated.zip";
  writeStoredZipContents(path, "payload.bin", std::string(128 * 1024, 'x'), true);
  assert(understateZipUncompressedSizes(path, "payload.bin", 1));
  archive_file::UnzipBudget budget{.limits = {.maximumArchiveBytes = 1024}};
  std::filesystem::path output;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      path, temporary.path() / "output", &error, nullptr, nullptr, nullptr, true,
      [&](const auto &folder, const auto &) { output = folder; return true; }, &budget);
  assert(!result && !output.empty());
  assert(budget.writtenBytes <= 1024);
  assert(!std::filesystem::exists(output / ".asobmashow_unzip_complete"));
  const auto payload = output / "payload.bin";
  assert(!std::filesystem::exists(payload) || std::filesystem::file_size(payload) <= 1024);
}

void testFullUnzipHonorsPauseDuringExtraction() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "pause-during-unzip.zip";
  writeStoredZip(archivePath,
                 {"folder/first.bms", "folder/second.bms",
                  "folder/third.bms"});

  bool wroteFirstFile = false;
  int pauseCalls = 0;
  std::string error;
  const auto result = archive_file::unzipArchiveFully(
      archivePath, temporary.path() / "output", &error, nullptr,
      [&](const archive_file::UnzipProgress &progress) {
        if (progress.current > 0) wroteFirstFile = true;
      },
      [&] {
        ++pauseCalls;
        return !wroteFirstFile;
      });

  assert(!result.has_value());
  assert(wroteFirstFile);
  assert(pauseCalls > 0);
  assert(error == "Unzip cancelled");
}

void testArchiveIndexRejectsPreservedMetadataReplacement(bool coldCache,
                                                        bool replaceFile) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "source.zip";
  const auto replacementPath = temporary.path() / "replacement.zip";
  const auto cacheDir = temporary.path() / "indexes";
  writeStoredZip(archivePath, {"old.wav"});
  writeStoredZip(replacementPath, {"new.wav"});
  const auto size = std::filesystem::file_size(archivePath);
  assert(std::filesystem::file_size(replacementPath) == size);
  const auto modified = std::filesystem::last_write_time(archivePath);
  const auto oldKey = archive_file::cacheKeyForPath(archivePath);
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  std::vector<archive_file::Entry> entries;
  std::string error;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 1 && entries.front().path == "old.wav");
  if (replaceFile) {
    std::filesystem::rename(replacementPath, archivePath);
  } else {
    std::filesystem::copy_file(replacementPath, archivePath,
                               std::filesystem::copy_options::overwrite_existing);
  }
  std::filesystem::last_write_time(archivePath, modified);
  assert(archive_file::cacheKeyForPath(archivePath) == oldKey);
  if (coldCache) {
    archive_file::clearArchiveIndexCacheForTesting();
    archive_file::setArchiveIndexCacheDirectory(cacheDir);
  }
  entries.clear();
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 1 && entries.front().path == "new.wav");
  archive_file::clearArchiveIndexCacheForTesting();
}

void testArchiveIndexPersistsAcrossColdCacheRestart() {
  constexpr int kEntryCount = 20;
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "persist.zip";
  std::vector<std::string> entryPaths;
  entryPaths.reserve(kEntryCount);
  for (int index = 0; index < kEntryCount; ++index) {
    entryPaths.push_back("folder/entry-" + std::to_string(index) + ".bms");
  }
  writeStoredZip(archivePath, entryPaths);
  const auto cacheDir = temporary.path() / "idx";
  std::filesystem::create_directories(cacheDir);

  archive_file::setArchiveIndexCacheDirectory(cacheDir);

  std::vector<archive_file::Entry> firstEntries;
  std::string error;
  assert(archive_file::listEntries(archivePath, firstEntries, &error));
  assert(firstEntries.size() == kEntryCount);

  // Simulate a cold restart: drop the in-memory index and re-list. The
  // persisted index file should be reloaded from disk (same size/mtime),
  // reproducing the same entry count without rebuilding from the archive.
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  std::vector<archive_file::Entry> reloadedEntries;
  assert(archive_file::listEntries(archivePath, reloadedEntries, &error));
  assert(reloadedEntries.size() == kEntryCount);

  // A changed archive (different mtime) must not trust the stale disk index;
  // it rebuilds and still yields the correct count.
  std::error_code touchError;
  std::filesystem::last_write_time(
      archivePath, std::filesystem::file_time_type(
                       std::filesystem::last_write_time(archivePath) +
                       std::chrono::seconds(2)),
      touchError);
  assert(!touchError);
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  std::vector<archive_file::Entry> rebuiltEntries;
  assert(archive_file::listEntries(archivePath, rebuiltEntries, &error));
  assert(rebuiltEntries.size() == kEntryCount);

  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void testArchiveIndexPrunesOrphanedCacheFiles() {
  TempDirectory temporary;
  const auto cacheDir = temporary.path() / "idx";
  std::filesystem::create_directories(cacheDir);
  archive_file::setArchiveIndexCacheDirectory(cacheDir);

  const auto keepArchive = temporary.path() / "keep.zip";
  const auto removedArchive = temporary.path() / "removed.zip";
  writeStoredZip(keepArchive, {"keep/a.bms"});
  writeStoredZip(removedArchive, {"removed/a.bms"});

  std::string error;
  std::vector<archive_file::Entry> entries;
  assert(archive_file::listEntries(keepArchive, entries, &error));
  assert(archive_file::listEntries(removedArchive, entries, &error));

  // Both archives have cache files.
  std::size_t before = 0;
  for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
    if (entry.is_regular_file()) {
      ++before;
    }
  }
  assert(before == 2);

  // Prune with only the still-present archive listed: removed.zip's cache file
  // is dropped, keep.zip's is retained.
  const std::size_t pruned =
      archive_file::pruneArchiveIndexCache({keepArchive});
  assert(pruned == 1);

  std::size_t after = 0;
  for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
    if (entry.is_regular_file()) {
      ++after;
    }
  }
  assert(after == 1);

  // The surviving archive still reloads from disk after clearing memory.
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  std::vector<archive_file::Entry> reloaded;
  assert(archive_file::listEntries(keepArchive, reloaded, &error));
  assert(reloaded.size() == 1);

  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void testArchiveIndexPrunesOrphanedTmpCacheFiles() {
  TempDirectory temporary;
  const auto cacheDir = temporary.path() / "idx";
  std::filesystem::create_directories(cacheDir);
  archive_file::setArchiveIndexCacheDirectory(cacheDir);

  const auto keepArchive = temporary.path() / "keep.zip";
  const auto removedArchive = temporary.path() / "removed.zip";
  writeStoredZip(keepArchive, {"keep/a.bms"});
  writeStoredZip(removedArchive, {"removed/a.bms"});

  std::string error;
  std::vector<archive_file::Entry> entries;
  assert(archive_file::listEntries(keepArchive, entries, &error));
  assert(archive_file::listEntries(removedArchive, entries, &error));

  // Simulate the orphans a crash between the temporary write and the rename in
  // the index writer leaves behind: a .idx.tmp sibling for every persisted
  // index, plus one whose hash no live archive produces.
  std::size_t idxFiles = 0;
  for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
    std::error_code typeError;
    if (entry.is_regular_file(typeError) && !typeError &&
        entry.path().extension() == ".idx") {
      ++idxFiles;
      std::error_code copyError;
      std::filesystem::copy_file(entry.path(),
                                 entry.path().string() + ".tmp", copyError);
      assert(!copyError);
    }
  }
  assert(idxFiles == 2);
  std::ofstream(cacheDir / "archive-index-0000000000000000.idx.tmp",
                std::ios::binary)
      << "stale";

  // Prune with only the still-present archive listed: the removed archive's
  // .idx and both of its .idx.tmp orphans are dropped. The live archive's .idx
  // is retained and its .idx.tmp sibling is left alone (the real .idx is
  // authoritative).
  const std::size_t pruned =
      archive_file::pruneArchiveIndexCache({keepArchive});
  assert(pruned == 3);
  assert(std::filesystem::exists(
      cacheDir / "archive-index-0000000000000000.idx.tmp") == false);

  std::size_t remaining = 0;
  for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
    if (entry.is_regular_file()) {
      ++remaining;
    }
  }
  assert(remaining == 2);

  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void testArchiveIndexPruningPreservesShortUnrelatedFiles() {
  bool threw = false;
  for (const std::string fileName : {"short", "sixsix", "seven77"}) {
    TempDirectory temporary;
    const auto cacheDir = temporary.path() / "idx";
    std::filesystem::create_directories(cacheDir);
    archive_file::setArchiveIndexCacheDirectory(cacheDir);
    std::ofstream(cacheDir / fileName) << "unrelated";
    try {
      assert(archive_file::pruneArchiveIndexCache({}) == 0);
    } catch (const std::out_of_range &error) {
      std::cerr << "FAIL: pruning unrelated filename of length "
                << fileName.size() << " threw: " << error.what() << '\n';
      threw = true;
    }
    std::ifstream preserved(cacheDir / fileName);
    std::string content;
    preserved >> content;
    assert(content == "unrelated");
    archive_file::setArchiveIndexCacheDirectory({});
    archive_file::clearArchiveIndexCacheForTesting();
  }
  assert(!threw);
}

void testCorruptIndexEntryCountIsRejected() {
  TempDirectory temporary;
  const auto archivePath = temporary.path() / "corrupt-count.zip";
  writeStoredZip(archivePath, {"folder/a.bms", "folder/b.bms"});
  const auto cacheDir = temporary.path() / "idx";
  std::filesystem::create_directories(cacheDir);
  archive_file::setArchiveIndexCacheDirectory(cacheDir);

  std::string error;
  std::vector<archive_file::Entry> entries;
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 2);

  std::filesystem::path cacheFile;
  for (const auto &entry : std::filesystem::directory_iterator(cacheDir)) {
    std::error_code typeError;
    if (entry.is_regular_file(typeError) && !typeError &&
        entry.path().extension() == ".idx") {
      cacheFile = entry.path();
      break;
    }
  }
  assert(!cacheFile.empty());

  std::ifstream input(cacheFile, std::ios::binary);
  assert(input);
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  input.close();
  // Layout: version(1) + keyLen(8) + key + size(8) + mtime(8) + backend(1) +
  // sevenZipFormat(1) + entryCount(8). Overwrite the entry count with a value
  // far larger than the file could ever hold; the loader must reject it
  // instead of allocating an unbounded entry vector.
  assert(bytes.size() >= 1 + 8 + 8 + 8 + 1 + 1 + 8);
  const std::uint64_t keyLen =
      static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[1])) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[2])) << 8) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[3])) << 16) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[4])) << 24) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[5])) << 32) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[6])) << 40) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[7])) << 48) |
      (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[8])) << 56);
  const std::size_t entryCountOffset = 1 + 8 + static_cast<std::size_t>(keyLen) +
                                       8 + 8 + 1 + 1;
  assert(entryCountOffset + 8 <= bytes.size());
  bytes[entryCountOffset + 0] = '\xff';
  bytes[entryCountOffset + 1] = '\xff';
  bytes[entryCountOffset + 2] = '\xff';
  bytes[entryCountOffset + 3] = '\xff';
  bytes[entryCountOffset + 4] = '\xff';
  bytes[entryCountOffset + 5] = '\xff';
  bytes[entryCountOffset + 6] = '\xff';
  bytes[entryCountOffset + 7] = '\xff';
  {
    std::ofstream output(cacheFile, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
  }

  // A corrupt index is discarded and the archive is re-listed instead of
  // crashing on an unbounded reserve.
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::setArchiveIndexCacheDirectory(cacheDir);
  entries.clear();
  error.clear();
  assert(archive_file::listEntries(archivePath, entries, &error));
  assert(entries.size() == 2);

  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
}

void testSingleFlightWaiterCancellation(bool completeBuilderDuringWaiterCallback) {
  TempDirectory temporary;
  const auto archivePath = temporary.path() /
      (completeBuilderDuringWaiterCallback ? "completed-builder.zip"
                                          : "paused-builder.zip");
  const auto callbackArchivePath = temporary.path() / "callback.zip";
  writeStoredZip(archivePath, {"song.bms"});
  writeStoredZip(callbackArchivePath, {"callback.bms"});
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::resetSingleFlightWaiterCountForTesting();

  std::promise<void> builderPaused;
  auto builderPausedFuture = builderPaused.get_future();
  std::promise<void> resumeBuilder;
  auto resumeBuilderFuture = resumeBuilder.get_future().share();
  auto builder = std::async(std::launch::async, [&] {
    bool paused = false;
    std::vector<archive_file::Entry> entries;
    std::string error;
    const bool listed = archive_file::listEntries(
        archivePath, entries, &error, [&] {
          if (!paused) {
            paused = true;
            builderPaused.set_value();
            assert(resumeBuilderFuture.wait_for(10s) ==
                   std::future_status::ready);
          }
          return true;
        });
    assert(listed);
    assert(error.empty());
    assert(entries.size() == 1);
    assert(entries.front().path == "song.bms");
  });
  assert(builderPausedFuture.wait_for(10s) == std::future_status::ready);

  auto healthyWaiter = std::async(std::launch::async, [&] {
    std::vector<archive_file::Entry> entries;
    std::string error;
    const bool listed = archive_file::listEntries(archivePath, entries, &error);
    assert(listed);
    assert(error.empty());
    assert(entries.size() == 1);
    assert(entries.front().path == "song.bms");
  });
  std::atomic_bool cancelWaiter{false};
  std::promise<void> waiterPaused;
  auto waiterPausedFuture = waiterPaused.get_future();
  std::promise<void> resumeWaiter;
  auto resumeWaiterFuture = resumeWaiter.get_future().share();
  std::vector<archive_file::Entry> cancelledEntries;
  std::string cancelledError;
  auto cancelledWaiter = std::async(std::launch::async, [&] {
    return archive_file::listEntries(
        archivePath, cancelledEntries, &cancelledError, [&] {
          if (!cancelWaiter.load(std::memory_order_acquire)) {
            return true;
          }
          waiterPaused.set_value();
          assert(resumeWaiterFuture.wait_for(10s) == std::future_status::ready);
          std::vector<archive_file::Entry> callbackEntries;
          std::string callbackError;
          const bool listed = archive_file::listEntries(
              callbackArchivePath, callbackEntries, &callbackError);
          assert(listed);
          assert(callbackError.empty());
          assert(callbackEntries.size() == 1);
          assert(callbackEntries.front().path == "callback.bms");
          return false;
        });
  });

  const auto registrationDeadline = std::chrono::steady_clock::now() + 10s;
  while (archive_file::singleFlightWaiterCountForTesting() < 2 &&
         std::chrono::steady_clock::now() < registrationDeadline) {
    std::this_thread::sleep_for(1ms);
  }
  assert(archive_file::singleFlightWaiterCountForTesting() == 2);
  cancelWaiter.store(true, std::memory_order_release);
  bool cancelledWhileBuilderPaused = false;
  if (completeBuilderDuringWaiterCallback) {
    assert(waiterPausedFuture.wait_for(10s) == std::future_status::ready);
    resumeBuilder.set_value();
    assert(builder.wait_for(10s) == std::future_status::ready);
    assert(healthyWaiter.wait_for(10s) == std::future_status::ready);
    resumeWaiter.set_value();
  } else {
    resumeWaiter.set_value();
    cancelledWhileBuilderPaused =
        cancelledWaiter.wait_for(2s) == std::future_status::ready;
    assert(builder.wait_for(0ms) == std::future_status::timeout);
    assert(healthyWaiter.wait_for(0ms) == std::future_status::timeout);
    resumeBuilder.set_value();
  }
  assert(builder.wait_for(10s) == std::future_status::ready);
  assert(healthyWaiter.wait_for(10s) == std::future_status::ready);
  assert(cancelledWaiter.wait_for(10s) == std::future_status::ready);
  builder.get();
  healthyWaiter.get();
  const bool cancelledListed = cancelledWaiter.get();
  assert(completeBuilderDuringWaiterCallback || cancelledWhileBuilderPaused);
  assert(!cancelledListed);
  assert(cancelledEntries.empty());
  assert(cancelledError == "Operation cancelled");

  std::vector<archive_file::Entry> cachedEntries;
  std::string cachedError;
  assert(archive_file::listEntries(archivePath, cachedEntries, &cachedError));
  assert(cachedError.empty());
  assert(cachedEntries.size() == 1);
  assert(cachedEntries.front().path == "song.bms");
  const auto logLines = archive_file::debugLogLines();
  const auto indexAttempts = std::count_if(
      logLines.begin(), logLines.end(), [&](const std::string &line) {
        return line.find("Indexing archive:") != std::string::npos &&
               line.find(archivePath.filename().string()) != std::string::npos;
      });
  assert(indexAttempts == 1);
}

void testSingleFlightWaitersDoNotEachReindexAfterFailedBuild() {
  constexpr int kWorkerCount = 6;
  TempDirectory temporary;
  // A file that pretends to be a ZIP but cannot be indexed: every build
  // attempt fails, which is what the single-flight waiters must observe
  // instead of each queueing its own full index build.
  const auto archivePath = temporary.path() / "broken.zip";
  {
    std::ofstream file(archivePath, std::ios::binary);
    file << "not a real zip archive payload";
  }
  archive_file::setArchiveIndexCacheDirectory({});
  archive_file::clearArchiveIndexCacheForTesting();
  archive_file::resetSingleFlightWaiterCountForTesting();

  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    bool builderInsideBuild = false;
  };
  Gate gate;

  // Only the single-flight builder thread reaches a pause callback while
  // inside the index build body. Hold it there until every non-builder worker
  // is deterministically registered inside the single-flight wait (counted by
  // the production test hook), then abort the build. No timing heuristic: the
  // build cannot finish while the callback is blocked, so once the waiter
  // count is complete no worker can still fall through to a second build.
  auto pauseCallback = [&] {
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.builderInsideBuild = true;
    gate.cv.notify_all();
    const bool allWaitersRegistered = gate.cv.wait_for(
        lock, 10s, [&] {
          return archive_file::singleFlightWaiterCountForTesting() >=
                 static_cast<std::uint32_t>(kWorkerCount - 1);
        });
    return allWaitersRegistered;
  };

  std::vector<std::atomic_bool> results(kWorkerCount);
  std::vector<std::thread> workers;
  workers.reserve(kWorkerCount);
  for (int index = 0; index < kWorkerCount; ++index) {
    workers.emplace_back([&, index] {
      {
        std::unique_lock<std::mutex> lock(gate.mutex);
        ++gate.arrived;
        gate.cv.notify_all();
      }
      std::vector<archive_file::Entry> entries;
      std::string error;
      const bool listed = archive_file::listEntries(archivePath, entries,
                                                    &error, pauseCallback);
      results[index].store(listed, std::memory_order_release);
    });
  }

  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    const bool primaryReachedGate = gate.cv.wait_for(lock, 10s, [&] {
      return gate.builderInsideBuild && gate.arrived == kWorkerCount;
    });
    assert(primaryReachedGate);
  }
  for (auto &worker : workers) {
    worker.join();
  }
  for (int index = 0; index < kWorkerCount; ++index) {
    assert(!results[index].load(std::memory_order_acquire));
  }

  // The one aborting builder logged the index attempt; no waiter re-indexed.
  const auto logLines = archive_file::debugLogLines();
  const std::string archiveName = archivePath.filename().string();
  const std::size_t indexAttempts = std::count_if(
      logLines.begin(), logLines.end(), [&](const std::string &line) {
        return line.find("Indexing archive:") != std::string::npos &&
               line.find(archiveName) != std::string::npos;
      });
  assert(indexAttempts == 1);
}

void testDebugLogRetainsNewestThousandLines() {
  for (int index = 0; index <= 1000; ++index) {
    archive_file::appendDebugLogLine("retention-marker-" +
                                     std::to_string(index));
  }

  const auto logLines = archive_file::debugLogLines();
  assert(logLines.size() == 1000);
  assert(logLines.front().find("retention-marker-1") != std::string::npos);
  assert(logLines.back().find("retention-marker-1000") != std::string::npos);
}

} // namespace

int main() {
  testZipIndexAmortizesPausePolling();
  testZipIndexPreservesFilenameBeyondEmbeddedStatBuffer();
  testGameplayBmsResourceAvailabilityPublishesLoaderResult();
  testGameplayBmsResourceAvailabilityResolvesVirtualChartNeighbors();
  testZipIndexRejectsEmbeddedNulInShortFilename();
  testZipIndexPausePollingStillCancelsDuringLargeDirectory();
  testZipIndexUsesCommonSystemEntryFilter();
  testBoundedReadRejectsOversizedIndexedEntryBeforeExtraction();
  testBoundedReadStreamsOrdinaryPlatformPathExactlyOnce();
  testIndependentSevenZipCacheMissesOpenConcurrently();
  testSevenZipArchiveLevelSolidDetection();
  testLegacySolidIndexIsRebuilt();
  // Deliberately registered after the 7-Zip index-count assertion above:
  // these bounded-read tests index real archives, which would otherwise pollute
  // that assertion's retained debug-log window.
  testZipBoundedReadStreamsFullInBoundsEntry();
  testZipBoundedReadAcceptsEmptyStoredEntry();
  testZipBoundedReadRejectsCorruptStoredPayloadWithUnchangedCrc();
  testBoundedReadFallsBackToAlternativeAudioExtension();
  testZipBoundedReadRejectsCentralDirectoryUnderstatedSize();
  testBoundedReadRejectsOversizedSevenZipEntry();
  testBzipZipFallbackStopsBeforeOversizedAllocation();
  testSevenZipReadUsesCurrentOperationPauseCallback();
  testEncodedHeaderSevenZipUsesSdk();
  testDeltaFilteredSevenZipUsesSdk();
  testFullUnzipHonorsPauseDuringExtraction();
  testParallelZipPreservesUnsupportedCompressionFallback();
  testSingleEntryZipPreservesIndexedFilename();
  testFullRarUsesIndependentEntryWorkers(false, false, 1);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::solidLzma2,
      sizeof(archive_sevenzip_fixtures::solidLzma2), 4, 1);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::filteredBlocks,
      sizeof(archive_sevenzip_fixtures::filteredBlocks), 4, 1);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::blocksLzma,
      sizeof(archive_sevenzip_fixtures::blocksLzma), 1, 1);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::blocksLzma,
      sizeof(archive_sevenzip_fixtures::blocksLzma), 4, 3);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::blocksLzma2,
      sizeof(archive_sevenzip_fixtures::blocksLzma2), 4, 3);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::blocksLzma2,
      sizeof(archive_sevenzip_fixtures::blocksLzma2), 4, 1, 128ull * 1024 * 1024);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::blocksLzma2,
      sizeof(archive_sevenzip_fixtures::blocksLzma2), 8, 2, 192ull * 1024 * 1024);
  testFullSevenZipDictionaryAdmission(19, 3);
  testFullSevenZipDictionaryAdmission(28, 1);
  testFullSevenZipKeepsCompressionBlocksTogether(archive_sevenzip_fixtures::compressedHeader,
      sizeof(archive_sevenzip_fixtures::compressedHeader), 4, 3, 256ull * 1024 * 1024, true);
  for (int failureKind = 0; failureKind < 5; ++failureKind) {
    testParallelSdkFailurePreservesOriginal(failureKind, true);
  }
  testFullRarUsesIndependentEntryWorkers(false, true, 4);
  testFullRarUsesIndependentEntryWorkers(false, false, 4);
  testFullRarUsesIndependentEntryWorkers(true, false, 4);
  testFullRarUsesIndependentEntryWorkers(true, false, 4, 1024ull * 1024 * 1024);
  testFullRarUsesIndependentEntryWorkers(true, false, 4, 1024ull * 1024 * 1024, true);
  testParallelRarDeclinesMismatchedCachedPath();
  testFullRarSerializesLargeDictionariesAndAliases(false);
  testFullRarSerializesLargeDictionariesAndAliases(true);
  for (int failureKind = 0; failureKind < 4; ++failureKind) testParallelSdkFailurePreservesOriginal(failureKind);
  for (const auto *extension : {".zip", ".7z", ".tar"}) {
    testParallelZipSerializesFilesystemAliases(extension);
  }
  testPipelinedUnzipCancellationDrainsWithoutCompleting(".7z");
  testPipelinedUnzipCancellationDrainsWithoutCompleting(".tar");
  testSingleArchiveOverlapsDecodingAndWriting(".7z");
  testSingleArchiveOverlapsDecodingAndWriting(".tar");
  testFullUnzipRejectsBudgetBeforePreparingOutput();
  testFullUnzipPreservesUnownedHashedFallback();
  testFullUnzipRejectsReservedRootNames();
  testFullUnzipIncompleteMarkerRecordsOwnership();
  testBoundedStreamingRejectsOversizedPayload();
  testConcurrentReaderRejectsOversizedPayload();
  testSerialZipUnzipDoesNotMaterializeLargeMembers();
  testFullUnzipCountsEntriesAndImplicitDirectories();
  for (const auto *extension : {".zip", ".7z", ".tar"}) testFullUnzipCountsExplicitDirectoriesAndEmptyFiles(extension);
  testUnzipMarkerMatchesRelocatedContainer();
  testFullUnzipUnderstatedSizeCannotExceedRuntimeBudget();
  for (const auto *extension : {".zip", ".7z", ".tar"}) {
    testFullUnzipRuntimeSpaceCheckPreservesIncompleteOutput(extension);
    testFullUnzipRuntimeByteLimitCannotRestartFallback(extension);
    testFullUnzipExactBudgetAndReuseDoNotChargeEstimatedBytes(extension);
  }
  testArchiveIndexPersistsAcrossColdCacheRestart();
  for (const bool coldCache : {true, false}) {
    for (const bool replaceFile : {false, true}) {
      testArchiveIndexRejectsPreservedMetadataReplacement(coldCache, replaceFile);
    }
  }
  testArchiveIndexPrunesOrphanedCacheFiles();
  testArchiveIndexPrunesOrphanedTmpCacheFiles();
  testArchiveIndexPruningPreservesShortUnrelatedFiles();
  testCorruptIndexEntryCountIsRejected();
  testSingleFlightWaiterCancellation(false);
  testSingleFlightWaiterCancellation(true);
  testSingleFlightWaitersDoNotEachReindexAfterFailedBuild();
  for (bool deflated : {false, true}) {
    testZipChunkedReadBoundaryIntegrity(deflated);
    testLargeZipCancellation(deflated, true, false);
    testLargeZipCancellation(deflated, true, true);
    testLargeZipCancellation(deflated, false, false);
  }
  for (bool emptyPrefix : {false, true}) {
    for (int operation = 1; operation < 4; ++operation) {
      testZipCancellationDuringEmptyDeflateBlocks(emptyPrefix, operation);
    }
  }
  testZipInputCallbackRestoresAcrossEntries();
  for (bool sevenZip : {false, true}) {
    for (bool ranged : {false, true}) {
      testBatchCancellationDoesNotRestartFallback(sevenZip, ranged);
    }
  }
  for (int operation = 0; operation < 6; ++operation) {
    testCachedSevenZipHandleWaitCancellation(operation);
  }
  testDebugLogRetainsNewestThousandLines();
  return 0;
}
