#include "../src/ArchiveFile.h"
#include "../src/ArchiveRAII.h"
#include "../src/scene/play/GameplayBmsResourceAvailability.h"

#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
                   const std::string &contents) {
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
  assert(archive_write_close(writer.get()) == ARCHIVE_OK);
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
                            const std::string &contents) {
  auto writer = makeArchiveWriteHandle();
  assert(writer);
  assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
  assert(archive_write_set_options(writer.get(), "zip:compression=store") ==
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
  // Deliberately registered after the 7-Zip index-count assertion above:
  // these bounded-read tests index real archives, which would otherwise pollute
  // that assertion's retained debug-log window.
  testZipBoundedReadStreamsFullInBoundsEntry();
  testZipBoundedReadRejectsCentralDirectoryUnderstatedSize();
  testBoundedReadRejectsOversizedSevenZipEntry();
  testSevenZipReadUsesCurrentOperationPauseCallback();
  testEncodedHeaderSevenZipUsesSdk();
  testDeltaFilteredSevenZipUsesSdk();
  testFullUnzipHonorsPauseDuringExtraction();
  testArchiveIndexPersistsAcrossColdCacheRestart();
  testArchiveIndexPrunesOrphanedCacheFiles();
  testArchiveIndexPrunesOrphanedTmpCacheFiles();
  testCorruptIndexEntryCountIsRejected();
  testSingleFlightWaitersDoNotEachReindexAfterFailedBuild();
  testDebugLogRetainsNewestThousandLines();
  return 0;
}
