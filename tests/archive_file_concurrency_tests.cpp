#include "../src/ArchiveFile.h"
#include "../src/ArchiveRAII.h"

#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using ArchiveEntryHandle =
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

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

} // namespace

int main() {
  testIndependentSevenZipCacheMissesOpenConcurrently();
  return 0;
}
