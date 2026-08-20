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
  testIndependentSevenZipCacheMissesOpenConcurrently();
  testSevenZipReadUsesCurrentOperationPauseCallback();
  testEncodedHeaderSevenZipUsesSdk();
  testDeltaFilteredSevenZipUsesSdk();
  testDebugLogRetainsNewestThousandLines();
  return 0;
}
