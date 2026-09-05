#include "ArchiveFile.h"

#include "BmsMetadataText.h"
#include "targets.h"
#include "RAII.h"
#if TARGET_OS_ANDROID
#include "AndroidNatives.h"
#endif

#include <SDL2/SDL.h>
#if __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#if __has_include(<archive.h>) && __has_include(<archive_entry.h>)
#include <archive.h>
#include <archive_entry.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 0
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
#include "ArchiveRAII.h"
#endif

#if __has_include(<unarr.h>) && \
    !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#include <unarr.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_UNARR 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_UNARR 0
#endif

#if __has_include(<7zip/CPP/7zip/Archive/IArchive.h>) &&                  \
    __has_include(<7zip/CPP/7zip/IStream.h>) &&                           \
    __has_include(<7zip/CPP/Common/MyCom.h>)
#if defined(_WIN32)
// The Windows 7-Zip package exposes the SDK interfaces from a DLL, so this
// translation unit owns their GUID definitions instead of expecting them from
// the DLL's deliberately small import library.
#include <7zip/CPP/Common/MyInitGuid.h>
#endif
#include <7zip/CPP/7zip/Archive/IArchive.h>
#include <7zip/CPP/7zip/IStream.h>
#include <7zip/CPP/Common/MyCom.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP 0
#endif

#if !TARGET_OS_ANDROID && __has_include(<iconv.h>)
#include <iconv.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_ICONV 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_ICONV 0
#endif

#if __has_include("../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h")
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"
#define ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ 0
#endif

namespace archive_file {

std::filesystem::path archiveIndexCacheDirectory();

namespace {

bool stopRequested(const std::stop_token *stopToken);
bool pauseIfNeeded(const PauseCallback &pauseCallback,
                   std::string *errorMessage = nullptr);
bool unzipCheckpoint(const std::stop_token *stopToken,
                     const PauseCallback &pauseCallback,
                     std::string *errorMessage = nullptr);
void reportUnzipProgress(const UnzipProgressCallback &callback,
                          double fraction, std::uint64_t current,
                          std::uint64_t total, std::string message);
bool emitFileData(FileData &&file, const FileDataCallback &onFile,
                  std::string *errorMessage);
bool archiveReadCancelled(const std::string &errorMessage);

using asobmshow::bms_metadata::lowerCopy;

bool createDirectoriesForUnzip(const std::filesystem::path &path,
                               std::string_view failurePrefix,
                               std::string *errorMessage,
                               std::error_code &error) {
  error.clear();
  std::filesystem::create_directories(path, error);
  if (!error) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = std::string(failurePrefix) + ": " + error.message();
  }
  return false;
}

bool pathExistsForUnzip(const std::filesystem::path &path,
                        std::string_view failurePrefix, bool &exists,
                        std::string *errorMessage,
                        std::error_code &error) {
  error.clear();
  exists = std::filesystem::exists(path, error);
  if (!error) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = std::string(failurePrefix) + ": " + error.message();
  }
  return false;
}

std::string replaceAll(std::string value, std::string_view needle,
                       std::string_view replacement) {
  if (needle.empty()) {
    return value;
  }
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

bool hasZipArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".zip" || extension == ".cbz";
}

bool hasRarArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".rar" || extension == ".cbr";
}

bool hasSevenZipArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".7z" || extension == ".cb7" || extension == ".rar" ||
         extension == ".cbr" || extension == ".lzh" || extension == ".lha" ||
         extension == ".zipx";
}

enum class ArchiveIndexBackend {
  Unknown,
  MinizZip,
  UnarrRar,
  SevenZip,
  LibArchive,
};

enum class RarSignature {
  Unknown,
  Rar4,
  Rar5,
};

RarSignature rarSignature(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return RarSignature::Unknown;
  }

  std::array<unsigned char, 8> marker{};
  file.read(reinterpret_cast<char *>(marker.data()),
            static_cast<std::streamsize>(marker.size()));
  const std::streamsize readSize = file.gcount();
  if (readSize >= 7 && marker[0] == 'R' && marker[1] == 'a' &&
      marker[2] == 'r' && marker[3] == '!' && marker[4] == 0x1a &&
      marker[5] == 0x07 && marker[6] == 0x00) {
    return RarSignature::Rar4;
  }
  if (readSize >= 8 && marker[0] == 'R' && marker[1] == 'a' &&
      marker[2] == 'r' && marker[3] == '!' && marker[4] == 0x1a &&
      marker[5] == 0x07 && marker[6] == 0x01 && marker[7] == 0x00) {
    return RarSignature::Rar5;
  }
  return RarSignature::Unknown;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
enum class SevenZipFormat : unsigned char {
  Zip = 0x01,
  Rar = 0x03,
  Lzh = 0x06,
  SevenZip = 0x07,
  Rar5 = 0xcc,
};

GUID sevenZipFormatGuid(SevenZipFormat format) {
  return {0x23170F69,
          0x40C1,
          0x278A,
          {0x10, 0x00, 0x00, 0x01, 0x10,
           static_cast<unsigned char>(format), 0x00, 0x00}};
}

std::vector<SevenZipFormat>
sevenZipFormatCandidates(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  if (extension == ".7z" || extension == ".cb7") {
    return {SevenZipFormat::SevenZip};
  }
  if (extension == ".rar" || extension == ".cbr") {
    // 7-Zip RAR support is decompression-only code under LGPL plus the
    // unRAR restriction; do not use it for RAR-compatible archive creation.
    switch (rarSignature(path)) {
    case RarSignature::Rar4:
      return {SevenZipFormat::Rar};
    case RarSignature::Rar5:
      return {SevenZipFormat::Rar5};
    case RarSignature::Unknown:
      return {SevenZipFormat::Rar, SevenZipFormat::Rar5};
    }
  }
  if (extension == ".lzh" || extension == ".lha") {
    return {SevenZipFormat::Lzh};
  }
  if (extension == ".zipx") {
    return {SevenZipFormat::Zip};
  }
  return {};
}
#endif

std::string normalizeEntryName(std::string value) {
  value = replaceAll(std::move(value), "\\", "/");
  std::filesystem::path path(value);
  path = path.lexically_normal();
  std::string normalized = path.generic_string();
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  if (normalized == ".") {
    normalized.clear();
  }
  return normalized;
}

bool safeEntryPath(const std::string &name, std::filesystem::path &outPath) {
  if (name.empty() || name.find('\0') != std::string::npos) {
    return false;
  }
  const std::string normalized = normalizeEntryName(name);
  std::filesystem::path relative(normalized);
  if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
    return false;
  }
  for (const auto &part : relative) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  outPath = relative;
  return true;
}

bool pathIsInsideFolder(const std::filesystem::path &path,
                        const std::filesystem::path &folderPath) {
  const std::string normalized = normalizeEntryName(path.generic_string());
  const std::string folder = normalizeEntryName(folderPath.generic_string());
  if (folder.empty()) {
    return !normalized.empty();
  }
  return normalized == folder ||
         (normalized.size() > folder.size() &&
          normalized.compare(0, folder.size(), folder) == 0 &&
          normalized[folder.size()] == '/');
}

bool isSystemDirectoryComponent(const std::string &lowerComponent) {
  // Archives may come from any OS; do not gate this list on the current build
  // target. Keep this to names that are very unlikely to be user chart folders.
  if (lowerComponent.empty()) {
    return false;
  }
  switch (lowerComponent.front()) {
  case '_':
    return lowerComponent == "__macosx";
  case '.':
    return lowerComponent == ".appledouble" ||
           lowerComponent == ".documentrevisions-v100" ||
           lowerComponent == ".fseventsd" ||
           lowerComponent == ".spotlight-v100" ||
           lowerComponent == ".temporaryitems" ||
           lowerComponent == ".trashes" || lowerComponent == ".trash" ||
           lowerComponent.starts_with(".trash-");
  case '$':
    return lowerComponent == "$recycle.bin";
  case 's':
    return lowerComponent == "system volume information";
  default:
    return false;
  }
}

bool isSystemFileComponent(const std::string &component,
                           const std::string &lowerComponent) {
  if (component.starts_with("._")) {
    return true;
  }
  if (lowerComponent.empty()) {
    return false;
  }
  switch (lowerComponent.front()) {
  case '.':
    return lowerComponent == ".com.apple.timemachine.donotpresent" ||
           lowerComponent == ".ds_store" || lowerComponent == ".localized" ||
           lowerComponent == ".volumeicon.icns";
  case 'd':
    return lowerComponent == "desktop.ini";
  case 'e':
    return lowerComponent == "ehthumbs.db";
  case 'i':
    return lowerComponent == "icon\r";
  case 't':
    return lowerComponent == "thumbs.db" ||
           lowerComponent == "thumbs.db:encryptable";
  default:
    return false;
  }
}

bool isSystemEntryPath(const std::filesystem::path &path) {
  const std::string normalized = normalizeEntryName(path.generic_string());
  if (normalized.empty()) {
    return false;
  }

  std::filesystem::path relative(normalized);
  for (const auto &part : relative) {
    const std::string component = part.generic_string();
    if (component.empty() || component == "." || component == "..") {
      continue;
    }
    const std::string lower = lowerCopy(component);
    if (isSystemDirectoryComponent(lower) ||
        isSystemFileComponent(component, lower)) {
      return true;
    }
  }
  return false;
}

std::size_t filterSystemEntries(std::vector<Entry> &entries) {
  const std::size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const Entry &entry) {
                                 return isSystemEntryPath(entry.path);
                               }),
                entries.end());
  return before - entries.size();
}

std::mutex gCachePathNormalizerMutex;
CachePathNormalizer gCachePathNormalizer;
std::mutex gTemporaryCacheMutex;

std::filesystem::path cacheNormalizedPath(const std::filesystem::path &path) {
  std::filesystem::path normalized = path.lexically_normal();
  CachePathNormalizer normalizer;
  {
    std::lock_guard<std::mutex> lock(gCachePathNormalizerMutex);
    normalizer = gCachePathNormalizer;
  }
  if (normalizer) {
    normalizer(normalized);
    normalized = normalized.lexically_normal();
  }
  return normalized;
}

std::string cachePathKey(const std::filesystem::path &path) {
  return fspath_to_utf8(cacheNormalizedPath(path));
}

std::string archiveKey(const std::filesystem::path &path) {
  return cachePathKey(path);
}

bool fileState(const std::filesystem::path &path, std::uintmax_t &size,
               std::filesystem::file_time_type &mtime) {
  std::error_code error;
  size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  mtime = std::filesystem::last_write_time(path, error);
  return !error;
}

std::string fileStateKey(const std::filesystem::path &path) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(path, size, mtime)) {
    return "missing";
  }

  std::ostringstream out;
  const auto mtimeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              mtime.time_since_epoch())
                              .count();
  out << size << ':' << mtimeNanos;
  return out.str();
}

std::uint64_t clampFileSizeForResult(std::uintmax_t value) {
  return static_cast<std::uint64_t>(
      std::min(value, static_cast<std::uintmax_t>(
                          std::numeric_limits<std::uint64_t>::max())));
}

void addClamped(std::uint64_t &total, std::uint64_t value) {
  const std::uint64_t maxValue = std::numeric_limits<std::uint64_t>::max();
  total = value > maxValue - total ? maxValue : total + value;
}

bool directoryStats(const std::filesystem::path &root, std::uint64_t &bytes,
                    std::uint64_t &entries,
                    const std::stop_token *stopToken = nullptr) {
  bytes = 0;
  entries = 0;
  std::error_code error;
  const bool rootIsFile = std::filesystem::is_regular_file(root, error);
  if (error) {
    return false;
  }
  if (rootIsFile) {
    const std::uintmax_t size = std::filesystem::file_size(root, error);
    if (error) {
      return false;
    }
    bytes = clampFileSizeForResult(size);
    entries = 1;
    return true;
  }
  const bool rootIsDirectory = std::filesystem::is_directory(root, error);
  if (error || !rootIsDirectory) {
    return false;
  }

  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && it != end) {
    if (stopRequested(stopToken)) {
      return false;
    }
    const std::filesystem::directory_entry &entry = *it;
    std::error_code entryError;
    if (entry.is_regular_file(entryError) && !entryError) {
      const std::uintmax_t size = entry.file_size(entryError);
      if (!entryError) {
        addClamped(bytes, clampFileSizeForResult(size));
      }
    }
    addClamped(entries, 1);
    it.increment(error);
  }
  return !error && !stopRequested(stopToken);
}

std::uint64_t directoryByteSize(const std::filesystem::path &root,
                                const std::stop_token *stopToken = nullptr) {
  std::uint64_t bytes = 0;
  std::uint64_t entries = 0;
  directoryStats(root, bytes, entries, stopToken);
  return bytes;
}

struct CachedIndex {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  ArchiveIndexBackend backend = ArchiveIndexBackend::Unknown;
  unsigned char sevenZipFormat = 0;
  std::vector<Entry> entries;
  std::unordered_map<std::string, std::size_t> exact;
  std::unordered_map<std::string, std::size_t> lower;
};

std::mutex gIndexMutex;
// NOTE: gIndexCache is intentionally unbounded (one full entry list per
// archive indexed this process, bounded by library size). An LRU eviction of
// least-recently-used archive indexes is a possible follow-up if memory
// becomes a concern on very large libraries.
std::unordered_map<std::string, std::shared_ptr<const CachedIndex>> gIndexCache;

// Directory for persisting archive entry indexes across app restarts. Empty
// means disk persistence is disabled. Guarded by gIndexCacheMutex.
std::mutex gIndexCacheDirectoryMutex;
std::filesystem::path gArchiveIndexCacheDirectory;

// Single-flight coordination: while one thread is building an archive index
// for a key, concurrent requesters wait and reuse the finished index instead
// of rebuilding it (a large library can otherwise index the same archive from
// the scan and the prefetch/read workers at once).
std::mutex gIndexBuildMutex;
std::unordered_map<std::string, std::condition_variable> gIndexBuildInFlight;
std::unordered_map<std::string, bool> gIndexBuildActive;
std::unordered_map<std::string, bool> gIndexBuildDone;
std::unordered_map<std::string, bool> gIndexBuildFailed;

#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
std::atomic<std::uint32_t> gSingleFlightWaiterCountForTesting{0};
#endif

// RAII scope for the single-flight index builder: on ANY exit from the builder
// body (including an exception thrown by a backend) it clears the in-flight
// flag, records the outcome, and wakes every waiter, so a failing or aborted
// build can never leave waiters blocked on the in-flight condition variable.
class IndexBuildScope {
public:
  explicit IndexBuildScope(std::string key) : key_(std::move(key)) {}

  ~IndexBuildScope() {
    if (!finished_) {
      complete(false);
    }
  }

  void complete(bool success) {
    std::lock_guard<std::mutex> lock(gIndexBuildMutex);
    gIndexBuildActive[key_] = false;
    gIndexBuildDone[key_] = true;
    gIndexBuildFailed[key_] = !success;
    gIndexBuildInFlight[key_].notify_all();
    finished_ = true;
  }

private:
  std::string key_;
  bool finished_ = false;
};

constexpr std::size_t kDebugLogMaxLines = 1000;
std::mutex gDebugLogMutex;
std::deque<std::string> gDebugLogLines;
std::uint64_t gDebugLogRevision = 0;

#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
std::mutex gStreamingEntryObserverMutex;
StreamingEntryObserverForTesting gStreamingEntryObserver;

void notifyStreamingEntryObserverForTesting(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &entryPath) {
  StreamingEntryObserverForTesting observer;
  {
    std::lock_guard lock(gStreamingEntryObserverMutex);
    observer = gStreamingEntryObserver;
  }
  if (observer) {
    observer(archivePath, entryPath);
  }
}
#endif

std::string pathForLog(const std::filesystem::path &path) {
  return fspath_to_utf8(path);
}

std::string byteCountForLog(std::uintmax_t bytes) {
  constexpr std::uintmax_t kib = 1024;
  constexpr std::uintmax_t mib = kib * 1024;
  constexpr std::uintmax_t gib = mib * 1024;
  if (bytes >= gib) {
    return std::to_string(bytes / gib) + " GiB";
  }
  if (bytes >= mib) {
    return std::to_string(bytes / mib) + " MiB";
  }
  if (bytes >= kib) {
    return std::to_string(bytes / kib) + " KiB";
  }
  return std::to_string(bytes) + " B";
}

void appendDebugLogLineImpl(std::string message) {
  if (message.empty()) {
    return;
  }
  if (message.size() > 6000) {
    message.resize(6000);
    message += "...";
  }

  std::ostringstream line;
  line << '[' << SDL_GetTicks64() << "ms] " << message;

  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  gDebugLogLines.push_back(line.str());
  while (gDebugLogLines.size() > kDebugLogMaxLines) {
    gDebugLogLines.pop_front();
  }
  ++gDebugLogRevision;
}

std::string backendName(ArchiveIndexBackend backend) {
  switch (backend) {
  case ArchiveIndexBackend::MinizZip:
    return "miniz ZIP";
  case ArchiveIndexBackend::UnarrRar:
    return "unarr RAR";
  case ArchiveIndexBackend::SevenZip:
    return "7-Zip SDK";
  case ArchiveIndexBackend::LibArchive:
    return "libarchive";
  case ArchiveIndexBackend::Unknown:
  default:
    return "unknown";
  }
}

std::uintmax_t maxBufferedReadSize() {
  return std::min<std::uintmax_t>(
      static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max()),
      static_cast<std::uintmax_t>(
          std::numeric_limits<std::streamsize>::max()));
}

void reserveBufferedBytes(std::vector<unsigned char> &bytes,
                          std::uintmax_t size) {
  if (size <=
      static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    bytes.reserve(static_cast<std::size_t>(size));
  }
}

#ifdef _WIN32
std::string windowsErrorMessage(DWORD error) {
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "Windows error " + std::to_string(error);
  }

  std::wstring message(buffer, length);
  ::LocalFree(buffer);
  while (!message.empty() &&
         (message.back() == L'\r' || message.back() == L'\n' ||
          message.back() == L' ' || message.back() == L'\t')) {
    message.pop_back();
  }
  return path_t_to_utf8(message) + " (" + std::to_string(error) + ")";
}
#endif

class RandomAccessFile {
public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile &) = delete;
  RandomAccessFile &operator=(const RandomAccessFile &) = delete;

  ~RandomAccessFile() {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
    }
#else
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
#endif
  }

  bool open(const std::filesystem::path &path, std::string *errorMessage) {
#ifndef _WIN32
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    const std::string pathText = fspath_to_utf8(path);
    fd_ = ::open(pathText.c_str(), O_RDONLY);
    if (fd_ < 0) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not open file for random access: " +
                        pathForLog(path) + ": " + std::strerror(errno);
      }
      return false;
    }
    return true;
#else
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
    handle_ = ::CreateFileW(
        fspath_to_path_t(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      if (errorMessage != nullptr) {
        const DWORD error = ::GetLastError();
        *errorMessage = "Could not open file for random access: " +
                        pathForLog(path) + ": " + windowsErrorMessage(error);
      }
      return false;
    }
    return true;
#endif
  }

  bool readAt(std::uint64_t offset, void *data, std::size_t size,
              std::string *errorMessage) {
    if (size == 0) {
      return true;
    }
    if (data == nullptr) {
      if (errorMessage != nullptr) {
        *errorMessage = "Random access read buffer is unavailable.";
      }
      return false;
    }

#ifndef _WIN32
    if (fd_ < 0) {
      if (errorMessage != nullptr) {
        *errorMessage = "Random access file is not open.";
      }
      return false;
    }
    if (offset >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      if (errorMessage != nullptr) {
        *errorMessage = "Random access read offset is out of range.";
      }
      return false;
    }

    auto *cursor = static_cast<unsigned char *>(data);
    std::size_t readTotal = 0;
    while (readTotal < size) {
      const std::size_t remaining = size - readTotal;
      const std::size_t chunk = std::min<std::size_t>(
          remaining, static_cast<std::size_t>(
                         std::numeric_limits<ssize_t>::max()));
      const auto readOffset = static_cast<off_t>(offset + readTotal);
      const ssize_t count = ::pread(fd_, cursor + readTotal, chunk, readOffset);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errorMessage != nullptr) {
          *errorMessage =
              std::string("Random access read failed: ") + std::strerror(errno);
        }
        return false;
      }
      if (count == 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Random access read reached end of file.";
        }
        return false;
      }
      readTotal += static_cast<std::size_t>(count);
    }
    return true;
#else
    if (handle_ == INVALID_HANDLE_VALUE) {
      if (errorMessage != nullptr) {
        *errorMessage = "Random access file is not open.";
      }
      return false;
    }

    auto *cursor = static_cast<unsigned char *>(data);
    std::size_t readTotal = 0;
    while (readTotal < size) {
      const std::size_t remaining = size - readTotal;
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
          remaining, static_cast<std::size_t>(
                         std::numeric_limits<DWORD>::max())));
      const std::uint64_t readOffset = offset + readTotal;
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(readOffset & 0xffffffffull);
      overlapped.OffsetHigh = static_cast<DWORD>(readOffset >> 32);

      DWORD read = 0;
      BOOL ok = ::ReadFile(handle_, cursor + readTotal, chunk, &read,
                           &overlapped);
      if (!ok) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_IO_PENDING) {
          ok = ::GetOverlappedResult(handle_, &overlapped, &read, TRUE);
        }
        if (!ok) {
          if (errorMessage != nullptr) {
            const DWORD finalError = ::GetLastError();
            *errorMessage =
                "Random access read failed: " +
                windowsErrorMessage(finalError == ERROR_SUCCESS ? error
                                                                : finalError);
          }
          return false;
        }
      }
      if (read == 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Random access read reached end of file.";
        }
        return false;
      }
      readTotal += static_cast<std::size_t>(read);
    }
    return true;
#endif
  }

private:
#ifndef _WIN32
  int fd_ = -1;
#else
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

bool readRegularFile(const std::filesystem::path &path,
                     std::vector<unsigned char> &bytes,
                     std::string *errorMessage) {
  std::ifstream file(path, std::ios::binary);
#if TARGET_OS_ANDROID
  if (!file) {
    const std::string assetPath = path.generic_string();
    UniqueResource<SDL_RWops, SDL_RWclose> rw(
        SDL_RWFromFile(assetPath.c_str(), "rb"));
    if (rw) {
      bytes.clear();
      const Sint64 size = SDL_RWsize(rw.get());
      if (size > 0) {
        if (static_cast<std::uintmax_t>(size) > maxBufferedReadSize()) {
          if (errorMessage != nullptr) {
            *errorMessage = "Android asset is too large to read: " + assetPath;
          }
          return false;
        }
        bytes.resize(static_cast<size_t>(size));
        const size_t read =
            SDL_RWread(rw.get(), bytes.data(), 1, bytes.size());
        if (read != bytes.size()) {
          if (errorMessage != nullptr) {
            *errorMessage = "Could not read Android asset: " + assetPath;
          }
          bytes.clear();
          return false;
        }
      } else {
        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;) {
          const size_t read =
              SDL_RWread(rw.get(), buffer.data(), 1, buffer.size());
          if (read > 0) {
            bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
          }
          if (read < buffer.size()) {
            break;
          }
        }
      }
      return true;
    }
  }
#endif
  if (!file) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open file: " + pathForLog(path);
    }
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read file size: " + pathForLog(path);
    }
    return false;
  }
  if (static_cast<std::uintmax_t>(size) > maxBufferedReadSize()) {
    if (errorMessage != nullptr) {
      *errorMessage = "File is too large to read: " + pathForLog(path);
    }
    return false;
  }
  file.seekg(0, std::ios::beg);
  bytes.resize(static_cast<size_t>(size));
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not read file: " + pathForLog(path);
      }
      return false;
    }
  }
  return true;
}

bool appendBoundedRead(std::vector<unsigned char> &bytes,
                       const unsigned char *data, std::size_t size,
                       std::size_t maximumBytes,
                       const std::filesystem::path &path,
                       std::string *errorMessage) {
  if (bytes.size() > maximumBytes || size > maximumBytes - bytes.size()) {
    bytes.clear();
    if (errorMessage != nullptr) {
      *errorMessage = "File exceeds bounded read limit: " + pathForLog(path);
    }
    return false;
  }
  bytes.insert(bytes.end(), data, data + size);
  return true;
}

bool readRegularFileBounded(const std::filesystem::path &path,
                            std::vector<unsigned char> &bytes,
                            std::size_t maximumBytes,
                            std::string *errorMessage,
                            std::stop_token stop) {
  bytes.clear();
  std::array<unsigned char, 64U * 1024U> buffer{};
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string androidError;
    const auto descriptor = OpenAndroidTreeFileDescriptor(path, androidError);
    if (!descriptor.has_value()) {
      if (errorMessage != nullptr) *errorMessage = std::move(androidError);
      return false;
    }
    const auto closeDescriptor =
        makeScopeExit([descriptor = *descriptor] { (void)close(descriptor); });
    for (;;) {
      if (stop.stop_requested()) {
        bytes.clear();
        return false;
      }
      const ssize_t count = read(*descriptor, buffer.data(), buffer.size());
      if (count > 0) {
        if (!appendBoundedRead(bytes, buffer.data(),
                               static_cast<std::size_t>(count), maximumBytes,
                               path, errorMessage)) {
          return false;
        }
        continue;
      }
      if (count == 0) return true;
      if (errno == EINTR) continue;
      bytes.clear();
      if (errorMessage != nullptr) {
        *errorMessage = "Android file descriptor read failed.";
      }
      return false;
    }
  }
#endif

  std::ifstream file(path, std::ios::binary);
  if (file) {
    for (;;) {
      if (stop.stop_requested()) {
        bytes.clear();
        return false;
      }
      file.read(reinterpret_cast<char *>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
      const std::streamsize count = file.gcount();
      if (count > 0 &&
          !appendBoundedRead(bytes, buffer.data(),
                             static_cast<std::size_t>(count), maximumBytes,
                             path, errorMessage)) {
        return false;
      }
      if (file.eof()) return true;
      if (!file) {
        bytes.clear();
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read file: " + pathForLog(path);
        }
        return false;
      }
    }
  }

#if TARGET_OS_ANDROID
  const std::string assetPath = path.generic_string();
  UniqueResource<SDL_RWops, SDL_RWclose> input(
      SDL_RWFromFile(assetPath.c_str(), "rb"));
  if (input) {
    for (;;) {
      if (stop.stop_requested()) {
        bytes.clear();
        return false;
      }
      const std::size_t count =
          SDL_RWread(input.get(), buffer.data(), 1, buffer.size());
      if (!appendBoundedRead(bytes, buffer.data(), count, maximumBytes, path,
                             errorMessage)) {
        return false;
      }
      if (count < buffer.size()) return true;
    }
  }
#endif

  if (errorMessage != nullptr) {
    *errorMessage = "Could not open file: " + pathForLog(path);
  }
  return false;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
using UnarrStreamHandle = UniqueResource<ar_stream, ar_close>;
using UnarrArchiveHandle = UniqueResource<ar_archive, ar_close_archive>;

constexpr unsigned char kRarMainHeader = 0x73;
constexpr unsigned char kRarFileHeader = 0x74;
constexpr unsigned short kRarMainSolidFlag = 1u << 3;
constexpr unsigned short kRarFileSolidFlag = 1u << 4;

unsigned short readUInt16Le(const unsigned char *data) {
  return static_cast<unsigned short>(data[0] |
                                     (static_cast<unsigned short>(data[1]) << 8));
}

bool readRar4BaseHeader(std::ifstream &file, std::int64_t offset,
                        unsigned char &type, unsigned short &flags,
                        unsigned short &size) {
  if (offset < 0) {
    return false;
  }
  unsigned char header[7] = {};
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.read(reinterpret_cast<char *>(header), sizeof(header))) {
    return false;
  }
  type = header[2];
  flags = readUInt16Le(header + 3);
  size = readUInt16Le(header + 5);
  return size >= sizeof(header);
}

bool readRar4MainSolidFlag(std::ifstream &file, bool &mainSolid) {
  unsigned char type = 0;
  unsigned short flags = 0;
  unsigned short size = 0;
  if (!readRar4BaseHeader(file, 7, type, flags, size) ||
      type != kRarMainHeader) {
    return false;
  }
  mainSolid = (flags & kRarMainSolidFlag) != 0;
  return true;
}

bool readRar4EntrySolidFlagAtOffset(std::ifstream &file, std::int64_t offset,
                                    bool mainSolid, bool &solid) {
  unsigned char type = 0;
  unsigned short flags = 0;
  unsigned short size = 0;
  if (!readRar4BaseHeader(file, offset, type, flags, size) ||
      type != kRarFileHeader) {
    return false;
  }

  unsigned char compressedSize[4] = {};
  unsigned char fileData[21] = {};
  if (!file.read(reinterpret_cast<char *>(compressedSize),
                 sizeof(compressedSize)) ||
      !file.read(reinterpret_cast<char *>(fileData), sizeof(fileData))) {
    return false;
  }

  const unsigned char unpackVersion = fileData[13];
  solid = unpackVersion < 20 ? mainSolid : (flags & kRarFileSolidFlag) != 0;
  return true;
}

bool openUnarrRarArchive(const std::filesystem::path &archivePath,
                         UnarrStreamHandle &stream,
                         UnarrArchiveHandle &archive,
                         std::string *errorMessage) {
  const std::string archiveText = fspath_to_utf8(archivePath);
  stream.reset(ar_open_file(archiveText.c_str()));
  if (stream == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not open archive file.";
    }
    return false;
  }

  archive.reset(ar_open_rar_archive(stream.get()));
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not open RAR4 archive.";
    }
    return false;
  }
  return true;
}

bool listUnarrRarEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         bool &containsSolidEntries,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback) {
  entries.clear();
  containsSolidEntries = false;

  UnarrStreamHandle stream;
  UnarrArchiveHandle archive;
  if (!openUnarrRarArchive(archivePath, stream, archive, errorMessage)) {
    return false;
  }

  std::ifstream headerFile(archivePath, std::ios::binary);
  bool mainSolid = false;
  if (!headerFile || !readRar4MainSolidFlag(headerFile, mainSolid)) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR header inspection failed.";
    }
    return false;
  }

  std::size_t order = 0;
  while (ar_parse_entry(archive.get())) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      entries.clear();
      return false;
    }
    const char *entryName = ar_entry_get_name(archive.get());
    if (entryName == nullptr || entryName[0] == '\0') {
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(entryName, relativePath)) {
      continue;
    }
    if (isSystemEntryPath(relativePath)) {
      continue;
    }

    const std::int64_t offset =
        static_cast<std::int64_t>(ar_entry_get_offset(archive.get()));
    bool solid = false;
    if (!readRar4EntrySolidFlagAtOffset(headerFile, offset, mainSolid, solid)) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR entry header inspection failed.";
      }
      entries.clear();
      return false;
    }
    containsSolidEntries = containsSolidEntries || solid;

    entries.push_back({
        .path = relativePath,
        .directory = false,
        .size = static_cast<std::uint64_t>(ar_entry_get_size(archive.get())),
        .order = order++,
        .offset = offset,
        .solid = solid,
    });
  }

  if (!ar_at_eof(archive.get())) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR index did not reach archive end.";
    }
    entries.clear();
    return false;
  }

  appendDebugLogLineImpl("Indexed RAR with unarr random offsets: " +
                         pathForLog(archivePath) +
                         " entries=" + std::to_string(entries.size()) +
                         " solid=" +
                         (containsSolidEntries ? "yes" : "no"));
  return true;
}
#endif

void appendUtf8CodePoint(std::string &output, std::uint32_t codePoint) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
    return;
  }
  if (codePoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  if (codePoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  if (codePoint <= 0x10ffff) {
    output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  appendUtf8CodePoint(output, 0xfffd);
}

std::string wideStringToUtf8(const wchar_t *input, std::size_t length) {
  std::string output;
  if (input == nullptr || length == 0) {
    return output;
  }
  output.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    std::uint32_t codePoint = static_cast<std::uint32_t>(input[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (codePoint >= 0xd800 && codePoint <= 0xdbff && i + 1 < length) {
        const std::uint32_t low = static_cast<std::uint32_t>(input[i + 1]);
        if (low >= 0xdc00 && low <= 0xdfff) {
          codePoint =
              0x10000 + (((codePoint - 0xd800) << 10) | (low - 0xdc00));
          ++i;
        } else {
          codePoint = 0xfffd;
        }
      } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
        codePoint = 0xfffd;
      }
    }
    appendUtf8CodePoint(output, codePoint);
  }
  return output;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP

#if defined(_WIN32)
using SevenZipCreateObject = HRESULT(WINAPI *)(const GUID *, const GUID *,
                                               void **);

SevenZipCreateObject resolveSevenZipCreateObject() noexcept {
  static const SevenZipCreateObject createObject = []() noexcept {
    std::array<wchar_t, 32768> executablePath{};
    const DWORD pathLength = GetModuleFileNameW(
        nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (pathLength == 0 || pathLength >= executablePath.size()) {
      return static_cast<SevenZipCreateObject>(nullptr);
    }

    std::filesystem::path modulePath(
        std::wstring_view(executablePath.data(), pathLength));
    modulePath.replace_filename(L"7zip.dll");
    HMODULE module = LoadLibraryW(modulePath.c_str());
    if (module == nullptr) {
      return static_cast<SevenZipCreateObject>(nullptr);
    }
    return reinterpret_cast<SevenZipCreateObject>(
        GetProcAddress(module, "CreateObject"));
  }();
  return createObject;
}

HRESULT createSevenZipObject(const GUID *clsID, const GUID *interfaceID,
                             void **out) noexcept {
  const auto createObject = resolveSevenZipCreateObject();
  if (createObject == nullptr) {
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }
  return createObject(clsID, interfaceID, out);
}
#else
extern "C" HRESULT WINAPI CreateObject(const GUID *clsID,
                                        const GUID *interfaceID, void **out);

HRESULT createSevenZipObject(const GUID *clsID, const GUID *interfaceID,
                             void **out) noexcept {
  return CreateObject(clsID, interfaceID, out);
}
#endif

std::string sevenZipResultMessage(HRESULT result) {
  return "7-Zip SDK error: " + std::to_string(static_cast<long long>(result));
}

struct SevenZipPropVariant : PROPVARIANT {
  SevenZipPropVariant() { std::memset(this, 0, sizeof(PROPVARIANT)); }
  ~SevenZipPropVariant() {
#if defined(_WIN32)
    PropVariantClear(this);
#else
    VariantClear(this);
#endif
  }
};

std::optional<std::string> sevenZipStringProperty(IInArchive *archive,
                                                  UInt32 index,
                                                  PROPID property) {
  if (archive == nullptr) {
    return std::nullopt;
  }
  SevenZipPropVariant value;
  if (archive->GetProperty(index, property, &value) != S_OK ||
      value.vt != VT_BSTR || value.bstrVal == nullptr) {
    return std::nullopt;
  }
  return wideStringToUtf8(value.bstrVal, SysStringLen(value.bstrVal));
}

bool sevenZipBoolProperty(IInArchive *archive, UInt32 index, PROPID property,
                          bool defaultValue = false) {
  SevenZipPropVariant value;
  if (archive == nullptr ||
      archive->GetProperty(index, property, &value) != S_OK) {
    return defaultValue;
  }
  if (value.vt == VT_BOOL) {
    return value.boolVal != VARIANT_FALSE;
  }
  if (value.vt == VT_UI4) {
    return value.ulVal != 0;
  }
  if (value.vt == VT_UI8) {
    return value.uhVal.QuadPart != 0;
  }
  return defaultValue;
}

bool sevenZipArchiveBoolProperty(IInArchive *archive, PROPID property,
                                 bool defaultValue = false) {
  SevenZipPropVariant value;
  if (archive == nullptr ||
      archive->GetArchiveProperty(property, &value) != S_OK) {
    return defaultValue;
  }
  if (value.vt == VT_BOOL) {
    return value.boolVal != VARIANT_FALSE;
  }
  if (value.vt == VT_UI4) {
    return value.ulVal != 0;
  }
  if (value.vt == VT_UI8) {
    return value.uhVal.QuadPart != 0;
  }
  return defaultValue;
}

std::uint64_t sevenZipUInt64Property(IInArchive *archive, UInt32 index,
                                     PROPID property,
                                     std::uint64_t defaultValue = 0) {
  SevenZipPropVariant value;
  if (archive == nullptr ||
      archive->GetProperty(index, property, &value) != S_OK) {
    return defaultValue;
  }
  if (value.vt == VT_UI8) {
    return static_cast<std::uint64_t>(value.uhVal.QuadPart);
  }
  if (value.vt == VT_I8) {
    return value.hVal.QuadPart > 0
               ? static_cast<std::uint64_t>(value.hVal.QuadPart)
               : defaultValue;
  }
  if (value.vt == VT_UI4) {
    return static_cast<std::uint64_t>(value.ulVal);
  }
  if (value.vt == VT_I4) {
    return value.lVal > 0 ? static_cast<std::uint64_t>(value.lVal)
                          : defaultValue;
  }
  return defaultValue;
}

class SevenZipInFileStream final : public IInStream, public IStreamGetSize {
public:
  SevenZipInFileStream(const std::filesystem::path &path, std::uintmax_t size,
                       PauseCallback pauseCallback)
      : size_(static_cast<UInt64>(size)),
        pauseCallback_(std::move(pauseCallback)) {
    file_.rdbuf()->pubsetbuf(buffer_.data(),
                             static_cast<std::streamsize>(buffer_.size()));
    file_.open(path, std::ios::binary);
  }

  bool isOpen() const { return file_.is_open(); }

  void setPauseCallback(PauseCallback pauseCallback) {
    pauseCallback_ = std::move(pauseCallback);
  }

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IInStream) {
      *outObject = static_cast<IInStream *>(this);
    } else if (iid == IID_ISequentialInStream) {
      *outObject =
          static_cast<ISequentialInStream *>(static_cast<IInStream *>(this));
    } else if (iid == IID_IStreamGetSize) {
      *outObject = static_cast<IStreamGetSize *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(Read)(void *data, UInt32 size, UInt32 *processedSize) throw()
      override {
    if (processedSize != nullptr) {
      *processedSize = 0;
    }
    if (size == 0) {
      return S_OK;
    }
    if (!pauseIfNeeded(pauseCallback_)) {
      return E_ABORT;
    }
    if (data == nullptr || !file_) {
      return E_FAIL;
    }
    file_.read(reinterpret_cast<char *>(data),
               static_cast<std::streamsize>(size));
    const std::streamsize readSize = file_.gcount();
    if (processedSize != nullptr) {
      *processedSize = static_cast<UInt32>(std::max<std::streamsize>(
          0, std::min<std::streamsize>(readSize, size)));
    }
    if (readSize > 0 || file_.eof()) {
      return S_OK;
    }
    return file_.bad() ? E_FAIL : S_OK;
  }

  STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin,
                  UInt64 *newPosition) throw() override {
    if (!file_) {
      return E_FAIL;
    }
    if (!pauseIfNeeded(pauseCallback_)) {
      return E_ABORT;
    }

    std::ios_base::seekdir direction = std::ios::beg;
    if (seekOrigin == STREAM_SEEK_CUR) {
      direction = std::ios::cur;
    } else if (seekOrigin == STREAM_SEEK_END) {
      direction = std::ios::end;
    } else if (seekOrigin != STREAM_SEEK_SET) {
      return STG_E_INVALIDFUNCTION;
    }

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), direction);
    if (!file_) {
      return STG_E_INVALIDFUNCTION;
    }
    if (newPosition != nullptr) {
      const auto position = file_.tellg();
      if (position < 0) {
        return E_FAIL;
      }
      *newPosition = static_cast<UInt64>(position);
    }
    return S_OK;
  }

  STDMETHOD(GetSize)(UInt64 *size) throw() override {
    if (size == nullptr) {
      return E_FAIL;
    }
    *size = size_;
    return S_OK;
  }

private:
  static constexpr std::size_t kInputBufferSize = 1u << 20;
  std::array<char, kInputBufferSize> buffer_{};
  std::ifstream file_;
  UInt64 size_ = 0;
  PauseCallback pauseCallback_;
  ULONG refCount_ = 0;
};

class SevenZipPauseCallbackScope {
public:
  SevenZipPauseCallbackScope(SevenZipInFileStream *stream,
                             PauseCallback pauseCallback)
      : stream_(stream) {
    if (stream_ != nullptr) {
      stream_->setPauseCallback(std::move(pauseCallback));
    }
  }

  ~SevenZipPauseCallbackScope() {
    if (stream_ != nullptr) {
      stream_->setPauseCallback(nullptr);
    }
  }

  SevenZipPauseCallbackScope(const SevenZipPauseCallbackScope &) = delete;
  SevenZipPauseCallbackScope &
  operator=(const SevenZipPauseCallbackScope &) = delete;

private:
  SevenZipInFileStream *stream_ = nullptr;
};

class SevenZipMemoryOutStream final : public ISequentialOutStream {
public:
  SevenZipMemoryOutStream(std::vector<unsigned char> &bytes,
                          PauseCallback pauseCallback)
      : bytes_(bytes), pauseCallback_(std::move(pauseCallback)) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
      *outObject = static_cast<ISequentialOutStream *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(Write)(const void *data, UInt32 size,
                   UInt32 *processedSize) throw() override {
    if (processedSize != nullptr) {
      *processedSize = 0;
    }
    if (size == 0) {
      return S_OK;
    }
    if (!pauseIfNeeded(pauseCallback_)) {
      return E_ABORT;
    }
    if (data == nullptr) {
      return E_FAIL;
    }
    const auto *bytes = static_cast<const unsigned char *>(data);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
    if (processedSize != nullptr) {
      *processedSize = size;
    }
    return S_OK;
  }

private:
  std::vector<unsigned char> &bytes_;
  PauseCallback pauseCallback_;
  ULONG refCount_ = 0;
};

class SevenZipFileOutStream final : public ISequentialOutStream {
public:
  SevenZipFileOutStream(const std::filesystem::path &path,
                        const std::stop_token *stopToken,
                        PauseCallback pauseCallback)
      : stopToken_(stopToken), pauseCallback_(std::move(pauseCallback)) {
    file_.open(path, std::ios::binary | std::ios::trunc);
  }

  bool isOpen() const { return file_.is_open(); }

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
      *outObject = static_cast<ISequentialOutStream *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(Write)(const void *data, UInt32 size,
                   UInt32 *processedSize) throw() override {
    if (processedSize != nullptr) {
      *processedSize = 0;
    }
    if (!unzipCheckpoint(stopToken_, pauseCallback_)) {
      return E_ABORT;
    }
    if (size == 0) {
      return S_OK;
    }
    if (data == nullptr || !file_) {
      return E_FAIL;
    }
    file_.write(static_cast<const char *>(data),
                static_cast<std::streamsize>(size));
    if (!file_) {
      return E_FAIL;
    }
    if (processedSize != nullptr) {
      *processedSize = size;
    }
    return S_OK;
  }

private:
  std::ofstream file_;
  const std::stop_token *stopToken_ = nullptr;
  PauseCallback pauseCallback_;
  ULONG refCount_ = 0;
};

class SevenZipExtractCallback final : public IArchiveExtractCallback {
public:
  explicit SevenZipExtractCallback(
      std::unordered_map<UInt32, FileData *> targets,
      PauseCallback pauseCallback)
      : targets_(std::move(targets)),
        pauseCallback_(std::move(pauseCallback)) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
      *outObject = static_cast<IArchiveExtractCallback *>(this);
    } else if (iid == IID_IProgress) {
      *outObject = static_cast<IProgress *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(SetTotal)(UInt64) throw() override { return S_OK; }
  STDMETHOD(SetCompleted)(const UInt64 *) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream,
                       Int32 askExtractMode) throw() override {
    if (outStream == nullptr) {
      return E_FAIL;
    }
    *outStream = nullptr;
    currentTarget_ = nullptr;
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
      return S_OK;
    }
    const auto it = targets_.find(index);
    if (it == targets_.end() || it->second == nullptr) {
      return S_OK;
    }

    currentTarget_ = it->second;
    currentTarget_->bytes.clear();
    auto *stream =
        new SevenZipMemoryOutStream(currentTarget_->bytes, pauseCallback_);
    ISequentialOutStream *streamInterface = stream;
    streamInterface->AddRef();
    *outStream = streamInterface;
    return S_OK;
  }

  STDMETHOD(PrepareOperation)(Int32) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(SetOperationResult)(Int32 opRes) throw() override {
    if (currentTarget_ != nullptr &&
        opRes != NArchive::NExtract::NOperationResult::kOK) {
      failed_ = true;
      operationResult_ = opRes;
    }
    currentTarget_ = nullptr;
    return S_OK;
  }

  bool failed() const { return failed_; }
  bool cancelled() const { return cancelled_; }
  Int32 operationResult() const { return operationResult_; }

private:
  std::unordered_map<UInt32, FileData *> targets_;
  PauseCallback pauseCallback_;
  FileData *currentTarget_ = nullptr;
  ULONG refCount_ = 0;
  bool failed_ = false;
  bool cancelled_ = false;
  Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
};

class SevenZipStreamingExtractCallback final : public IArchiveExtractCallback {
public:
  SevenZipStreamingExtractCallback(
      std::unordered_map<UInt32, std::filesystem::path> targets,
      FileDataCallback onFile, PauseCallback pauseCallback)
      : targets_(std::move(targets)), onFile_(std::move(onFile)),
        pauseCallback_(std::move(pauseCallback)) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
      *outObject = static_cast<IArchiveExtractCallback *>(this);
    } else if (iid == IID_IProgress) {
      *outObject = static_cast<IProgress *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(SetTotal)(UInt64) throw() override { return S_OK; }
  STDMETHOD(SetCompleted)(const UInt64 *) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream,
                       Int32 askExtractMode) throw() override {
    if (outStream == nullptr) {
      return E_FAIL;
    }
    *outStream = nullptr;
    currentFile_.reset();
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
      return S_OK;
    }
    const auto it = targets_.find(index);
    if (it == targets_.end()) {
      return S_OK;
    }

    currentFile_ = std::make_unique<FileData>();
    currentFile_->path = it->second;
    auto *stream =
        new SevenZipMemoryOutStream(currentFile_->bytes, pauseCallback_);
    ISequentialOutStream *streamInterface = stream;
    streamInterface->AddRef();
    *outStream = streamInterface;
    return S_OK;
  }

  STDMETHOD(PrepareOperation)(Int32) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(SetOperationResult)(Int32 opRes) throw() override {
    if (currentFile_ != nullptr) {
      if (opRes != NArchive::NExtract::NOperationResult::kOK) {
        failed_ = true;
        operationResult_ = opRes;
      } else if (!emitFileData(std::move(*currentFile_), onFile_,
                               nullptr)) {
        consumerCancelled_ = true;
        cancelled_ = true;
        currentFile_.reset();
        return E_ABORT;
      } else {
        ++emittedFiles_;
      }
    }
    currentFile_.reset();
    return S_OK;
  }

  bool failed() const { return failed_; }
  bool cancelled() const { return cancelled_; }
  bool consumerCancelled() const { return consumerCancelled_; }
  Int32 operationResult() const { return operationResult_; }
  std::size_t emittedFiles() const { return emittedFiles_; }

private:
  std::unordered_map<UInt32, std::filesystem::path> targets_;
  FileDataCallback onFile_;
  PauseCallback pauseCallback_;
  std::unique_ptr<FileData> currentFile_;
  ULONG refCount_ = 0;
  bool failed_ = false;
  bool cancelled_ = false;
  bool consumerCancelled_ = false;
  Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
  std::size_t emittedFiles_ = 0;
};

struct SevenZipStreamingTarget {
  std::filesystem::path path;
  std::uint64_t size = 0;
};

class SevenZipThrottledStreamingExtractCallback final
    : public IArchiveExtractCallback {
public:
  SevenZipThrottledStreamingExtractCallback(
      std::unordered_map<UInt32, SevenZipStreamingTarget> targets,
      FileDataCallback onFile,
      std::function<bool(std::uint64_t)> acquireBytes,
      std::function<void(std::uint64_t)> releaseBytes,
      PauseCallback pauseCallback)
      : targets_(std::move(targets)), onFile_(std::move(onFile)),
        acquireBytes_(std::move(acquireBytes)),
        releaseBytes_(std::move(releaseBytes)),
        pauseCallback_(std::move(pauseCallback)) {}

  ~SevenZipThrottledStreamingExtractCallback() { releaseCurrentBytes(); }

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
      *outObject = static_cast<IArchiveExtractCallback *>(this);
    } else if (iid == IID_IProgress) {
      *outObject = static_cast<IProgress *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(SetTotal)(UInt64) throw() override { return S_OK; }
  STDMETHOD(SetCompleted)(const UInt64 *) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream,
                       Int32 askExtractMode) throw() override {
    if (outStream == nullptr) {
      return E_FAIL;
    }
    *outStream = nullptr;
    currentFile_.reset();
    releaseCurrentBytes();
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
      return S_OK;
    }
    const auto it = targets_.find(index);
    if (it == targets_.end()) {
      return S_OK;
    }

    currentReservedBytes_ = it->second.size;
    if (acquireBytes_ && !acquireBytes_(currentReservedBytes_)) {
      currentReservedBytes_ = 0;
      cancelled_ = true;
      return E_ABORT;
    }
    currentBytesAcquired_ = true;

    currentFile_ = std::make_unique<FileData>();
    currentFile_->path = it->second.path;
    if (it->second.size > 0) {
      reserveBufferedBytes(currentFile_->bytes, it->second.size);
    }
    auto *stream =
        new SevenZipMemoryOutStream(currentFile_->bytes, pauseCallback_);
    ISequentialOutStream *streamInterface = stream;
    streamInterface->AddRef();
    *outStream = streamInterface;
    return S_OK;
  }

  STDMETHOD(PrepareOperation)(Int32) throw() override {
    if (!pauseIfNeeded(pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(SetOperationResult)(Int32 opRes) throw() override {
    if (currentFile_ != nullptr) {
      if (opRes != NArchive::NExtract::NOperationResult::kOK) {
        failed_ = true;
        operationResult_ = opRes;
      } else {
        const auto callbackStart = std::chrono::steady_clock::now();
        if (!emitFileData(std::move(*currentFile_), onFile_, nullptr)) {
          callbackMicros_ +=
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - callbackStart)
                  .count();
          consumerCancelled_ = true;
          cancelled_ = true;
          currentFile_.reset();
          releaseCurrentBytes();
          return E_ABORT;
        }
        callbackMicros_ +=
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - callbackStart)
                .count();
        ++emittedFiles_;
      }
    }
    currentFile_.reset();
    releaseCurrentBytes();
    return S_OK;
  }

  bool failed() const { return failed_; }
  bool cancelled() const { return cancelled_; }
  bool consumerCancelled() const { return consumerCancelled_; }
  Int32 operationResult() const { return operationResult_; }
  std::size_t emittedFiles() const { return emittedFiles_; }
  long long callbackMicros() const { return callbackMicros_; }

private:
  void releaseCurrentBytes() {
    if (!currentBytesAcquired_) {
      return;
    }
    if (releaseBytes_) {
      releaseBytes_(currentReservedBytes_);
    }
    currentReservedBytes_ = 0;
    currentBytesAcquired_ = false;
  }

  std::unordered_map<UInt32, SevenZipStreamingTarget> targets_;
  FileDataCallback onFile_;
  std::function<bool(std::uint64_t)> acquireBytes_;
  std::function<void(std::uint64_t)> releaseBytes_;
  PauseCallback pauseCallback_;
  std::unique_ptr<FileData> currentFile_;
  std::uint64_t currentReservedBytes_ = 0;
  ULONG refCount_ = 0;
  bool currentBytesAcquired_ = false;
  bool failed_ = false;
  bool cancelled_ = false;
  bool consumerCancelled_ = false;
  Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
  std::size_t emittedFiles_ = 0;
  long long callbackMicros_ = 0;
};

class SevenZipFullExtractCallback final : public IArchiveExtractCallback {
public:
  SevenZipFullExtractCallback(std::filesystem::path outputFolder,
                              std::unordered_map<UInt32, Entry> entries,
                              std::uint64_t totalFiles,
                              const std::stop_token *stopToken,
                              UnzipProgressCallback progressCallback,
                              PauseCallback pauseCallback)
      : outputFolder_(std::move(outputFolder)), entries_(std::move(entries)),
        totalFiles_(std::max<std::uint64_t>(totalFiles, 1)),
        stopToken_(stopToken), progressCallback_(std::move(progressCallback)),
        pauseCallback_(std::move(pauseCallback)) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
      *outObject = static_cast<IArchiveExtractCallback *>(this);
    } else if (iid == IID_IProgress) {
      *outObject = static_cast<IProgress *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(SetTotal)(UInt64) throw() override { return S_OK; }

  STDMETHOD(SetCompleted)(const UInt64 *) throw() override {
    if (!unzipCheckpoint(stopToken_, pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream,
                       Int32 askExtractMode) throw() override {
    if (outStream == nullptr) {
      return E_FAIL;
    }
    *outStream = nullptr;
    currentEntry_ = nullptr;
    if (!unzipCheckpoint(stopToken_, pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
      return S_OK;
    }
    const auto it = entries_.find(index);
    if (it == entries_.end() || it->second.directory) {
      return S_OK;
    }

    std::error_code error;
    const std::filesystem::path outputPath = outputFolder_ / it->second.path;
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
      failed_ = true;
      return E_FAIL;
    }

    auto *stream =
        new SevenZipFileOutStream(outputPath, stopToken_, pauseCallback_);
    if (!stream->isOpen()) {
      delete stream;
      failed_ = true;
      return E_FAIL;
    }

    currentEntry_ = &it->second;
    ISequentialOutStream *streamInterface = stream;
    streamInterface->AddRef();
    *outStream = streamInterface;
    return S_OK;
  }

  STDMETHOD(PrepareOperation)(Int32) throw() override {
    if (!unzipCheckpoint(stopToken_, pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHOD(SetOperationResult)(Int32 opRes) throw() override {
    if (currentEntry_ != nullptr) {
      if (opRes != NArchive::NExtract::NOperationResult::kOK) {
        failed_ = true;
        operationResult_ = opRes;
      } else {
        ++completedFiles_;
        reportUnzipProgress(
            progressCallback_,
            0.08 + 0.88 * (static_cast<double>(completedFiles_) /
                           static_cast<double>(totalFiles_)),
            completedFiles_, totalFiles_, "Unzipping archive");
      }
    }
    currentEntry_ = nullptr;
    if (!unzipCheckpoint(stopToken_, pauseCallback_)) {
      cancelled_ = true;
      return E_ABORT;
    }
    return S_OK;
  }

  bool failed() const { return failed_; }
  bool cancelled() const { return cancelled_; }
  Int32 operationResult() const { return operationResult_; }

private:
  std::filesystem::path outputFolder_;
  std::unordered_map<UInt32, Entry> entries_;
  std::uint64_t totalFiles_ = 1;
  const std::stop_token *stopToken_ = nullptr;
  UnzipProgressCallback progressCallback_;
  PauseCallback pauseCallback_;
  const Entry *currentEntry_ = nullptr;
  std::uint64_t completedFiles_ = 0;
  ULONG refCount_ = 0;
  bool failed_ = false;
  bool cancelled_ = false;
  Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
};

struct SevenZipArchiveState {
  ~SevenZipArchiveState() {
    std::lock_guard<std::mutex> lock(mutex);
    if (archive.Interface() != nullptr) {
      archive->Close();
    }
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  unsigned char formatId = 0;
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  SevenZipInFileStream *inputStream = nullptr;
  std::mutex mutex;
  std::uint64_t lastUse = 0;
};

constexpr std::size_t kMaxOpenSevenZipArchives = 4;
std::mutex gSevenZipArchiveMutex;
std::unordered_map<std::string, std::shared_ptr<SevenZipArchiveState>>
    gSevenZipArchiveCache;
std::uint64_t gSevenZipArchiveUseCounter = 0;

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         SevenZipFormat &formatUsed,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback = nullptr);
bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         unsigned char formatId,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback = nullptr);

bool openSevenZipArchiveWithFormat(const std::filesystem::path &archivePath,
                                   SevenZipFormat format,
                                   CMyComPtr<IInArchive> &archive,
                                   CMyComPtr<IInStream> &stream,
                                   std::string *errorMessage,
                                   const PauseCallback &pauseCallback) {
  archive.Release();
  stream.Release();

  IInArchive *rawArchive = nullptr;
  const GUID formatId = sevenZipFormatGuid(format);
  HRESULT result =
      createSevenZipObject(&formatId, &IID_IInArchive,
                           reinterpret_cast<void **>(&rawArchive));
  if (result != S_OK || rawArchive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }
  CMyComPtr<IInArchive> archiveHandle;
  archiveHandle.Attach(rawArchive);

  std::uintmax_t archiveSize = 0;
  std::filesystem::file_time_type ignoredMtime{};
  if (!fileState(archivePath, archiveSize, ignoredMtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    return false;
  }

  auto *fileStream =
      new SevenZipInFileStream(archivePath, archiveSize, pauseCallback);
  if (!fileStream->isOpen()) {
    delete fileStream;
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open archive file: " + pathForLog(archivePath);
    }
    return false;
  }

  IInStream *streamInterface = fileStream;
  streamInterface->AddRef();
  CMyComPtr<IInStream> streamHandle;
  streamHandle.Attach(streamInterface);

  UInt64 maxCheckStartPosition = 0;
  result = archiveHandle->Open(streamHandle, &maxCheckStartPosition, nullptr);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  archive = archiveHandle;
  stream = streamHandle;
  return true;
}

void trimSevenZipArchiveCacheLocked() {
  while (gSevenZipArchiveCache.size() > kMaxOpenSevenZipArchives) {
    auto evictIt = gSevenZipArchiveCache.end();
    for (auto it = gSevenZipArchiveCache.begin();
         it != gSevenZipArchiveCache.end(); ++it) {
      if (it->second == nullptr || it->second.use_count() > 1) {
        continue;
      }
      if (evictIt == gSevenZipArchiveCache.end() ||
          it->second->lastUse < evictIt->second->lastUse) {
        evictIt = it;
      }
    }
    if (evictIt == gSevenZipArchiveCache.end()) {
      return;
    }
    appendDebugLogLineImpl("Closing cached 7-Zip archive: " + evictIt->first);
    gSevenZipArchiveCache.erase(evictIt);
  }
}

std::shared_ptr<SevenZipArchiveState> openCachedSevenZipArchive(
    const std::filesystem::path &archivePath, unsigned char requestedFormatId,
    bool *cacheHit, long long *openMs, std::string *errorMessage,
    const PauseCallback &pauseCallback = nullptr) {
  if (cacheHit != nullptr) {
    *cacheHit = false;
  }
  if (openMs != nullptr) {
    *openMs = 0;
  }

  std::uintmax_t archiveSize = 0;
  std::filesystem::file_time_type archiveMtime{};
  if (!fileState(archivePath, archiveSize, archiveMtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  {
    std::lock_guard<std::mutex> cacheLock(gSevenZipArchiveMutex);
    const auto cachedIt = gSevenZipArchiveCache.find(key);
    if (cachedIt != gSevenZipArchiveCache.end()) {
      const auto &cached = cachedIt->second;
      if (cached != nullptr && cached->size == archiveSize &&
          cached->mtime == archiveMtime &&
          (requestedFormatId == 0 || cached->formatId == requestedFormatId)) {
        cached->lastUse = ++gSevenZipArchiveUseCounter;
        if (cacheHit != nullptr) {
          *cacheHit = true;
        }
        return cached;
      }
      gSevenZipArchiveCache.erase(cachedIt);
      appendDebugLogLineImpl("Invalidated cached 7-Zip archive: " + key);
    }
  }

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  SevenZipFormat formatUsed = SevenZipFormat::SevenZip;
  const bool opened =
      requestedFormatId == 0
          ? openSevenZipArchive(archivePath, archive, stream, formatUsed,
                                errorMessage, pauseCallback)
          : openSevenZipArchive(archivePath, requestedFormatId, archive, stream,
                                errorMessage, pauseCallback);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count();
  if (openMs != nullptr) {
    *openMs = elapsed;
  }
  if (!opened) {
    return nullptr;
  }
  if (requestedFormatId != 0) {
    formatUsed = static_cast<SevenZipFormat>(requestedFormatId);
  }

  auto state = std::make_shared<SevenZipArchiveState>();
  state->size = archiveSize;
  state->mtime = archiveMtime;
  state->formatId = static_cast<unsigned char>(formatUsed);
  state->archive = archive;
  state->stream = stream;
  state->inputStream =
      static_cast<SevenZipInFileStream *>(state->stream.Interface());
  {
    std::lock_guard<std::mutex> cacheLock(gSevenZipArchiveMutex);
    const auto cachedIt = gSevenZipArchiveCache.find(key);
    if (cachedIt != gSevenZipArchiveCache.end()) {
      const auto &cached = cachedIt->second;
      if (cached != nullptr && cached->size == archiveSize &&
          cached->mtime == archiveMtime &&
          (requestedFormatId == 0 || cached->formatId == requestedFormatId)) {
        cached->lastUse = ++gSevenZipArchiveUseCounter;
        if (cacheHit != nullptr) {
          *cacheHit = true;
        }
        return cached;
      }
      gSevenZipArchiveCache.erase(cachedIt);
      appendDebugLogLineImpl("Invalidated cached 7-Zip archive: " + key);
    }

    state->lastUse = ++gSevenZipArchiveUseCounter;
    gSevenZipArchiveCache[key] = state;
    appendDebugLogLineImpl("Opened cached 7-Zip archive: " + key +
                           " openMs=" + std::to_string(elapsed));
    trimSevenZipArchiveCacheLocked();
  }
  return state;
}

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         SevenZipFormat &formatUsed,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback) {
  const auto candidates = sevenZipFormatCandidates(archivePath);
  if (candidates.empty()) {
    return false;
  }

  std::string lastError;
  for (SevenZipFormat format : candidates) {
    std::string currentError;
    if (openSevenZipArchiveWithFormat(archivePath, format, archive, stream,
                                      &currentError, pauseCallback)) {
      formatUsed = format;
      return true;
    }
    lastError = std::move(currentError);
  }

  if (errorMessage != nullptr) {
    *errorMessage = lastError.empty() ? "Could not open archive with 7-Zip SDK."
                                      : lastError;
  }
  return false;
}

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         unsigned char formatId,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback) {
  return openSevenZipArchiveWithFormat(
      archivePath, static_cast<SevenZipFormat>(formatId), archive, stream,
      errorMessage, pauseCallback);
}

bool listSevenZipEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         unsigned char &formatUsed,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback) {
  entries.clear();
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }

  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, 0, nullptr, &openMs, errorMessage, pauseCallback);
  if (archiveState == nullptr) {
    return false;
  }
  formatUsed = archiveState->formatId;

  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  SevenZipPauseCallbackScope pauseScope(archiveState->inputStream,
                                        pauseCallback);
  IInArchive *archive = archiveState->archive.Interface();
  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  entries.reserve(itemCount);
  std::size_t skippedUnnamed = 0;
  std::size_t skippedUnsafe = 0;
  std::size_t skippedSystem = 0;
  std::size_t skippedEncrypted = 0;
  std::size_t solidEntries = 0;
  for (UInt32 index = 0; index < itemCount; ++index) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      entries.clear();
      return false;
    }
    auto entryName = sevenZipStringProperty(archive, index, kpidPath);
    if (!entryName.has_value() || entryName->empty()) {
      entryName = sevenZipStringProperty(archive, index, kpidName);
    }
    if (!entryName.has_value() || entryName->empty()) {
      ++skippedUnnamed;
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(*entryName, relativePath)) {
      ++skippedUnsafe;
      continue;
    }
    if (isSystemEntryPath(relativePath)) {
      ++skippedSystem;
      continue;
    }

    const bool directory =
        sevenZipBoolProperty(archive, index, kpidIsDir, false);
    const bool solid = !directory &&
                       sevenZipBoolProperty(archive, index, kpidSolid, false);
    if (!directory &&
        sevenZipBoolProperty(archive, index, kpidEncrypted, false)) {
      ++skippedEncrypted;
      continue;
    }
    if (solid) {
      ++solidEntries;
    }

    entries.push_back({
        .path = relativePath,
        .directory = directory,
        .size = sevenZipUInt64Property(archive, index, kpidSize),
        .order = static_cast<std::size_t>(index),
        .offset = -1,
        .solid = solid,
    });
  }

  appendDebugLogLineImpl("Indexed 7-Zip archive from cached open handle: " +
                         pathForLog(archivePath) +
                         " entries=" + std::to_string(entries.size()) +
                         " solidEntries=" + std::to_string(solidEntries) +
                         " formatId=" + std::to_string(formatUsed) +
                         " openMs=" + std::to_string(openMs));
  if (skippedUnnamed > 0 || skippedUnsafe > 0 || skippedSystem > 0 ||
      skippedEncrypted > 0) {
    appendDebugLogLineImpl(
        "7-Zip skipped entries while indexing " + pathForLog(archivePath) +
        ": unnamed=" + std::to_string(skippedUnnamed) +
        " unsafe=" + std::to_string(skippedUnsafe) +
        " system=" + std::to_string(skippedSystem) +
        " encrypted=" + std::to_string(skippedEncrypted));
  }
  return true;
}

#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE

bool isValidUtf8(const std::string &value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char c = bytes[i];
    if (c <= 0x7f) {
      ++i;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) {
      if (i + 1 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80) {
        return false;
      }
      i += 2;
      continue;
    }
    if (c == 0xe0) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0xa0 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if ((c >= 0xe1 && c <= 0xec) || c == 0xee || c == 0xef) {
      if (i + 2 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xed) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x9f || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xf0) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x90 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c >= 0xf1 && c <= 0xf3) {
      if (i + 3 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c == 0xf4) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x8f || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
std::optional<std::string> convertTextToUtf8(const std::string &input,
                                             const char *fromEncoding) {
  iconv_t converter = iconv_open("UTF-8", fromEncoding);
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return std::nullopt;
  }

  std::string output(std::max<size_t>(input.size() * 4, 32), '\0');
  char *inputPtr = const_cast<char *>(input.data());
  size_t inputBytes = input.size();
  size_t outputOffset = 0;

  while (inputBytes > 0) {
    char *outputPtr = output.data() + outputOffset;
    size_t outputBytes = output.size() - outputOffset;
    const size_t status =
        iconv(converter, &inputPtr, &inputBytes, &outputPtr, &outputBytes);
    outputOffset = output.size() - outputBytes;
    if (status != static_cast<size_t>(-1)) {
      continue;
    }
    if (errno != E2BIG) {
      iconv_close(converter);
      return std::nullopt;
    }
    output.resize(output.size() * 2);
  }

  iconv_close(converter);
  output.resize(outputOffset);
  if (!isValidUtf8(output)) {
    return std::nullopt;
  }
  return output;
}
#endif

std::string wideToUtf8(const wchar_t *input) {
  if (input == nullptr) {
    return "";
  }
  return wideStringToUtf8(input, std::wcslen(input));
}

std::string entryPathnameUtf8(archive_entry *entry) {
  if (entry == nullptr) {
    return "";
  }
  if (const char *utf8Name = archive_entry_pathname_utf8(entry);
      utf8Name != nullptr && utf8Name[0] != '\0') {
    return utf8Name;
  }
  if (const wchar_t *wideName = archive_entry_pathname_w(entry);
      wideName != nullptr && wideName[0] != L'\0') {
    return wideToUtf8(wideName);
  }

  const char *rawName = archive_entry_pathname(entry);
  if (rawName == nullptr || rawName[0] == '\0') {
    return "";
  }

  const std::string raw(rawName);
  if (isValidUtf8(raw)) {
    return raw;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
  for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (const auto converted = convertTextToUtf8(raw, encoding)) {
      return *converted;
    }
  }
#endif
  return raw;
}

struct ArchiveEntryInfo {
  std::filesystem::path relativePath;
  bool directory = false;
  bool regular = false;
  std::uint64_t size = 0;
};

bool archiveEntryInfo(archive_entry *entry, const std::string &entryName,
                      ArchiveEntryInfo &info) {
  if (entry == nullptr) {
    return false;
  }

  std::filesystem::path relativePath;
  if (!safeEntryPath(entryName, relativePath)) {
    return false;
  }
  if (isSystemEntryPath(relativePath)) {
    return false;
  }

  const auto fileType = archive_entry_filetype(entry);
  const bool typeIsSet = archive_entry_filetype_is_set(entry) != 0;
  const bool nameLooksDirectory =
      !entryName.empty() &&
      (entryName.back() == '/' || entryName.back() == '\\');
  const bool directory =
      fileType == AE_IFDIR ||
      ((!typeIsSet || fileType == 0) && nameLooksDirectory);
  const bool regular =
      !directory && (!typeIsSet || fileType == 0 || fileType == AE_IFREG);
  if (!directory && !regular) {
    return false;
  }

  info = {
      .relativePath = relativePath,
      .directory = directory,
      .regular = regular,
      .size = archive_entry_size_is_set(entry)
                  ? static_cast<std::uint64_t>(archive_entry_size(entry))
                  : 0,
  };
  return true;
}

bool localeNameLooksArchiveCompatible(const char *name) {
  if (name == nullptr) {
    return false;
  }
  const std::string lower = lowerCopy(name);
  return lower.find("utf") != std::string::npos ||
         lower.find("sjis") != std::string::npos ||
         lower.find("shift") != std::string::npos ||
         lower.find("932") != std::string::npos;
}

void ensureArchiveFilenameLocale() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (localeNameLooksArchiveCompatible(std::setlocale(LC_CTYPE, nullptr))) {
      return;
    }
    for (const char *candidate :
         {"", "C.UTF-8", "en_US.UTF-8", "ja_JP.UTF-8", "ko_KR.UTF-8",
          "UTF-8", "ja_JP.SJIS", "ja_JP.Shift_JIS", "ja_JP.CP932",
          "Shift_JIS", "CP932", "SJIS"}) {
      const char *selected = std::setlocale(LC_CTYPE, candidate);
      if (localeNameLooksArchiveCompatible(selected)) {
        return;
      }
    }
  });
}

std::string archiveErrorString(archive *archiveHandle,
                               const std::string &fallback) {
  if (archiveHandle == nullptr ||
      archive_error_string(archiveHandle) == nullptr) {
    return fallback;
  }
  return archive_error_string(archiveHandle);
}

bool trySetArchiveHeaderCharset(archive *archiveHandle, const char *charset) {
  if (archiveHandle == nullptr || charset == nullptr || charset[0] == '\0') {
    return false;
  }

  bool applied = false;
  for (const char *format :
       {"zip", "7zip", "rar", "lha", "tar", "cab", "cpio"}) {
    const int status =
        archive_read_set_option(archiveHandle, format, "hdrcharset", charset);
    if (status == ARCHIVE_OK || status == ARCHIVE_WARN) {
      applied = true;
    }
  }
  return applied;
}

void configureArchiveReader(archive *archiveHandle) {
  archive_read_support_filter_all(archiveHandle);
  archive_read_support_format_all(archiveHandle);
  for (const char *charset : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (trySetArchiveHeaderCharset(archiveHandle, charset)) {
      break;
    }
  }
}

ArchiveReadHandle openArchive(const std::filesystem::path &archivePath,
                              std::string *errorMessage) {
  ensureArchiveFilenameLocale();
  auto archiveStorage = makeArchiveReadHandle();
  archive *archiveHandle = archiveStorage.get();
  if (archiveHandle == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not initialize archive reader.";
    }
    return archiveStorage;
  }
  configureArchiveReader(archiveHandle);

  const std::string archiveText = fspath_to_utf8(archivePath);
  const int status =
      archive_read_open_filename(archiveHandle, archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage =
          "Could not open archive: " + archiveErrorString(archiveHandle, "");
    }
    archiveStorage.reset();
    return archiveStorage;
  }
  return archiveStorage;
}

bool listEntriesUncached(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback) {
  entries.clear();
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  archive_entry *entry = nullptr;
  for (;;) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      entries.clear();
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    entries.push_back({
        .path = info.relativePath,
        .directory = info.directory,
        .size = info.size,
        .order = entries.size(),
    });
    archive_read_data_skip(archiveHandle);
  }

  return true;
}

bool readArchiveEntry(const std::filesystem::path &archivePath,
                      const std::filesystem::path &innerPath,
                      std::vector<unsigned char> &bytes,
                      std::string *errorMessage,
                      const PauseCallback &pauseCallback = nullptr,
                      std::uintmax_t maximumBytes =
                          std::numeric_limits<std::uintmax_t>::max()) {
  bytes.clear();
  const std::string target = normalizeEntryName(innerPath.generic_string());
  if (target.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry path is empty.";
    }
    return false;
  }

  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  archive_entry *entry = nullptr;
  for (;;) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(entryPathnameUtf8(entry), relativePath)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (normalizeEntryName(relativePath.generic_string()) != target) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const la_int64_t entrySize = archive_entry_size(entry);
    if (archive_entry_size_is_set(entry) && entrySize > 0 &&
        static_cast<std::uintmax_t>(entrySize) > maximumBytes) {
      if (errorMessage != nullptr) {
        *errorMessage = "Archive entry exceeds bounded read limit: " + target;
      }
      return false;
    }
    if (archive_entry_size_is_set(entry) && entrySize > 0) {
      reserveBufferedBytes(bytes, static_cast<std::uintmax_t>(entrySize));
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
      if (!pauseIfNeeded(pauseCallback, errorMessage)) {
        return false;
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        return true;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        return false;
      }
      if (static_cast<std::uintmax_t>(count) > maximumBytes - bytes.size()) {
        if (errorMessage != nullptr) {
          *errorMessage = "Archive entry exceeds bounded read limit: " + target;
        }
        bytes.clear();
        return false;
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
  }

  if (errorMessage != nullptr) {
    *errorMessage = "Archive entry not found: " + target;
  }
  return false;
}

bool readArchiveEntriesUncached(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  while (!targets.empty()) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    if (range.has_value()) {
      if (currentOrder < range->start) {
        archive_read_data_skip(archiveHandle);
        continue;
      }
      if (currentOrder > range->end) {
        archive_read_data_skip(archiveHandle);
        break;
      }
    }

    if (!info.regular) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::string normalized =
        normalizeEntryName(info.relativePath.generic_string());
    const auto targetIt = targets.find(normalized);
    if (targetIt == targets.end()) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    FileData file;
    file.path = info.relativePath;
    const la_int64_t entrySize = archive_entry_size(entry);
    if (archive_entry_size_is_set(entry) && entrySize > 0) {
      reserveBufferedBytes(file.bytes,
                           static_cast<std::uintmax_t>(entrySize));
    }
    for (;;) {
      if (!pauseIfNeeded(pauseCallback, errorMessage)) {
        files.clear();
        return false;
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        return false;
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    files.push_back(std::move(file));
    targets.erase(targetIt);
  }

  return true;
}

bool readArchiveEntriesUncachedStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  while (!targets.empty()) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    if (range.has_value()) {
      if (currentOrder < range->start) {
        archive_read_data_skip(archiveHandle);
        continue;
      }
      if (currentOrder > range->end) {
        archive_read_data_skip(archiveHandle);
        break;
      }
    }

    if (!info.regular) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::string normalized =
        normalizeEntryName(info.relativePath.generic_string());
    const auto targetIt = targets.find(normalized);
    if (targetIt == targets.end()) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    FileData file;
    file.path = info.relativePath;
    const la_int64_t entrySize = archive_entry_size(entry);
    if (archive_entry_size_is_set(entry) && entrySize > 0) {
      reserveBufferedBytes(file.bytes,
                           static_cast<std::uintmax_t>(entrySize));
    }
    for (;;) {
      if (!pauseIfNeeded(pauseCallback, errorMessage)) {
        return false;
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        return false;
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    targets.erase(targetIt);
    if (!emitFileData(std::move(file), onFile, errorMessage)) {
      return false;
    }
  }

  return true;
}

bool extractArchiveFullyWithLibarchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputFolder,
    const std::shared_ptr<const CachedIndex> &index,
    const std::stop_token *stopToken,
    const UnzipProgressCallback &progressCallback,
    const PauseCallback &pauseCallback,
    std::string *errorMessage) {
  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  std::uint64_t totalFiles = 0;
  if (index != nullptr) {
    for (const Entry &entry : index->entries) {
      if (!entry.directory) {
        ++totalFiles;
      }
    }
  }
  totalFiles = std::max<std::uint64_t>(totalFiles, 1);

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    return false;
  };

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::uint64_t completedFiles = 0;
  for (;;) {
    if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
      return fail("Unzip cancelled");
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      return fail("Could not read archive: " +
                  archiveErrorString(archiveHandle, ""));
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::filesystem::path outputPath = outputFolder / info.relativePath;
    std::error_code error;
    if (info.directory) {
      std::filesystem::create_directories(outputPath, error);
      if (error) {
        return fail("Could not create unzip folder: " + error.message());
      }
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (!info.regular) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
      return fail("Could not create unzip subfolder: " + error.message());
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      return fail("Could not write unzipped file: " + pathForLog(outputPath));
    }
    for (;;) {
      if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
        return fail("Unzip cancelled");
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        return fail("Could not read archive entry: " +
                    archiveErrorString(archiveHandle, ""));
      }
      output.write(reinterpret_cast<const char *>(buffer.data()), count);
      if (!output) {
        return fail("Failed while writing unzipped file: " +
                    pathForLog(outputPath));
      }
    }
    ++completedFiles;
    reportUnzipProgress(progressCallback,
                        0.08 + 0.88 * (static_cast<double>(completedFiles) /
                                      static_cast<double>(totalFiles)),
                        completedFiles, totalFiles, "Unzipping archive");
  }

  appendDebugLogLineImpl("Finished full libarchive unzip: " +
                         pathForLog(archivePath) +
                         " files=" + std::to_string(completedFiles));
  return true;
}

#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
constexpr mz_uint kZipIndexPauseCheckInterval = 256;

bool listZipEntries(const std::filesystem::path &archivePath,
                    std::vector<Entry> &entries, std::string *errorMessage,
                    const PauseCallback &pauseCallback);
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
bool listUnarrRarEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         bool &containsSolidEntries,
                         std::string *errorMessage,
                         const PauseCallback &pauseCallback);
#endif

void buildIndexLookups(CachedIndex &index) {
  index.exact.clear();
  index.lower.clear();
  index.exact.reserve(index.entries.size());
  index.lower.reserve(index.entries.size());
  for (std::size_t i = 0; i < index.entries.size(); ++i) {
    const Entry &entry = index.entries[i];
    if (entry.directory) {
      continue;
    }
    const std::string normalized = entry.path.generic_string();
    if (normalized.empty()) {
      continue;
    }
    index.exact.emplace(normalized, i);
    index.lower.emplace(lowerCopy(normalized), i);
  }
}

// ---------------------------------------------------------------------------
// On-disk archive index persistence
//
// The in-memory gIndexCache is rebuilt on every cold start, which for large
// archives (hundreds of thousands of entries) costs seconds of miniz/7-Zip
// listing before any chart can be read. When a cache directory is configured,
// each built CachedIndex is serialized to a per-archive file keyed by a hash
// of the normalized archive path, and reloaded on the next launch when the
// archive's size and mtime still match (so the entry offsets stay valid).
// ---------------------------------------------------------------------------

std::string hex64(std::uint64_t value);
std::uint64_t fnv1a64(const std::string &value);

std::filesystem::path archiveIndexCacheFilePath(const std::string &key) {
  return archiveIndexCacheDirectory() /
         ("archive-index-" + hex64(fnv1a64(key)) + ".idx");
}

// Internal implementation of pruneArchiveIndexCache; kept in the anonymous
// namespace because it uses anonymous helpers (archiveKey). The public
// entry point below delegates to this.
std::size_t pruneArchiveIndexCacheImpl(
    const std::vector<std::filesystem::path> &liveArchivePaths) {
  const std::filesystem::path directory = archiveIndexCacheDirectory();
  if (directory.empty()) {
    return 0;
  }
  std::unordered_set<std::string> liveArchiveKeys;
  std::unordered_set<std::uint64_t> liveArchiveHashes;
  liveArchiveKeys.reserve(liveArchivePaths.size());
  liveArchiveHashes.reserve(liveArchivePaths.size());
  for (const auto &path : liveArchivePaths) {
    const std::string key = archiveKey(path);
    liveArchiveKeys.insert(key);
    liveArchiveHashes.insert(fnv1a64(key));
  }
  const auto parseHashFromFileName = [](const std::string &fileName)
      -> std::optional<std::uint64_t> {
    constexpr std::string_view prefix = "archive-index-";
    // A crash between the temporary write and the rename in
    // writeCachedIndexToDisk leaves an orphaned "<name>.idx.tmp" sibling; prune
    // those the same way, so they cannot accumulate across refreshes.
    constexpr std::string_view suffixes[] = {".idx", ".idx.tmp"};
    std::uint64_t value = 0;
    for (const auto &suffix : suffixes) {
      if (fileName.size() != prefix.size() + 16 + suffix.size() ||
          fileName.compare(0, prefix.size(), prefix) != 0 ||
          fileName.compare(fileName.size() - suffix.size(), suffix.size(),
                           suffix) != 0) {
        continue;
      }
      value = 0;
      const std::size_t hexStart = prefix.size();
      for (std::size_t i = 0; i < 16; ++i) {
        const char character = fileName[hexStart + i];
        const unsigned digit =
            (character >= '0' && character <= '9')
                ? static_cast<unsigned>(character - '0')
                : (character >= 'a' && character <= 'f')
                      ? static_cast<unsigned>(character - 'a' + 10)
                      : 16;
        if (digit >= 16) {
          return std::nullopt;
        }
        value = (value << 4) | digit;
      }
      return value;
    }
    return std::nullopt;
  };

  std::size_t removed = 0;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied,
      error);
  for (const auto end = std::filesystem::directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    std::error_code typeError;
    if (!iterator->is_regular_file(typeError) || typeError) {
      continue;
    }
    const std::filesystem::path filePath = iterator->path();
    const std::string fileName = filePath.filename().string();
    const bool orphanTmpIndex =
        fileName.size() > 4 &&
        fileName.compare(fileName.size() - 8, 8, ".idx.tmp") == 0;
    if (filePath.extension() != ".idx" && !orphanTmpIndex) {
      continue;
    }
    const auto fileNameHash = parseHashFromFileName(fileName);
    if (fileNameHash.has_value() &&
        !liveArchiveHashes.contains(*fileNameHash)) {
      // The file name encodes a hash no live archive produces, so the key
      // cannot match; remove it without reading the stored key.
      std::error_code removeError;
      std::filesystem::remove(filePath, removeError);
      if (!removeError) {
        ++removed;
      }
      continue;
    }
    // A live archive's ".idx.tmp" may be mid-write (writeCachedIndexToDisk
    // holds no lock against prune), so its header may be partial and
    // unreadable. Never remove a live archive's temp file here; the writer
    // cleans it up itself, and only truly orphaned temps (no live hash) are
    // removed above.
    if (orphanTmpIndex && fileNameHash.has_value()) {
      continue;
    }
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
      continue;
    }
    std::error_code sizeError;
    const std::uintmax_t fileBytes =
        std::filesystem::file_size(filePath, sizeError);
    std::uint8_t version = 0;
    file.read(reinterpret_cast<char *>(&version), sizeof(version));
    std::uint64_t keyLen = 0;
    file.read(reinterpret_cast<char *>(&keyLen), sizeof(keyLen));
    bool shouldRemove = true;
    if (file.good() && version == 2 &&
        (!sizeError && keyLen <= fileBytes) &&
        keyLen <= (1024ull * 1024ull * 1024ull)) {
      std::string storedKey(static_cast<std::size_t>(keyLen), '\0');
      file.read(storedKey.data(), static_cast<std::streamsize>(keyLen));
      if (file.good() && liveArchiveKeys.contains(storedKey)) {
        shouldRemove = false;
      }
    }
    if (shouldRemove) {
      std::error_code removeError;
      std::filesystem::remove(filePath, removeError);
      if (!removeError) {
        ++removed;
      }
    }
  }
  return removed;
}

bool writeCachedIndexToDisk(const std::string &key,
                            const CachedIndex &index) {
  const std::filesystem::path directory = archiveIndexCacheDirectory();
  if (directory.empty()) {
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return false;
  }
  const std::filesystem::path filePath = archiveIndexCacheFilePath(key);
  const auto serialize = [&](std::ostream &stream) {
    auto writeU64 = [&](std::uint64_t value) {
      stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    auto writeU8 = [&](std::uint8_t value) {
      stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    writeU8(2);  // format version
    writeU64(key.size());
    stream.write(key.data(), static_cast<std::streamsize>(key.size()));
    writeU64(static_cast<std::uint64_t>(index.size));
    writeU64(static_cast<std::uint64_t>(
        index.mtime.time_since_epoch().count()));
    writeU8(static_cast<std::uint8_t>(index.backend));
    writeU8(index.sevenZipFormat);
    writeU64(index.entries.size());
    for (const auto &entry : index.entries) {
      const std::string pathText = entry.path.generic_string();
      writeU64(pathText.size());
      stream.write(pathText.data(),
                   static_cast<std::streamsize>(pathText.size()));
      writeU8(entry.directory ? 1 : 0);
      writeU64(entry.size);
      writeU64(entry.order);
      writeU64(static_cast<std::uint64_t>(entry.offset));
      writeU8(entry.solid ? 1 : 0);
    }
    stream.flush();
    return stream.good();
  };
  // Write to a temporary sibling and rename atomically so a crash mid-write
  // never leaves a partial file that future reads would have to re-validate
  // and rebuild.
  std::filesystem::path tmpPath = filePath;
  tmpPath += ".tmp";
  {
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    if (!serialize(file)) {
      std::error_code removeError;
      std::filesystem::remove(tmpPath, removeError);
      return false;
    }
  }
  std::filesystem::rename(tmpPath, filePath, error);
  if (!error) {
    return true;
  }
  // Some filesystems cannot atomically replace an existing file; fall back
  // to a direct write so the cache still persists.
  std::ofstream direct(filePath, std::ios::binary | std::ios::trunc);
  if (!direct) {
    std::error_code removeError;
    std::filesystem::remove(tmpPath, removeError);
    return false;
  }
  const bool directOk = serialize(direct);
  // The tmp sibling is no longer needed; leave no orphaned .tmp files whether
  // or not the direct write succeeded.
  std::error_code removeError;
  std::filesystem::remove(tmpPath, removeError);
  return directOk;
}

std::shared_ptr<CachedIndex> readCachedIndexFromDisk(
    const std::string &key, std::uintmax_t size,
    std::filesystem::file_time_type mtime) {
  const std::filesystem::path directory = archiveIndexCacheDirectory();
  if (directory.empty()) {
    return nullptr;
  }
  const std::filesystem::path filePath = archiveIndexCacheFilePath(key);
  std::error_code sizeError;
  const std::uintmax_t fileBytes =
      std::filesystem::file_size(filePath, sizeError);
  if (sizeError || fileBytes == 0) {
    return nullptr;
  }
  std::ifstream file(filePath, std::ios::binary);
  if (!file) {
    return nullptr;
  }
  auto readU64 = [&]() -> std::uint64_t {
    std::uint64_t value = 0;
    file.read(reinterpret_cast<char *>(&value), sizeof(value));
    return value;
  };
  auto readU8 = [&]() -> std::uint8_t {
    std::uint8_t value = 0;
    file.read(reinterpret_cast<char *>(&value), sizeof(value));
    return value;
  };
  auto index = std::make_shared<CachedIndex>();
  const std::uint8_t version = readU8();
  const std::uint64_t storedKeyLen = readU64();
  if (!file.good() || version != 2 ||
      storedKeyLen > fileBytes ||
      storedKeyLen > (1024ull * 1024ull * 1024ull)) {
    return nullptr;
  }
  std::string storedKey(static_cast<std::size_t>(storedKeyLen), '\0');
  file.read(storedKey.data(), static_cast<std::streamsize>(storedKeyLen));
  if (!file.good() || storedKey != key) {
    return nullptr;
  }
  const std::uint64_t storedSize = readU64();
  const std::uint64_t storedMtime = readU64();
  if (!file.good() ||
      storedSize != static_cast<std::uint64_t>(size) ||
      storedMtime != static_cast<std::uint64_t>(mtime.time_since_epoch().count())) {
    return nullptr;
  }
  index->size = size;
  index->mtime = mtime;
  index->backend = static_cast<ArchiveIndexBackend>(readU8());
  index->sevenZipFormat = readU8();
  const std::uint64_t entryCount = readU64();
  // Each serialized entry needs at least 34 bytes beyond the variable-length
  // path, so a count much larger than the file can hold is malformed and
  // would otherwise allow an unbounded allocation from a corrupt file.
  if (!file.good() || entryCount > (fileBytes / 34ull)) {
    return nullptr;
  }
  index->entries.reserve(static_cast<std::size_t>(entryCount));
  for (std::uint64_t i = 0; i < entryCount; ++i) {
    const std::uint64_t pathLen = readU64();
    if (!file.good() || pathLen > fileBytes ||
        pathLen > (1024ull * 1024ull * 1024ull)) {
      return nullptr;
    }
    std::string pathText(static_cast<std::size_t>(pathLen), '\0');
    file.read(pathText.data(), static_cast<std::streamsize>(pathLen));
    Entry entry;
    entry.path = utf8_to_path_t(pathText);
    entry.directory = readU8() != 0;
    entry.size = readU64();
    entry.order = static_cast<std::size_t>(readU64());
    entry.offset = static_cast<std::int64_t>(readU64());
    entry.solid = readU8() != 0;
    if (!file.good()) {
      return nullptr;
    }
    index->entries.push_back(std::move(entry));
  }
  return index;
}

std::shared_ptr<const CachedIndex>
cachedIndexForArchiveIfFresh(const std::filesystem::path &archivePath) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  std::lock_guard<std::mutex> lock(gIndexMutex);
  const auto it = gIndexCache.find(key);
  if (it != gIndexCache.end() && it->second != nullptr &&
      it->second->size == size && it->second->mtime == mtime) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<const CachedIndex>
cachedIndexForArchive(const std::filesystem::path &archivePath,
                      std::string *errorMessage,
                      const PauseCallback &pauseCallback = nullptr) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  bool hadCachedIndex = false;
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    const auto it = gIndexCache.find(key);
    if (it != gIndexCache.end() && it->second != nullptr &&
        it->second->size == size && it->second->mtime == mtime) {
      return it->second;
    }
    hadCachedIndex = it != gIndexCache.end() && it->second != nullptr;
  }

  // Try to restore a previously persisted index from disk (cold start) before
  // rebuilding. Validated by size+mtime so offsets remain valid.
  if (!hadCachedIndex) {
    auto diskIndex = readCachedIndexFromDisk(key, size, mtime);
    if (diskIndex != nullptr) {
      buildIndexLookups(*diskIndex);
      appendDebugLogLineImpl("Loaded archive index from disk cache: " +
                             pathForLog(archivePath) + " entries=" +
                             std::to_string(diskIndex->entries.size()));
      std::lock_guard<std::mutex> cacheLock(gIndexMutex);
      gIndexCache[key] = diskIndex;
      return diskIndex;
    }
  }

  // Single-flight: if another thread is already building this archive's index,
  // wait for it and reuse the result instead of rebuilding. The first caller
  // to reach here becomes the builder.
  for (;;) {
    std::unique_lock<std::mutex> buildLock(gIndexBuildMutex);
    if (!gIndexBuildActive[key]) {
      gIndexBuildActive[key] = true;
      gIndexBuildDone[key] = false;
      gIndexBuildFailed[key] = false;
      break;
    }
    auto &inflightCv = gIndexBuildInFlight[key];
#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
    gSingleFlightWaiterCountForTesting.fetch_add(1, std::memory_order_relaxed);
#endif
    inflightCv.wait(buildLock, [&] { return !gIndexBuildActive[key]; });
    const bool builtOk = gIndexBuildDone[key];
    const bool builtFailed = gIndexBuildFailed[key];
    buildLock.unlock();
    std::lock_guard<std::mutex> cacheLock(gIndexMutex);
    const auto cacheIt = gIndexCache.find(key);
    if (builtOk && cacheIt != gIndexCache.end() &&
        cacheIt->second != nullptr && cacheIt->second->size == size &&
        cacheIt->second->mtime == mtime) {
      return cacheIt->second;
    }
    if (builtFailed) {
      // The in-flight build failed (corrupt archive, backend error, or a
      // pause abort). Report the failure to this caller instead of retrying,
      // so N concurrent waiters do not each run a full re-index back to
      // back. A later request can rebuild normally.
      if (errorMessage != nullptr) {
        *errorMessage = "Failed to index archive: " + pathForLog(archivePath);
      }
      return nullptr;
    }
    // Re-acquire the build lock and re-check whether another waiter has
    // claimed the builder role while this thread inspected the cache; if so,
    // loop back and wait on that build instead of racing into a duplicate
    // one. Reaching the fall-through otherwise means the in-flight result did
    // not match (e.g. a completed build that was evicted) rather than
    // returning a mismatched index.
    buildLock.lock();
    if (gIndexBuildActive[key]) {
      buildLock.unlock();
      continue;
    }
    gIndexBuildActive[key] = true;
    gIndexBuildDone[key] = false;
    gIndexBuildFailed[key] = false;
    break;
  }

  // Guard the entire builder body below: even if make_shared, buildIndexLookups,
  // or a backend throws, the in-flight flag is cleared and every waiter wakes
  // with a recorded failure instead of blocking forever.
  IndexBuildScope buildScope(key);

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ || ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP || \
    ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  appendDebugLogLineImpl((hadCachedIndex ? "Archive changed; rebuilding index: "
                                         : "Indexing archive: ") +
                         pathForLog(archivePath) + " (" +
                         byteCountForLog(size) + ")");
  auto loaded = std::make_shared<CachedIndex>();
  loaded->size = size;
  loaded->mtime = mtime;
  bool loadedEntries = false;
   if (!pauseIfNeeded(pauseCallback, errorMessage)) {
     buildScope.complete(false);
     return nullptr;
  }
#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      listZipEntries(archivePath, loaded->entries, &zipError,
                     pauseCallback)) {
    loaded->backend = ArchiveIndexBackend::MinizZip;
    loadedEntries = true;
  } else if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP index failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (!loadedEntries && hasRarArchiveExtension(archivePath)) {
    const RarSignature signature = rarSignature(archivePath);
    if (signature == RarSignature::Rar4) {
      std::vector<Entry> unarrEntries;
      bool containsSolidEntries = false;
      if (listUnarrRarEntries(archivePath, unarrEntries, containsSolidEntries,
                              &unarrError, pauseCallback)) {
        if (!containsSolidEntries) {
          loaded->entries = std::move(unarrEntries);
          loaded->backend = ArchiveIndexBackend::UnarrRar;
          loadedEntries = true;
        } else {
          appendDebugLogLineImpl(
              "RAR archive is solid; random-access unarr backend disabled: " +
              pathForLog(archivePath));
        }
      } else if (!unarrError.empty()) {
        appendDebugLogLineImpl("unarr RAR4 index failed: " +
                               pathForLog(archivePath) + ": " + unarrError);
      }
    } else if (signature == RarSignature::Rar5) {
      appendDebugLogLineImpl("RAR5 archive detected; using 7-Zip backend: " +
                             pathForLog(archivePath));
    } else {
      appendDebugLogLineImpl("unarr RAR index skipped: " +
                             pathForLog(archivePath) +
                             ": unknown RAR signature");
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (!loadedEntries && hasSevenZipArchiveExtension(archivePath) &&
      listSevenZipEntries(archivePath, loaded->entries,
                          loaded->sevenZipFormat, &sevenZipError,
                          pauseCallback)) {
    loaded->backend = ArchiveIndexBackend::SevenZip;
    loadedEntries = true;
  } else if (!loadedEntries && hasSevenZipArchiveExtension(archivePath) &&
             !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip index failed: " + pathForLog(archivePath) +
                           ": " + sevenZipError);
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipError;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string libarchiveError;
  if (!loadedEntries &&
      listEntriesUncached(archivePath, loaded->entries, &libarchiveError,
                          pauseCallback)) {
    loaded->backend = ArchiveIndexBackend::LibArchive;
    loadedEntries = true;
  } else if (!loadedEntries && !libarchiveError.empty()) {
    appendDebugLogLineImpl("libarchive index failed: " +
                           pathForLog(archivePath) + ": " + libarchiveError);
    if (errorMessage != nullptr) {
      *errorMessage = libarchiveError;
    }
  }
#endif
  if (!loadedEntries) {
    appendDebugLogLineImpl("Archive indexing failed: " +
                           pathForLog(archivePath));
    buildScope.complete(false);
    return nullptr;
  }
  const std::size_t skippedSystemEntries = filterSystemEntries(loaded->entries);
  if (skippedSystemEntries > 0) {
    appendDebugLogLineImpl("Skipped system archive entries: " +
                           pathForLog(archivePath) + " count=" +
                           std::to_string(skippedSystemEntries));
  }
  buildIndexLookups(*loaded);
  appendDebugLogLineImpl("Indexed archive with " + backendName(loaded->backend) +
                         ": " + pathForLog(archivePath) + " entries=" +
                         std::to_string(loaded->entries.size()));

  if (!writeCachedIndexToDisk(key, *loaded)) {
    appendDebugLogLineImpl("Failed to persist archive index to disk: " +
                           pathForLog(archivePath));
  }
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    gIndexCache[key] = loaded;
  }
  buildScope.complete(true);
  return loaded;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  buildScope.complete(false);
  return nullptr;
#endif
}

const Entry *findIndexedEntry(const CachedIndex &index,
                              const std::filesystem::path &innerPath) {
  const std::string target = normalizeEntryName(innerPath.generic_string());
  if (target.empty()) {
    return nullptr;
  }

  const auto exactIt = index.exact.find(target);
  if (exactIt != index.exact.end() && exactIt->second < index.entries.size()) {
    return &index.entries[exactIt->second];
  }

  const auto lowerIt = index.lower.find(lowerCopy(target));
  if (lowerIt != index.lower.end() && lowerIt->second < index.entries.size()) {
    return &index.entries[lowerIt->second];
  }

  return nullptr;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
struct CachedReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
};

bool readArchiveEntriesByCachedOrder(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }

  const auto index = cachedIndexForArchiveIfFresh(archivePath);
  if (index == nullptr) {
    return false;
  }

  std::vector<CachedReadTarget> targets;
  targets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    targets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
    });
  }
  if (targets.empty()) {
    return true;
  }

  std::sort(targets.begin(), targets.end(),
            [](const CachedReadTarget &a, const CachedReadTarget &b) {
              return a.order < b.order;
            });
  targets.erase(std::unique(targets.begin(), targets.end(),
                            [](const CachedReadTarget &a,
                               const CachedReadTarget &b) {
                              return a.order == b.order;
                            }),
                targets.end());

  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    return false;
  };

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  std::size_t targetIndex = 0;
  while (targetIndex < targets.size()) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      return fail("Could not read archive: " +
                  archiveErrorString(archiveHandle, ""));
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    const CachedReadTarget &target = targets[targetIndex];
    if (currentOrder < target.order) {
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (currentOrder > target.order) {
      return fail("Cached archive entry order did not match archive stream.");
    }
    if (!info.regular) {
      return fail("Cached archive entry is not a regular file.");
    }

    const std::string actual =
        normalizeEntryName(info.relativePath.generic_string());
    const std::string expected =
        normalizeEntryName(target.entryPath.generic_string());
    if (actual != expected) {
      return fail("Cached archive entry path did not match archive stream.");
    }

    FileData file;
    file.path = target.entryPath;
    const la_int64_t entrySize = archive_entry_size(entry);
    if (archive_entry_size_is_set(entry) && entrySize > 0) {
      reserveBufferedBytes(file.bytes,
                           static_cast<std::uintmax_t>(entrySize));
    }
    for (;;) {
      if (!pauseIfNeeded(pauseCallback, errorMessage)) {
        files.clear();
        return false;
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        return fail("Could not read archive entry: " +
                    archiveErrorString(archiveHandle, ""));
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    files.push_back(std::move(file));
    ++targetIndex;
  }

  return targetIndex == targets.size();
}

bool readArchiveEntriesByCachedOrderStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }

  const auto index = cachedIndexForArchiveIfFresh(archivePath);
  if (index == nullptr) {
    return false;
  }

  std::vector<CachedReadTarget> targets;
  targets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    targets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
    });
  }
  if (targets.empty()) {
    return true;
  }

  std::sort(targets.begin(), targets.end(),
            [](const CachedReadTarget &a, const CachedReadTarget &b) {
              return a.order < b.order;
            });
  targets.erase(std::unique(targets.begin(), targets.end(),
                            [](const CachedReadTarget &a,
                               const CachedReadTarget &b) {
                              return a.order == b.order;
                            }),
                targets.end());

  auto archiveStorage = openArchive(archivePath, errorMessage);
  if (archiveStorage == nullptr) {
    return false;
  }
  archive *archiveHandle = archiveStorage.get();

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    return false;
  };

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  std::size_t targetIndex = 0;
  while (targetIndex < targets.size()) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      return fail("Could not read archive: " +
                  archiveErrorString(archiveHandle, ""));
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    const CachedReadTarget &target = targets[targetIndex];
    if (currentOrder < target.order) {
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (currentOrder > target.order) {
      return fail("Cached archive entry order did not match archive stream.");
    }
    if (!info.regular) {
      return fail("Cached archive entry is not a regular file.");
    }

    const std::string actual =
        normalizeEntryName(info.relativePath.generic_string());
    const std::string expected =
        normalizeEntryName(target.entryPath.generic_string());
    if (actual != expected) {
      return fail("Cached archive entry path did not match archive stream.");
    }

    FileData file;
    file.path = target.entryPath;
    const la_int64_t entrySize = archive_entry_size(entry);
    if (archive_entry_size_is_set(entry) && entrySize > 0) {
      reserveBufferedBytes(file.bytes,
                           static_cast<std::uintmax_t>(entrySize));
    }
    for (;;) {
      if (!pauseIfNeeded(pauseCallback, errorMessage)) {
        return false;
      }
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        return fail("Could not read archive entry: " +
                    archiveErrorString(archiveHandle, ""));
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    if (!emitFileData(std::move(file), onFile, errorMessage)) {
      return false;
    }
    ++targetIndex;
  }

  return targetIndex == targets.size();
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
struct ZipReadTarget {
  std::string normalized;
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::uint64_t size = 0;
};

struct ZipDirectReadTarget {
  std::string normalized;
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::uint64_t size = 0;
  std::uint64_t compressedSize = 0;
  std::uint64_t dataOffset = 0;
  mz_uint16 method = 0;
  mz_uint32 crc32 = 0;
};

struct ZipDirectReadTiming {
  long long readMicros = 0;
  long long inflateMicros = 0;
  long long crcMicros = 0;
};

struct ZipDirectExtractionStats {
  std::size_t files = 0;
  long long acquireMicros = 0;
  long long readMicros = 0;
  long long inflateMicros = 0;
  long long crcMicros = 0;
  long long callbackMicros = 0;
};

constexpr std::uint32_t kZipLocalHeaderSignature = 0x04034b50u;
constexpr std::size_t kZipLocalHeaderSize = 30;
constexpr std::size_t kZipLocalHeaderFilenameLengthOffset = 26;
constexpr std::size_t kZipLocalHeaderExtraLengthOffset = 28;

long long elapsedMicrosSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void addZipDirectExtractionStats(ZipDirectExtractionStats &total,
                                 const ZipDirectExtractionStats &delta) {
  total.files += delta.files;
  total.acquireMicros += delta.acquireMicros;
  total.readMicros += delta.readMicros;
  total.inflateMicros += delta.inflateMicros;
  total.crcMicros += delta.crcMicros;
  total.callbackMicros += delta.callbackMicros;
}

std::uint16_t readZipLe16(const unsigned char *data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t readZipLe32(const unsigned char *data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::optional<std::string> minizFilename(mz_zip_archive *archive,
                                         mz_uint fileIndex) {
  const mz_uint size = mz_zip_reader_get_filename(archive, fileIndex, nullptr, 0);
  if (size == 0) {
    return std::nullopt;
  }
  std::string filename(size, '\0');
  if (mz_zip_reader_get_filename(archive, fileIndex, filename.data(), size) ==
      0) {
    return std::nullopt;
  }
  if (!filename.empty() && filename.back() == '\0') {
    filename.pop_back();
  }
  return filename;
}

enum class ZipNameMatch { Matches, Mismatches, Unknown };

std::optional<std::string> normalizedZipEntryName(const std::string &filename,
                                                  bool *knownMismatch) {
  if (knownMismatch != nullptr) {
    *knownMismatch = false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE && ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
  if (!isValidUtf8(filename)) {
    std::filesystem::path convertedPath;
    bool convertedAny = false;
    for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
      const auto converted = convertTextToUtf8(filename, encoding);
      if (!converted.has_value()) {
        continue;
      }
      convertedAny = true;
      if (safeEntryPath(*converted, convertedPath)) {
        return convertedPath.generic_string();
      }
    }
    if (knownMismatch != nullptr) {
      *knownMismatch = convertedAny;
    }
    return std::nullopt;
  }
#elif ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  if (!isValidUtf8(filename)) {
    return std::nullopt;
  }
#endif

  std::filesystem::path minizPath;
  if (safeEntryPath(filename, minizPath)) {
    return minizPath.generic_string();
  }

  if (knownMismatch != nullptr) {
    *knownMismatch = true;
  }
  return std::nullopt;
}

ZipNameMatch compareZipEntryName(const std::string &filename,
                                 const std::string &target) {
  bool knownMismatch = false;
  const auto normalized = normalizedZipEntryName(filename, &knownMismatch);
  if (normalized.has_value() && *normalized == target) {
    return ZipNameMatch::Matches;
  }
  return knownMismatch ? ZipNameMatch::Mismatches : ZipNameMatch::Unknown;
}

bool zipCompressedDataOffset(RandomAccessFile &file,
                             const mz_zip_archive_file_stat &stat,
                             std::uint64_t archiveSize,
                             std::uint64_t &dataOffset,
                             std::string *errorMessage) {
  std::array<unsigned char, kZipLocalHeaderSize> header{};
  if (!file.readAt(stat.m_local_header_ofs, header.data(), header.size(),
                   errorMessage)) {
    return false;
  }
  if (readZipLe32(header.data()) != kZipLocalHeaderSignature) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP local header signature did not match.";
    }
    return false;
  }

  const std::uint64_t filenameLength =
      readZipLe16(header.data() + kZipLocalHeaderFilenameLengthOffset);
  const std::uint64_t extraLength =
      readZipLe16(header.data() + kZipLocalHeaderExtraLengthOffset);
  dataOffset = stat.m_local_header_ofs + kZipLocalHeaderSize + filenameLength +
               extraLength;
  if (dataOffset < stat.m_local_header_ofs || dataOffset > archiveSize ||
      stat.m_comp_size > archiveSize - dataOffset) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP local header data range is out of bounds.";
    }
    return false;
  }
  return true;
}

bool prepareZipDirectReadTarget(mz_zip_archive *archive,
                                RandomAccessFile &archiveFile,
                                const ZipReadTarget &target,
                                std::uint64_t archiveSize,
                                ZipDirectReadTarget &directTarget,
                                std::string *errorMessage,
                                const PauseCallback &pauseCallback) {
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (target.order > std::numeric_limits<mz_uint>::max()) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP index is out of range.";
    }
    return false;
  }
  const auto fileIndex = static_cast<mz_uint>(target.order);
  const mz_uint fileCount = mz_zip_reader_get_num_files(archive);
  if (fileIndex >= fileCount) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP index is out of range.";
    }
    return false;
  }

  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(archive, fileIndex, &stat)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory entry.";
    }
    return false;
  }
  if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is not supported by direct reader.";
    }
    return false;
  }
  if (stat.m_method != 0 && stat.m_method != MZ_DEFLATED) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry compression method is not supported.";
    }
    return false;
  }
  if (stat.m_uncomp_size != target.size) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP central directory size did not match archive index.";
    }
    return false;
  }
  if (stat.m_uncomp_size >
          static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max()) ||
      stat.m_comp_size >
          static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is too large to read into memory.";
    }
    return false;
  }
  const auto filename = minizFilename(archive, fileIndex);
  if (!filename.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory filename.";
    }
    return false;
  }
  if (compareZipEntryName(*filename, target.normalized) ==
      ZipNameMatch::Mismatches) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP central directory order did not match archive index.";
    }
    return false;
  }

  std::uint64_t dataOffset = 0;
  if (!zipCompressedDataOffset(archiveFile, stat, archiveSize, dataOffset,
                               errorMessage)) {
    return false;
  }

  directTarget = {
      .normalized = target.normalized,
      .entryPath = target.entryPath,
      .order = target.order,
      .size = stat.m_uncomp_size,
      .compressedSize = stat.m_comp_size,
      .dataOffset = dataOffset,
      .method = stat.m_method,
      .crc32 = stat.m_crc32,
  };
  return true;
}

bool readZipDirectTarget(RandomAccessFile &archiveFile,
                         const ZipDirectReadTarget &target, FileData &file,
                         std::vector<unsigned char> &compressedScratch,
                         std::string *errorMessage,
                         ZipDirectReadTiming *timing,
                         const PauseCallback &pauseCallback) {
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (target.size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      target.compressedSize >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is too large to read into memory.";
    }
    return false;
  }

  file.path = target.entryPath;
  file.bytes.resize(static_cast<std::size_t>(target.size));
  if (target.size == 0 && target.compressedSize == 0) {
    if (target.crc32 != MZ_CRC32_INIT) {
      if (errorMessage != nullptr) {
        *errorMessage = "ZIP CRC check failed.";
      }
      return false;
    }
    return true;
  }

  if (target.method == 0) {
    if (target.compressedSize != target.size) {
      if (errorMessage != nullptr) {
        *errorMessage = "Stored ZIP entry size mismatch.";
      }
      return false;
    }
    const auto readStart = std::chrono::steady_clock::now();
    if (!archiveFile.readAt(target.dataOffset, file.bytes.data(),
                            file.bytes.size(), errorMessage)) {
      return false;
    }
    if (timing != nullptr) {
      timing->readMicros += elapsedMicrosSince(readStart);
    }
  } else if (target.method == MZ_DEFLATED) {
    compressedScratch.resize(static_cast<std::size_t>(target.compressedSize));
    const auto readStart = std::chrono::steady_clock::now();
    if (!archiveFile.readAt(target.dataOffset, compressedScratch.data(),
                            compressedScratch.size(), errorMessage)) {
      return false;
    }
    if (timing != nullptr) {
      timing->readMicros += elapsedMicrosSince(readStart);
    }
    const auto inflateStart = std::chrono::steady_clock::now();
    const size_t decompressed = tinfl_decompress_mem_to_mem(
        file.bytes.data(), file.bytes.size(), compressedScratch.data(),
        compressedScratch.size(), 0);
    if (timing != nullptr) {
      timing->inflateMicros += elapsedMicrosSince(inflateStart);
    }
    if (decompressed == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ||
        decompressed != file.bytes.size()) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not inflate ZIP entry by direct reader.";
      }
      return false;
    }
  } else {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry compression method is not supported.";
    }
    return false;
  }

  const auto crcStart = std::chrono::steady_clock::now();
  const mz_ulong crc =
      mz_crc32(MZ_CRC32_INIT, file.bytes.data(), file.bytes.size());
  if (timing != nullptr) {
    timing->crcMicros += elapsedMicrosSince(crcStart);
  }
  if (crc != target.crc32) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP CRC check failed.";
    }
    return false;
  }
  return true;
}

bool listZipEntries(const std::filesystem::path &archivePath,
                    std::vector<Entry> &entries, std::string *errorMessage,
                    const PauseCallback &pauseCallback) {
  entries.clear();
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    entries.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  entries.reserve(fileCount);
  for (mz_uint fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
    if (fileIndex > 0 && fileIndex % kZipIndexPauseCheckInterval == 0 &&
        !pauseIfNeeded(pauseCallback, errorMessage)) {
      return fail("Operation cancelled");
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }

    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    const auto normalized = normalizedZipEntryName(*filename, nullptr);
    if (!normalized.has_value() || normalized->empty()) {
      continue;
    }

    entries.push_back({
        .path = std::filesystem::path(*normalized),
        .directory = stat.m_is_directory != 0,
        .size = stat.m_uncomp_size,
        .order = static_cast<std::size_t>(fileIndex),
    });
  }
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return fail("Operation cancelled");
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntryByFileIndex(mz_zip_archive *archive, mz_uint fileIndex,
                             const std::filesystem::path &entryPath,
                             FileData &file, std::string *errorMessage,
                             const PauseCallback &pauseCallback,
                             std::vector<unsigned char> *readScratch = nullptr) {
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(archive, fileIndex, &stat)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory entry.";
    }
    return false;
  }
  if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is not supported by direct reader.";
    }
    return false;
  }
  if (stat.m_uncomp_size >
      static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is too large to read into memory.";
    }
    return false;
  }

  file.path = entryPath;
  file.bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  bool extracted = false;
  if (readScratch != nullptr) {
    if (readScratch->empty()) {
      readScratch->resize(MZ_ZIP_MAX_IO_BUF_SIZE);
    }
    extracted = mz_zip_reader_extract_to_mem_no_alloc(
        archive, fileIndex, file.bytes.data(), file.bytes.size(), 0,
        readScratch->data(), readScratch->size());
  } else {
    extracted = mz_zip_reader_extract_to_mem(
        archive, fileIndex, file.bytes.data(), file.bytes.size(), 0);
  }
  if (!extracted) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not extract ZIP entry by index.";
    }
    return false;
  }
  return true;
}

bool readZipTargetByFileIndex(mz_zip_archive *archive,
                              const ZipReadTarget &target, FileData &file,
                              std::string *errorMessage,
                              const PauseCallback &pauseCallback,
                              std::vector<unsigned char> *readScratch = nullptr) {
  if (target.order > std::numeric_limits<mz_uint>::max()) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP index is out of range.";
    }
    return false;
  }
  const auto fileIndex = static_cast<mz_uint>(target.order);
  const mz_uint fileCount = mz_zip_reader_get_num_files(archive);
  if (fileIndex >= fileCount) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP index is out of range.";
    }
    return false;
  }

  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(archive, fileIndex, &stat)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory entry.";
    }
    return false;
  }
  if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is not supported by direct reader.";
    }
    return false;
  }
  if (stat.m_uncomp_size != target.size) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP central directory size did not match archive index.";
    }
    return false;
  }
  const auto filename = minizFilename(archive, fileIndex);
  if (!filename.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory filename.";
    }
    return false;
  }
  if (compareZipEntryName(*filename, target.normalized) ==
      ZipNameMatch::Mismatches) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP central directory order did not match archive index.";
    }
    return false;
  }
  return readZipEntryByFileIndex(archive, fileIndex, target.entryPath, file,
                                 errorMessage, pauseCallback, readScratch);
}

bool readZipEntriesByName(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    std::vector<FileData> &files, std::string *errorMessage,
    const PauseCallback &pauseCallback = nullptr) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> exactTargets;
  std::unordered_map<std::string, std::filesystem::path> lowerTargets;
  exactTargets.reserve(innerPaths.size());
  lowerTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (target.empty()) {
      continue;
    }
    exactTargets.emplace(target, innerPath);
    lowerTargets.emplace(lowerCopy(target), innerPath);
  }
  if (exactTargets.empty()) {
    return true;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(exactTargets.size());
  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  for (mz_uint fileIndex = 0; fileIndex < fileCount && !exactTargets.empty();
       ++fileIndex) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return fail("Operation cancelled");
    }
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      continue;
    }

    const auto normalized = normalizedZipEntryName(*filename, nullptr);
    if (!normalized.has_value()) {
      continue;
    }

    auto targetIt = exactTargets.find(*normalized);
    if (targetIt == exactTargets.end()) {
      const auto lowerIt = lowerTargets.find(lowerCopy(*normalized));
      if (lowerIt == lowerTargets.end()) {
        continue;
      }
      targetIt = exactTargets.find(normalizeEntryName(
          lowerIt->second.generic_string()));
      if (targetIt == exactTargets.end()) {
        continue;
      }
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
      return fail("ZIP entry is not supported by direct reader.");
    }
    if (stat.m_uncomp_size >
        static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
      return fail("ZIP entry is too large to read into memory.");
    }

    readTargets.push_back({
        .normalized = *normalized,
        .entryPath = targetIt->second,
        .order = static_cast<std::size_t>(fileIndex),
        .size = static_cast<std::uint64_t>(stat.m_uncomp_size),
    });
    lowerTargets.erase(lowerCopy(targetIt->first));
    exactTargets.erase(targetIt);
  }

  for (const ZipReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return fail("Operation cancelled");
    }
    FileData file;
    std::string readError;
    if (!readZipEntryByFileIndex(&archive, static_cast<mz_uint>(target.order),
                                 target.entryPath, file, &readError,
                                 pauseCallback)) {
      return fail(readError.empty() ? "Could not extract ZIP entry by name."
                                    : readError);
    }
    files.push_back(std::move(file));
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntryBounded(const std::filesystem::path &archivePath,
                         const Entry &entry, std::vector<unsigned char> &bytes,
                         std::size_t maximumBytes, std::string *errorMessage,
                         const PauseCallback &pauseCallback, bool *oversize) {
  *oversize = false;
  bytes.clear();
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message, bool wasOversize = false) {
    if (wasOversize) {
      *oversize = true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    bytes.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  if (entry.order > std::numeric_limits<mz_uint>::max()) {
    return fail("ZIP index is out of range.");
  }
  const mz_uint fileIndex = static_cast<mz_uint>(entry.order);
  if (fileIndex >= mz_zip_reader_get_num_files(&archive)) {
    return fail("ZIP index is out of range.");
  }
  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
    return fail("Could not read ZIP central directory entry.");
  }
  if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
    return fail("ZIP entry is not supported by direct reader.");
  }
  if (stat.m_uncomp_size != entry.size) {
    return fail("ZIP central directory size did not match archive index.");
  }
  const auto filename = minizFilename(&archive, fileIndex);
  if (!filename.has_value()) {
    return fail("Could not read ZIP central directory filename.");
  }
  if (compareZipEntryName(*filename,
                          normalizeEntryName(entry.path.generic_string())) ==
      ZipNameMatch::Mismatches) {
    return fail("ZIP central directory order did not match archive index.");
  }
  if (stat.m_uncomp_size >
      static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
    return fail("ZIP entry is too large to read into memory.");
  }

  // Bounded streaming extraction: decompression is pulled in small chunks and
  // capped at maximumBytes, so a lying central directory cannot force a full
  // oversized allocation before the cap is enforced.
  bytes.reserve(static_cast<std::size_t>(stat.m_uncomp_size));
  mz_zip_reader_extract_iter_state *iterator =
      mz_zip_reader_extract_iter_new(&archive, fileIndex, 0);
  if (iterator == nullptr) {
    return fail("Could not start streaming ZIP extraction.");
  }
  std::array<unsigned char, 64 * 1024> chunk{};
  for (;;) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      mz_zip_reader_extract_iter_free(iterator);
      return fail("Operation cancelled");
    }
    const size_t produced =
        mz_zip_reader_extract_iter_read(iterator, chunk.data(), chunk.size());
    if (produced == 0) {
      break;
    }
    if (produced > maximumBytes - bytes.size()) {
      mz_zip_reader_extract_iter_free(iterator);
      return fail("Archive entry exceeds bounded read limit: " +
                      entry.path.generic_string(),
                  true);
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + produced);
  }
  mz_zip_reader_extract_iter_free(iterator);
  if (bytes.size() != static_cast<std::size_t>(stat.m_uncomp_size)) {
    return fail("ZIP entry extraction did not produce the declared size.");
  }
  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntriesByIndex(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr) {
    return false;
  }

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(targets.size());
  for (const auto &targetPair : targets) {
    const std::filesystem::path &requestedPath = targetPair.second;
    const Entry *entry = findIndexedEntry(*index, requestedPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }

    readTargets.push_back({
        .normalized = normalizeEntryName(entry->path.generic_string()),
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }
  std::sort(readTargets.begin(), readTargets.end(),
            [](const ZipReadTarget &a, const ZipReadTarget &b) {
              return a.order < b.order;
            });

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  for (const ZipReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return fail("Operation cancelled");
    }
    if (target.order >= static_cast<std::size_t>(fileCount)) {
      return fail("ZIP index is out of range.");
    }

    const mz_uint fileIndex = static_cast<mz_uint>(target.order);
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
      return fail("ZIP entry is not supported by direct reader.");
    }
    if (stat.m_uncomp_size != target.size) {
      return fail("ZIP central directory size did not match archive index.");
    }
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    if (compareZipEntryName(*filename, target.normalized) ==
        ZipNameMatch::Mismatches) {
      return fail("ZIP central directory order did not match archive index.");
    }
    if (stat.m_uncomp_size >
        static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
      return fail("ZIP entry is too large to read into memory.");
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&archive, fileIndex, file.bytes.data(),
                                      file.bytes.size(), 0)) {
      return fail("Could not extract ZIP entry by index.");
    }
    files.push_back(std::move(file));
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntriesByIndexStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr) {
    return false;
  }

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(targets.size());
  for (const auto &targetPair : targets) {
    const std::filesystem::path &requestedPath = targetPair.second;
    const Entry *entry = findIndexedEntry(*index, requestedPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }

    readTargets.push_back({
        .normalized = normalizeEntryName(entry->path.generic_string()),
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }
  std::sort(readTargets.begin(), readTargets.end(),
            [](const ZipReadTarget &a, const ZipReadTarget &b) {
              return a.order < b.order;
            });

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    mz_zip_reader_end(&archive);
    return false;
  };

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  for (const ZipReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return fail("Operation cancelled");
    }
    if (target.order >= static_cast<std::size_t>(fileCount)) {
      return fail("ZIP index is out of range.");
    }

    const mz_uint fileIndex = static_cast<mz_uint>(target.order);
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
      return fail("ZIP entry is not supported by direct reader.");
    }
    if (stat.m_uncomp_size != target.size) {
      return fail("ZIP central directory size did not match archive index.");
    }
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    if (compareZipEntryName(*filename, target.normalized) ==
        ZipNameMatch::Mismatches) {
      return fail("ZIP central directory order did not match archive index.");
    }
    if (stat.m_uncomp_size >
        static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
      return fail("ZIP entry is too large to read into memory.");
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&archive, fileIndex, file.bytes.data(),
                                      file.bytes.size(), 0)) {
      return fail("Could not extract ZIP entry by index.");
    }
#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
    notifyStreamingEntryObserverForTesting(archivePath, target.entryPath);
#endif
    if (!emitFileData(std::move(file), onFile, errorMessage)) {
      mz_zip_reader_end(&archive);
      return false;
    }
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntriesByIndexConcurrent(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::size_t maxWorkers, std::uint64_t maxInFlightBytes,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }
  if (!onFile) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr) {
    return false;
  }
  if (index->backend != ArchiveIndexBackend::MinizZip) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive is not a random-access ZIP archive.";
    }
    return false;
  }

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(targets.size());
  for (const auto &targetPair : targets) {
    const std::filesystem::path &requestedPath = targetPair.second;
    const Entry *entry = findIndexedEntry(*index, requestedPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }

    readTargets.push_back({
        .normalized = normalizeEntryName(entry->path.generic_string()),
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }
  std::sort(readTargets.begin(), readTargets.end(),
            [](const ZipReadTarget &a, const ZipReadTarget &b) {
              return a.order < b.order;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const ZipReadTarget &a,
                                   const ZipReadTarget &b) {
                                  return a.order == b.order;
                                }),
                    readTargets.end());

  using Clock = std::chrono::steady_clock;
  const auto prepareStart = Clock::now();
  std::uint64_t archiveSize = 0;
  if (index->size >
      static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP archive is too large for direct reader.";
    }
    return false;
  }
  archiveSize = static_cast<std::uint64_t>(index->size);

  RandomAccessFile headerFile;
  if (!headerFile.open(archivePath, errorMessage)) {
    return false;
  }
  mz_zip_archive prepareArchive{};
  mz_zip_zero_struct(&prepareArchive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file_v2(&prepareArchive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }
  auto failPrepare = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    mz_zip_reader_end(&prepareArchive);
    return false;
  };

  std::vector<ZipDirectReadTarget> directTargets;
  directTargets.reserve(readTargets.size());
  for (const ZipReadTarget &target : readTargets) {
    ZipDirectReadTarget directTarget;
    std::string prepareError;
    if (!prepareZipDirectReadTarget(&prepareArchive, headerFile, target,
                                    archiveSize, directTarget, &prepareError,
                                    pauseCallback)) {
      return failPrepare(prepareError.empty()
                             ? "Could not prepare ZIP direct read target."
                             : prepareError);
    }
    directTargets.push_back(std::move(directTarget));
  }
  mz_zip_reader_end(&prepareArchive);

  if (directTargets.empty()) {
    return true;
  }

  auto addSaturated = [](std::uint64_t &total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
      total = std::numeric_limits<std::uint64_t>::max();
    } else {
      total += value;
    }
  };

  std::uint64_t totalCompressedBytes = 0;
  std::uint64_t totalUncompressedBytes = 0;
  std::uint64_t minDataOffset = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t maxDataEnd = 0;
  std::size_t storedTargets = 0;
  std::size_t deflatedTargets = 0;
  for (const ZipDirectReadTarget &target : directTargets) {
    addSaturated(totalCompressedBytes, target.compressedSize);
    addSaturated(totalUncompressedBytes, target.size);
    minDataOffset = std::min(minDataOffset, target.dataOffset);
    const std::uint64_t dataEnd =
        target.compressedSize >
                std::numeric_limits<std::uint64_t>::max() - target.dataOffset
            ? std::numeric_limits<std::uint64_t>::max()
            : target.dataOffset + target.compressedSize;
    maxDataEnd = std::max(maxDataEnd, dataEnd);
    if (target.method == 0) {
      ++storedTargets;
    } else if (target.method == MZ_DEFLATED) {
      ++deflatedTargets;
    }
  }
  const std::uint64_t targetDataSpan =
      maxDataEnd > minDataOffset ? maxDataEnd - minDataOffset : 0;

  maxWorkers =
      std::max<std::size_t>(1, std::min(maxWorkers, directTargets.size()));
  if (maxInFlightBytes == 0) {
    maxInFlightBytes = std::numeric_limits<std::uint64_t>::max();
  }

  std::mutex stateMutex;
  std::condition_variable spaceCv;
  std::size_t nextTarget = 0;
  std::size_t emittedFiles = 0;
  std::uint64_t inFlightBytes = 0;
  bool failed = false;
  std::string failureMessage;
  ZipDirectExtractionStats aggregateStats;

  auto setFailure = [&](std::string message) {
    {
      std::lock_guard lock(stateMutex);
      if (!failed) {
        failed = true;
        failureMessage = std::move(message);
      }
    }
    spaceCv.notify_all();
  };

  auto acquireBytes = [&](std::uint64_t bytes) {
    std::unique_lock lock(stateMutex);
    for (;;) {
      if (failed) {
        return false;
      }
      if (inFlightBytes == 0 || inFlightBytes + bytes <= maxInFlightBytes) {
        inFlightBytes += bytes;
        return true;
      }
      lock.unlock();
      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        return false;
      }
      lock.lock();
      spaceCv.wait_for(lock, std::chrono::milliseconds(20));
    }
  };

  auto releaseBytes = [&](std::uint64_t bytes) {
    {
      std::lock_guard lock(stateMutex);
      inFlightBytes = bytes > inFlightBytes ? 0 : inFlightBytes - bytes;
    }
    spaceCv.notify_all();
  };

  auto worker = [&]() {
    ZipDirectExtractionStats localStats;
    RandomAccessFile archiveFile;
    std::string openError;
    if (!archiveFile.open(archivePath, &openError)) {
      setFailure(openError.empty()
                     ? "Could not open ZIP archive for direct reading."
                     : openError);
      return;
    }
    std::vector<unsigned char> compressedScratch;

    for (;;) {
      ZipDirectReadTarget target;
      {
        std::lock_guard lock(stateMutex);
        if (failed || nextTarget >= directTargets.size()) {
          break;
        }
        target = directTargets[nextTarget++];
      }

      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        break;
      }

      const std::uint64_t targetBytes =
          target.size > std::numeric_limits<std::uint64_t>::max() -
                            target.compressedSize
              ? std::numeric_limits<std::uint64_t>::max()
              : target.size + target.compressedSize;
      const auto acquireStart = std::chrono::steady_clock::now();
      if (!acquireBytes(targetBytes)) {
        break;
      }
      localStats.acquireMicros += elapsedMicrosSince(acquireStart);

      FileData file;
      std::string readError;
      ZipDirectReadTiming timing;
      bool ok = readZipDirectTarget(archiveFile, target, file,
                                    compressedScratch, &readError, &timing,
                                    pauseCallback);
      localStats.readMicros += timing.readMicros;
      localStats.inflateMicros += timing.inflateMicros;
      localStats.crcMicros += timing.crcMicros;
      if (ok) {
        {
          std::lock_guard lock(stateMutex);
          ok = !failed;
        }
      }
      if (ok) {
        const auto callbackStart = std::chrono::steady_clock::now();
        ok = emitFileData(std::move(file), onFile, &readError);
        localStats.callbackMicros += elapsedMicrosSince(callbackStart);
      }
      releaseBytes(targetBytes);

      if (!ok) {
        setFailure(readError.empty()
                       ? "Could not extract ZIP entry by index."
                       : readError);
        break;
      }
      {
        std::lock_guard lock(stateMutex);
        ++emittedFiles;
      }
      ++localStats.files;
    }

    {
      std::lock_guard lock(stateMutex);
      addZipDirectExtractionStats(aggregateStats, localStats);
    }
  };

  const auto start = Clock::now();
  std::vector<std::thread> workers;
  workers.reserve(maxWorkers);
  for (std::size_t i = 0; i < maxWorkers; ++i) {
    workers.emplace_back(worker);
  }
  for (auto &thread : workers) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (failed) {
    if (errorMessage != nullptr) {
      *errorMessage = failureMessage.empty()
                          ? "Parallel ZIP extraction failed."
                          : failureMessage;
    }
    return false;
  }

  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count();
  const auto prepareMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(start -
                                                            prepareStart)
          .count();
  appendDebugLogLineImpl("Finished concurrent direct miniz ZIP extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(directTargets.size()) +
                         " files=" + std::to_string(emittedFiles) +
                         " workers=" + std::to_string(maxWorkers) +
                         " stored=" + std::to_string(storedTargets) +
                         " deflated=" + std::to_string(deflatedTargets) +
                         " compressedBytes=" +
                         std::to_string(totalCompressedBytes) +
                         " uncompressedBytes=" +
                         std::to_string(totalUncompressedBytes) +
                         " targetDataSpan=" +
                         std::to_string(targetDataSpan) +
                         " maxInFlightBytes=" +
                         std::to_string(maxInFlightBytes) +
                         " prepareMs=" + std::to_string(prepareMs) +
                         " extractMs=" + std::to_string(extractMs) +
                         " readMs=" +
                         std::to_string(aggregateStats.readMicros / 1000) +
                         " inflateMs=" +
                         std::to_string(aggregateStats.inflateMicros / 1000) +
                         " crcMs=" +
                         std::to_string(aggregateStats.crcMicros / 1000) +
                         " callbackMs=" +
                         std::to_string(aggregateStats.callbackMicros / 1000) +
                         " acquireMs=" +
                         std::to_string(aggregateStats.acquireMicros / 1000));
  return true;
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
struct UnarrRarReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::int64_t offset = -1;
  std::uint64_t size = 0;
};

bool readUnarrRarEntriesByOffset(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::UnarrRar) {
    return false;
  }

  std::vector<UnarrRarReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory || entry->offset < 0) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    if (entry->solid) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "RAR entry is solid; random-access extraction is impossible.";
      }
      files.clear();
      return false;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .offset = entry->offset,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const UnarrRarReadTarget &a, const UnarrRarReadTarget &b) {
              return a.offset < b.offset;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const UnarrRarReadTarget &a,
                                   const UnarrRarReadTarget &b) {
                                  return a.offset == b.offset;
                                }),
                    readTargets.end());

  UnarrStreamHandle stream;
  UnarrArchiveHandle archive;
  if (!openUnarrRarArchive(archivePath, stream, archive, errorMessage)) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  const auto readStart = Clock::now();
  appendDebugLogLineImpl("Starting unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " firstOffset=" +
                         std::to_string(readTargets.front().offset) +
                         " lastOffset=" +
                         std::to_string(readTargets.back().offset));

  files.reserve(readTargets.size());
  for (const UnarrRarReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    if (!ar_parse_entry_at(archive.get(),
                           static_cast<off64_t>(target.offset))) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not seek to RAR entry offset.";
      }
      files.clear();
      return false;
    }

    const char *actualName = ar_entry_get_name(archive.get());
    std::filesystem::path actualPath;
    if (actualName == nullptr || !safeEntryPath(actualName, actualPath) ||
        normalizeEntryName(actualPath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR offset did not match cached entry path.";
      }
      files.clear();
      return false;
    }

    const size_t entrySize = ar_entry_get_size(archive.get());
    if (static_cast<std::uint64_t>(entrySize) != target.size) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR entry size did not match cached index.";
      }
      files.clear();
      return false;
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(entrySize);
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    if (entrySize > 0 &&
        !ar_entry_uncompress(archive.get(), file.bytes.data(), entrySize)) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not extract RAR entry.";
      }
      files.clear();
      return false;
    }
    files.push_back(std::move(file));
  }

  const auto readMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            readStart)
          .count();
  appendDebugLogLineImpl("Finished unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " files=" + std::to_string(files.size()) +
                         " extractMs=" + std::to_string(readMs));
  return true;
}

bool readUnarrRarEntriesByOffsetStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::UnarrRar) {
    return false;
  }

  std::vector<UnarrRarReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory || entry->offset < 0) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    if (entry->solid) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "RAR entry is solid; random-access extraction is impossible.";
      }
      return false;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .offset = entry->offset,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const UnarrRarReadTarget &a, const UnarrRarReadTarget &b) {
              return a.offset < b.offset;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const UnarrRarReadTarget &a,
                                   const UnarrRarReadTarget &b) {
                                  return a.offset == b.offset;
                                }),
                    readTargets.end());

  UnarrStreamHandle stream;
  UnarrArchiveHandle archive;
  if (!openUnarrRarArchive(archivePath, stream, archive, errorMessage)) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  const auto readStart = Clock::now();
  appendDebugLogLineImpl("Starting streaming unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " firstOffset=" +
                         std::to_string(readTargets.front().offset) +
                         " lastOffset=" +
                         std::to_string(readTargets.back().offset));

  std::size_t emittedFiles = 0;
  for (const UnarrRarReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    if (!ar_parse_entry_at(archive.get(),
                           static_cast<off64_t>(target.offset))) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not seek to RAR entry offset.";
      }
      return false;
    }

    const char *actualName = ar_entry_get_name(archive.get());
    std::filesystem::path actualPath;
    if (actualName == nullptr || !safeEntryPath(actualName, actualPath) ||
        normalizeEntryName(actualPath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR offset did not match cached entry path.";
      }
      return false;
    }

    const size_t entrySize = ar_entry_get_size(archive.get());
    if (static_cast<std::uint64_t>(entrySize) != target.size) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR entry size did not match cached index.";
      }
      return false;
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(entrySize);
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    if (entrySize > 0 &&
        !ar_entry_uncompress(archive.get(), file.bytes.data(), entrySize)) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not extract RAR entry.";
      }
      return false;
    }
    if (!emitFileData(std::move(file), onFile, errorMessage)) {
      return false;
    }
    ++emittedFiles;
  }

  const auto readMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            readStart)
          .count();
  appendDebugLogLineImpl("Finished streaming unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " files=" + std::to_string(emittedFiles) +
                         " extractMs=" + std::to_string(readMs));
  return true;
}

bool readUnarrRarTargetByOffset(ar_archive *archive,
                                const UnarrRarReadTarget &target,
                                FileData &file, std::string *errorMessage,
                                const PauseCallback &pauseCallback) {
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (!ar_parse_entry_at(archive, static_cast<off64_t>(target.offset))) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not seek to RAR entry offset.";
    }
    return false;
  }

  const char *actualName = ar_entry_get_name(archive);
  std::filesystem::path actualPath;
  if (actualName == nullptr || !safeEntryPath(actualName, actualPath) ||
      normalizeEntryName(actualPath.generic_string()) !=
          normalizeEntryName(target.entryPath.generic_string())) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR offset did not match cached entry path.";
    }
    return false;
  }

  const size_t entrySize = ar_entry_get_size(archive);
  if (static_cast<std::uint64_t>(entrySize) != target.size) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR entry size did not match cached index.";
    }
    return false;
  }

  file.path = target.entryPath;
  file.bytes.resize(entrySize);
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (entrySize > 0 &&
      !ar_entry_uncompress(archive, file.bytes.data(), entrySize)) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not extract RAR entry.";
    }
    return false;
  }
  return true;
}

bool readUnarrRarEntriesByOffsetConcurrent(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::size_t maxWorkers, std::uint64_t maxInFlightBytes,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }
  if (!onFile) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::UnarrRar) {
    return false;
  }

  std::vector<UnarrRarReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory || entry->offset < 0) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    if (entry->solid) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "RAR entry is solid; random-access extraction is impossible.";
      }
      return false;
    }
    if (entry->size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      if (errorMessage != nullptr) {
        *errorMessage = "RAR entry is too large to read into memory.";
      }
      return false;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .offset = entry->offset,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const UnarrRarReadTarget &a, const UnarrRarReadTarget &b) {
              return a.offset < b.offset;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const UnarrRarReadTarget &a,
                                   const UnarrRarReadTarget &b) {
                                  return a.offset == b.offset;
                                }),
                    readTargets.end());

  maxWorkers =
      std::max<std::size_t>(1, std::min(maxWorkers, readTargets.size()));
  if (maxInFlightBytes == 0) {
    maxInFlightBytes = std::numeric_limits<std::uint64_t>::max();
  }

  std::mutex stateMutex;
  std::condition_variable spaceCv;
  std::size_t nextTarget = 0;
  std::size_t emittedFiles = 0;
  std::uint64_t inFlightBytes = 0;
  bool failed = false;
  std::string failureMessage;
  long long acquireMicros = 0;
  long long extractMicros = 0;
  long long callbackMicros = 0;

  auto setFailure = [&](std::string message) {
    {
      std::lock_guard lock(stateMutex);
      if (!failed) {
        failed = true;
        failureMessage = std::move(message);
      }
    }
    spaceCv.notify_all();
  };

  auto acquireBytes = [&](std::uint64_t bytes) {
    std::unique_lock lock(stateMutex);
    for (;;) {
      if (failed) {
        return false;
      }
      if (inFlightBytes == 0 || inFlightBytes + bytes <= maxInFlightBytes) {
        inFlightBytes += bytes;
        return true;
      }
      lock.unlock();
      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        return false;
      }
      lock.lock();
      spaceCv.wait_for(lock, std::chrono::milliseconds(20));
    }
  };

  auto releaseBytes = [&](std::uint64_t bytes) {
    {
      std::lock_guard lock(stateMutex);
      inFlightBytes = bytes > inFlightBytes ? 0 : inFlightBytes - bytes;
    }
    spaceCv.notify_all();
  };

  auto worker = [&]() {
    UnarrStreamHandle stream;
    UnarrArchiveHandle archive;
    std::string openError;
    if (!openUnarrRarArchive(archivePath, stream, archive, &openError)) {
      setFailure(openError.empty()
                     ? "Could not open RAR archive for random access."
                     : openError);
      return;
    }

    long long localAcquireMicros = 0;
    long long localExtractMicros = 0;
    long long localCallbackMicros = 0;

    for (;;) {
      UnarrRarReadTarget target;
      {
        std::lock_guard lock(stateMutex);
        if (failed || nextTarget >= readTargets.size()) {
          break;
        }
        target = readTargets[nextTarget++];
      }

      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        break;
      }

      const auto acquireStart = std::chrono::steady_clock::now();
      if (!acquireBytes(target.size)) {
        break;
      }
      localAcquireMicros += elapsedMicrosSince(acquireStart);

      FileData file;
      std::string readError;
      const auto extractStart = std::chrono::steady_clock::now();
      bool ok = readUnarrRarTargetByOffset(archive.get(), target, file,
                                           &readError, pauseCallback);
      localExtractMicros += elapsedMicrosSince(extractStart);
      if (ok) {
        {
          std::lock_guard lock(stateMutex);
          ok = !failed;
        }
      }
      if (ok) {
        const auto callbackStart = std::chrono::steady_clock::now();
        ok = emitFileData(std::move(file), onFile, &readError);
        localCallbackMicros += elapsedMicrosSince(callbackStart);
      }
      releaseBytes(target.size);

      if (!ok) {
        setFailure(readError.empty()
                       ? "Could not extract RAR entry by offset."
                       : readError);
        break;
      }
      {
        std::lock_guard lock(stateMutex);
        ++emittedFiles;
      }
    }

    {
      std::lock_guard lock(stateMutex);
      acquireMicros += localAcquireMicros;
      extractMicros += localExtractMicros;
      callbackMicros += localCallbackMicros;
    }
  };

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  appendDebugLogLineImpl("Starting concurrent unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " workers=" + std::to_string(maxWorkers) +
                         " firstOffset=" +
                         std::to_string(readTargets.front().offset) +
                         " lastOffset=" +
                         std::to_string(readTargets.back().offset) +
                         " maxInFlightBytes=" +
                         std::to_string(maxInFlightBytes));

  std::vector<std::thread> workers;
  workers.reserve(maxWorkers);
  for (std::size_t i = 0; i < maxWorkers; ++i) {
    workers.emplace_back(worker);
  }
  for (auto &thread : workers) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (failed) {
    if (errorMessage != nullptr) {
      *errorMessage = failureMessage.empty()
                          ? "Parallel RAR extraction failed."
                          : failureMessage;
    }
    return false;
  }

  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count();
  appendDebugLogLineImpl("Finished concurrent unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " files=" + std::to_string(emittedFiles) +
                         " workers=" + std::to_string(maxWorkers) +
                         " maxInFlightBytes=" +
                         std::to_string(maxInFlightBytes) +
                         " extractMs=" + std::to_string(extractMs) +
                         " workerExtractMs=" +
                         std::to_string(extractMicros / 1000) +
                         " callbackMs=" +
                         std::to_string(callbackMicros / 1000) +
                         " acquireMs=" +
                         std::to_string(acquireMicros / 1000));
  return true;
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
struct SevenZipReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::uint64_t size = 0;
  bool solid = false;
};

struct SevenZipRar5BatchStats {
  std::size_t targets = 0;
  std::uint64_t totalBytes = 0;
  std::uint64_t maxBytes = 0;
  bool hasSolid = false;
};

constexpr std::uint64_t kSevenZipRar5SingleHandleMaxTargetBytes =
    512ull * 1024ull * 1024ull;
constexpr std::size_t kSevenZipRar5TargetChunksPerWorker = 4;

void addSaturated(std::uint64_t &total, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) {
    total = std::numeric_limits<std::uint64_t>::max();
  } else {
    total += value;
  }
}

bool sevenZipRar5BatchStats(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    SevenZipRar5BatchStats &stats, std::string *errorMessage,
    const PauseCallback &pauseCallback) {
  stats = {};
  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat !=
          static_cast<unsigned char>(SevenZipFormat::Rar5)) {
    return false;
  }

  for (const auto &innerPath : innerPaths) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    ++stats.targets;
    addSaturated(stats.totalBytes, entry->size);
    stats.maxBytes = std::max(stats.maxBytes, entry->size);
    stats.hasSolid = stats.hasSolid || entry->solid;
  }
  return true;
}

std::size_t solidReplayStartOrderForTarget(const CachedIndex &index,
                                           const SevenZipReadTarget &target) {
  if (!target.solid) {
    return target.order;
  }

  auto it = std::lower_bound(
      index.entries.begin(), index.entries.end(), target.order,
      [](const Entry &entry, std::size_t order) { return entry.order < order; });
  if (it == index.entries.end() || it->order != target.order) {
    return target.order;
  }

  while (it != index.entries.begin() && it->solid) {
    --it;
  }
  return it->order;
}

void sevenZipSolidReplayEstimate(const CachedIndex &index,
                                 const std::vector<SevenZipReadTarget> &targets,
                                 std::size_t &replayStartOrder,
                                 std::size_t &replayEntryCount,
                                 std::uint64_t &replayUnpackedBytes) {
  replayStartOrder = targets.empty() ? 0 : targets.front().order;
  replayEntryCount = 0;
  replayUnpackedBytes = 0;
  if (targets.empty()) {
    return;
  }

  bool hasSolidTarget = false;
  for (const SevenZipReadTarget &target : targets) {
    if (!target.solid) {
      continue;
    }
    hasSolidTarget = true;
    replayStartOrder = std::min(
        replayStartOrder, solidReplayStartOrderForTarget(index, target));
  }
  if (!hasSolidTarget) {
    return;
  }

  const std::size_t replayEndOrder = targets.back().order;
  for (const Entry &entry : index.entries) {
    if (entry.order < replayStartOrder || entry.order > replayEndOrder) {
      continue;
    }
    ++replayEntryCount;
    if (!entry.directory) {
      replayUnpackedBytes += entry.size;
    }
  }
}

bool readSevenZipEntriesByIndex(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat == 0) {
    return false;
  }

  std::vector<SevenZipReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (entry->order > std::numeric_limits<UInt32>::max()) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
        .solid = entry->solid,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const SevenZipReadTarget &a, const SevenZipReadTarget &b) {
              return a.order < b.order;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const SevenZipReadTarget &a,
                                   const SevenZipReadTarget &b) {
                                  return a.order == b.order;
                                }),
                    readTargets.end());

  std::size_t predictedSolidReplayStart = 0;
  std::size_t predictedSolidReplayEntries = 0;
  std::uint64_t predictedSolidReplayBytes = 0;
  sevenZipSolidReplayEstimate(*index, readTargets, predictedSolidReplayStart,
                              predictedSolidReplayEntries,
                              predictedSolidReplayBytes);

  bool archiveCacheHit = false;
  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, index->sevenZipFormat, &archiveCacheHit, &openMs,
      errorMessage, pauseCallback);
  if (archiveState == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  SevenZipPauseCallbackScope pauseScope(archiveState->inputStream,
                                        pauseCallback);
  IInArchive *archive = archiveState->archive.Interface();
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Cached 7-Zip archive is unavailable.";
    }
    return false;
  }
  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  std::vector<UInt32> itemIndices;
  itemIndices.reserve(readTargets.size());
  std::unordered_map<UInt32, FileData *> outputTargets;
  outputTargets.reserve(readTargets.size());
  files.reserve(readTargets.size());
  std::size_t solidTargets = 0;

  for (const SevenZipReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      files.clear();
      return false;
    }
    const auto itemIndex = static_cast<UInt32>(target.order);
    if (itemIndex >= itemCount) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      files.clear();
      return false;
    }

    auto actualPath = sevenZipStringProperty(archive, itemIndex, kpidPath);
    if (!actualPath.has_value() || actualPath->empty()) {
      actualPath = sevenZipStringProperty(archive, itemIndex, kpidName);
    }
    std::filesystem::path relativePath;
    if (!actualPath.has_value() ||
        !safeEntryPath(*actualPath, relativePath) ||
        normalizeEntryName(relativePath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index did not match cached entry path.";
      }
      files.clear();
      return false;
    }
    if (sevenZipBoolProperty(archive, itemIndex, kpidIsDir, false) ||
        sevenZipBoolProperty(archive, itemIndex, kpidEncrypted, false)) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive entry is not extractable.";
      }
      files.clear();
      return false;
    }
    const bool actualSolid =
        sevenZipBoolProperty(archive, itemIndex, kpidSolid, false);
    if (actualSolid != target.solid) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive solid flag did not match cached index.";
      }
      files.clear();
      return false;
    }
    if (target.solid) {
      ++solidTargets;
    }

    FileData file;
    file.path = target.entryPath;
    if (target.size > 0) {
      reserveBufferedBytes(file.bytes, target.size);
    }
    files.push_back(std::move(file));
    itemIndices.push_back(itemIndex);
    outputTargets.emplace(itemIndex, &files.back());
  }

  auto *callback =
      new SevenZipExtractCallback(std::move(outputTargets), pauseCallback);
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  appendDebugLogLineImpl("Starting 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " firstOrder=" +
                         std::to_string(readTargets.front().order) +
                         " lastOrder=" +
                         std::to_string(readTargets.back().order) +
                         " solidTargets=" +
                         std::to_string(solidTargets) +
                         (solidTargets > 0
                              ? " solidReplayStart=" +
                                    std::to_string(predictedSolidReplayStart) +
                                    " solidReplayEntries=" +
                                    std::to_string(predictedSolidReplayEntries) +
                                    " solidReplayUnpacked=" +
                                    byteCountForLog(predictedSolidReplayBytes) +
                                    " note=solid-archives-decode-sequentially"
                              : "") +
                         " archiveCache=" +
                         (archiveCacheHit ? "hit" : "miss") +
                         " openMs=" + std::to_string(openMs));
  using Clock = std::chrono::steady_clock;
  const auto extractStart = Clock::now();
  result =
      archive->Extract(itemIndices.data(), static_cast<UInt32>(itemIndices.size()),
                       0, callbackHandle);
  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            extractStart)
          .count();
  if (result != S_OK || callback->failed() || callback->cancelled()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          callback->cancelled()
              ? "Operation cancelled"
              : result != S_OK
              ? sevenZipResultMessage(result)
              : "7-Zip archive extraction failed with operation result: " +
                    std::to_string(callback->operationResult());
    }
    files.clear();
    return false;
  }

  appendDebugLogLineImpl("Finished 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " files=" + std::to_string(files.size()) +
                         " extractMs=" + std::to_string(extractMs));
  return true;
}

bool readSevenZipEntriesByIndexStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat == 0) {
    return false;
  }

  std::vector<SevenZipReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (entry->order > std::numeric_limits<UInt32>::max()) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
        .solid = entry->solid,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const SevenZipReadTarget &a, const SevenZipReadTarget &b) {
              return a.order < b.order;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const SevenZipReadTarget &a,
                                   const SevenZipReadTarget &b) {
                                  return a.order == b.order;
                                }),
                    readTargets.end());

  std::size_t predictedSolidReplayStart = 0;
  std::size_t predictedSolidReplayEntries = 0;
  std::uint64_t predictedSolidReplayBytes = 0;
  sevenZipSolidReplayEstimate(*index, readTargets, predictedSolidReplayStart,
                              predictedSolidReplayEntries,
                              predictedSolidReplayBytes);

  bool archiveCacheHit = false;
  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, index->sevenZipFormat, &archiveCacheHit, &openMs,
      errorMessage, pauseCallback);
  if (archiveState == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  SevenZipPauseCallbackScope pauseScope(archiveState->inputStream,
                                        pauseCallback);
  IInArchive *archive = archiveState->archive.Interface();
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Cached 7-Zip archive is unavailable.";
    }
    return false;
  }
  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  std::vector<UInt32> itemIndices;
  itemIndices.reserve(readTargets.size());
  std::unordered_map<UInt32, std::filesystem::path> outputTargets;
  outputTargets.reserve(readTargets.size());
  std::size_t solidTargets = 0;

  for (const SevenZipReadTarget &target : readTargets) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const auto itemIndex = static_cast<UInt32>(target.order);
    if (itemIndex >= itemCount) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      return false;
    }

    auto actualPath = sevenZipStringProperty(archive, itemIndex, kpidPath);
    if (!actualPath.has_value() || actualPath->empty()) {
      actualPath = sevenZipStringProperty(archive, itemIndex, kpidName);
    }
    std::filesystem::path relativePath;
    if (!actualPath.has_value() ||
        !safeEntryPath(*actualPath, relativePath) ||
        normalizeEntryName(relativePath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index did not match cached entry path.";
      }
      return false;
    }
    if (sevenZipBoolProperty(archive, itemIndex, kpidIsDir, false) ||
        sevenZipBoolProperty(archive, itemIndex, kpidEncrypted, false)) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive entry is not extractable.";
      }
      return false;
    }
    const bool actualSolid =
        sevenZipBoolProperty(archive, itemIndex, kpidSolid, false);
    if (actualSolid != target.solid) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive solid flag did not match cached index.";
      }
      return false;
    }
    if (target.solid) {
      ++solidTargets;
    }

    itemIndices.push_back(itemIndex);
    outputTargets.emplace(itemIndex, target.entryPath);
  }

  auto *callback = new SevenZipStreamingExtractCallback(
      std::move(outputTargets), onFile, pauseCallback);
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  appendDebugLogLineImpl("Starting streaming 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " firstOrder=" +
                         std::to_string(readTargets.front().order) +
                         " lastOrder=" +
                         std::to_string(readTargets.back().order) +
                         " solidTargets=" +
                         std::to_string(solidTargets) +
                         (solidTargets > 0
                              ? " solidReplayStart=" +
                                    std::to_string(predictedSolidReplayStart) +
                                    " solidReplayEntries=" +
                                    std::to_string(predictedSolidReplayEntries) +
                                    " solidReplayUnpacked=" +
                                    byteCountForLog(predictedSolidReplayBytes) +
                                    " note=solid-archives-decode-sequentially"
                              : "") +
                         " archiveCache=" +
                         (archiveCacheHit ? "hit" : "miss") +
                         " openMs=" + std::to_string(openMs));
  using Clock = std::chrono::steady_clock;
  const auto extractStart = Clock::now();
  result =
      archive->Extract(itemIndices.data(), static_cast<UInt32>(itemIndices.size()),
                       0, callbackHandle);
  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            extractStart)
          .count();
  if (result != S_OK || callback->failed() || callback->cancelled()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          callback->consumerCancelled()
              ? "Archive file consumer cancelled."
              : callback->cancelled()
              ? "Operation cancelled"
              : result != S_OK
              ? sevenZipResultMessage(result)
              : "7-Zip archive extraction failed with operation result: " +
                    std::to_string(callback->operationResult());
    }
    return false;
  }

  appendDebugLogLineImpl("Finished streaming 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " files=" + std::to_string(callback->emittedFiles()) +
                         " extractMs=" + std::to_string(extractMs));
  return true;
}

bool sevenZipEntryMatchesTarget(IInArchive *archive, UInt32 itemIndex,
                                const SevenZipReadTarget &target,
                                std::string *errorMessage) {
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive is unavailable.";
    }
    return false;
  }

  auto actualPath = sevenZipStringProperty(archive, itemIndex, kpidPath);
  if (!actualPath.has_value() || actualPath->empty()) {
    actualPath = sevenZipStringProperty(archive, itemIndex, kpidName);
  }
  std::filesystem::path relativePath;
  if (!actualPath.has_value() || !safeEntryPath(*actualPath, relativePath) ||
      normalizeEntryName(relativePath.generic_string()) !=
          normalizeEntryName(target.entryPath.generic_string())) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive index did not match cached entry path.";
    }
    return false;
  }

  if (sevenZipBoolProperty(archive, itemIndex, kpidIsDir, false) ||
      sevenZipBoolProperty(archive, itemIndex, kpidEncrypted, false)) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive entry is not extractable.";
    }
    return false;
  }
  const bool actualSolid =
      sevenZipBoolProperty(archive, itemIndex, kpidSolid, false);
  if (actualSolid != target.solid) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive solid flag did not match cached index.";
    }
    return false;
  }
  return true;
}

bool readSevenZipTargetByIndex(IInArchive *archive,
                               const SevenZipReadTarget &target,
                               FileData &file, std::string *errorMessage,
                               const PauseCallback &pauseCallback) {
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive is unavailable.";
    }
    return false;
  }
  if (target.order > std::numeric_limits<UInt32>::max()) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive index is out of range.";
    }
    return false;
  }

  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  const auto itemIndex = static_cast<UInt32>(target.order);
  if (itemIndex >= itemCount) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive index is out of range.";
    }
    return false;
  }
  if (!sevenZipEntryMatchesTarget(archive, itemIndex, target, errorMessage)) {
    return false;
  }

  file.path = target.entryPath;
  file.bytes.clear();
  if (target.size > 0) {
    reserveBufferedBytes(file.bytes, target.size);
  }

  std::unordered_map<UInt32, FileData *> outputTargets;
  outputTargets.emplace(itemIndex, &file);
  auto *callback =
      new SevenZipExtractCallback(std::move(outputTargets), pauseCallback);
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  result = archive->Extract(&itemIndex, 1, 0, callbackHandle);
  if (result != S_OK || callback->failed() || callback->cancelled()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          callback->cancelled()
              ? "Operation cancelled"
              : result != S_OK
              ? sevenZipResultMessage(result)
              : "7-Zip archive extraction failed with operation result: " +
                    std::to_string(callback->operationResult());
    }
    return false;
  }
  return true;
}

bool readSevenZipTargetsByIndexStreaming(
    IInArchive *archive, const std::vector<SevenZipReadTarget> &readTargets,
    std::size_t begin, std::size_t end, const FileDataCallback &onFile,
    const std::function<bool(std::uint64_t)> &acquireBytes,
    const std::function<void(std::uint64_t)> &releaseBytes,
    std::size_t &emittedFiles, long long &callbackMicros,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  emittedFiles = 0;
  callbackMicros = 0;
  if (begin >= end) {
    return true;
  }
  if (!pauseIfNeeded(pauseCallback, errorMessage)) {
    return false;
  }
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "7-Zip archive is unavailable.";
    }
    return false;
  }

  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  std::vector<UInt32> itemIndices;
  itemIndices.reserve(end - begin);
  std::unordered_map<UInt32, SevenZipStreamingTarget> outputTargets;
  outputTargets.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const SevenZipReadTarget &target = readTargets[i];
    if (target.order > std::numeric_limits<UInt32>::max()) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      return false;
    }
    const auto itemIndex = static_cast<UInt32>(target.order);
    if (itemIndex >= itemCount) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      return false;
    }
    if (!sevenZipEntryMatchesTarget(archive, itemIndex, target,
                                    errorMessage)) {
      return false;
    }
    itemIndices.push_back(itemIndex);
    outputTargets.emplace(
        itemIndex,
        SevenZipStreamingTarget{.path = target.entryPath, .size = target.size});
  }

  auto *callback = new SevenZipThrottledStreamingExtractCallback(
      std::move(outputTargets), onFile, acquireBytes, releaseBytes,
      pauseCallback);
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  result =
      archive->Extract(itemIndices.data(), static_cast<UInt32>(itemIndices.size()),
                       0, callbackHandle);
  emittedFiles = callback->emittedFiles();
  callbackMicros = callback->callbackMicros();
  if (result != S_OK || callback->failed() || callback->cancelled()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          callback->consumerCancelled()
              ? "Archive file consumer cancelled."
              : callback->cancelled()
              ? "Operation cancelled"
              : result != S_OK
              ? sevenZipResultMessage(result)
              : "7-Zip archive extraction failed with operation result: " +
                    std::to_string(callback->operationResult());
    }
    return false;
  }
  return true;
}

bool readSevenZipEntriesByIndexConcurrent(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, const FileDataCallback &onFile,
    std::size_t maxWorkers, std::uint64_t maxInFlightBytes,
    std::string *errorMessage, const PauseCallback &pauseCallback) {
  if (innerPaths.empty()) {
    return true;
  }
  if (!onFile) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat !=
          static_cast<unsigned char>(SevenZipFormat::Rar5)) {
    return false;
  }

  std::vector<SevenZipReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    if (!pauseIfNeeded(pauseCallback, errorMessage)) {
      return false;
    }
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (entry->order > std::numeric_limits<UInt32>::max()) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    if (entry->solid) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "RAR5 entry is solid; parallel random extraction is disabled.";
      }
      return false;
    }
    if (entry->size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      if (errorMessage != nullptr) {
        *errorMessage = "RAR5 entry is too large to read into memory.";
      }
      return false;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
        .solid = entry->solid,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const SevenZipReadTarget &a, const SevenZipReadTarget &b) {
              return a.order < b.order;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const SevenZipReadTarget &a,
                                   const SevenZipReadTarget &b) {
                                  return a.order == b.order;
                                }),
                    readTargets.end());

  std::uint64_t totalTargetBytes = 0;
  std::uint64_t maxTargetBytes = 0;
  for (const SevenZipReadTarget &target : readTargets) {
    addSaturated(totalTargetBytes, target.size);
    maxTargetBytes = std::max(maxTargetBytes, target.size);
  }

  const std::size_t requestedWorkers = maxWorkers;
  maxWorkers =
      std::max<std::size_t>(1, std::min(maxWorkers, readTargets.size()));
  if (maxInFlightBytes == 0) {
    maxInFlightBytes = std::numeric_limits<std::uint64_t>::max();
  }
  const std::size_t targetChunkCount =
      std::min<std::size_t>(
          readTargets.size(),
          maxWorkers >
                  std::numeric_limits<std::size_t>::max() /
                      kSevenZipRar5TargetChunksPerWorker
              ? readTargets.size()
              : maxWorkers * kSevenZipRar5TargetChunksPerWorker);
  const std::size_t targetChunkSize =
      std::max<std::size_t>(
          1, (readTargets.size() + targetChunkCount - 1) / targetChunkCount);

  std::mutex stateMutex;
  std::condition_variable spaceCv;
  std::size_t nextTarget = 0;
  std::size_t emittedFiles = 0;
  std::uint64_t inFlightBytes = 0;
  bool failed = false;
  std::string failureMessage;
  long long acquireMicros = 0;
  long long openMicros = 0;
  long long extractMicros = 0;
  long long callbackMicros = 0;

  auto setFailure = [&](std::string message) {
    {
      std::lock_guard lock(stateMutex);
      if (!failed) {
        failed = true;
        failureMessage = std::move(message);
      }
    }
    spaceCv.notify_all();
  };

  auto acquireBytes = [&](std::uint64_t bytes) {
    std::unique_lock lock(stateMutex);
    for (;;) {
      if (failed) {
        return false;
      }
      if (inFlightBytes == 0 || inFlightBytes + bytes <= maxInFlightBytes) {
        inFlightBytes += bytes;
        return true;
      }
      lock.unlock();
      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        return false;
      }
      lock.lock();
      spaceCv.wait_for(lock, std::chrono::milliseconds(20));
    }
  };

  auto releaseBytes = [&](std::uint64_t bytes) {
    {
      std::lock_guard lock(stateMutex);
      inFlightBytes = bytes > inFlightBytes ? 0 : inFlightBytes - bytes;
    }
    spaceCv.notify_all();
  };

  auto worker = [&]() {
    long long localAcquireMicros = 0;
    long long localOpenMicros = 0;
    long long localExtractMicros = 0;
    long long localCallbackMicros = 0;
    std::size_t localEmittedFiles = 0;

    CMyComPtr<IInArchive> archiveHandle;
    CMyComPtr<IInStream> streamHandle;
    std::string openError;
    const auto openStart = std::chrono::steady_clock::now();
    if (!openSevenZipArchive(archivePath, index->sevenZipFormat, archiveHandle,
                             streamHandle, &openError, pauseCallback)) {
      setFailure(openError.empty()
                     ? "Could not open RAR5 archive for parallel extraction."
                     : openError);
      return;
    }
    localOpenMicros += elapsedMicrosSince(openStart);
    IInArchive *archive = archiveHandle.Interface();
    if (sevenZipArchiveBoolProperty(archive, kpidSolid, false)) {
      setFailure("RAR5 archive is solid; parallel extraction is disabled.");
      return;
    }

    FileDataCallback guardedOnFile = [&](FileData &&file) {
      {
        std::lock_guard lock(stateMutex);
        if (failed) {
          return false;
        }
      }
      return onFile(std::move(file));
    };
    auto timedAcquireBytes = [&](std::uint64_t bytes) {
      const auto acquireStart = std::chrono::steady_clock::now();
      const bool acquired = acquireBytes(bytes);
      localAcquireMicros += elapsedMicrosSince(acquireStart);
      return acquired;
    };

    for (;;) {
      std::size_t chunkBegin = 0;
      std::size_t chunkEnd = 0;
      {
        std::lock_guard lock(stateMutex);
        if (failed || nextTarget >= readTargets.size()) {
          break;
        }
        chunkBegin = nextTarget;
        chunkEnd = std::min(readTargets.size(), chunkBegin + targetChunkSize);
        nextTarget = chunkEnd;
      }

      std::string pauseError;
      if (!pauseIfNeeded(pauseCallback, &pauseError)) {
        setFailure(pauseError.empty() ? "Operation cancelled" : pauseError);
        break;
      }

      std::string readError;
      std::size_t chunkEmittedFiles = 0;
      long long chunkCallbackMicros = 0;
      const auto extractStart = std::chrono::steady_clock::now();
      bool ok = readSevenZipTargetsByIndexStreaming(
          archive, readTargets, chunkBegin, chunkEnd, guardedOnFile,
          timedAcquireBytes, releaseBytes, chunkEmittedFiles,
          chunkCallbackMicros, &readError, pauseCallback);
      localExtractMicros += elapsedMicrosSince(extractStart);
      localCallbackMicros += chunkCallbackMicros;
      if (ok) {
        {
          std::lock_guard lock(stateMutex);
          ok = !failed;
        }
      }
      if (ok) {
        localEmittedFiles += chunkEmittedFiles;
      } else {
        setFailure(readError.empty()
                       ? "Could not extract RAR5 entry batch by index."
                       : readError);
        break;
      }
    }

    {
      std::lock_guard lock(stateMutex);
      acquireMicros += localAcquireMicros;
      openMicros += localOpenMicros;
      extractMicros += localExtractMicros;
      callbackMicros += localCallbackMicros;
      emittedFiles += localEmittedFiles;
    }
  };

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  appendDebugLogLineImpl("Starting concurrent 7-Zip RAR5 extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " workers=" + std::to_string(maxWorkers) +
                         " requestedWorkers=" +
                         std::to_string(requestedWorkers) +
                         " firstOrder=" +
                         std::to_string(readTargets.front().order) +
                         " lastOrder=" +
                         std::to_string(readTargets.back().order) +
                         " targetBytes=" +
                         byteCountForLog(totalTargetBytes) +
                         " maxTargetBytes=" +
                         byteCountForLog(maxTargetBytes) +
                         " chunkSize=" + std::to_string(targetChunkSize) +
                         " maxInFlightBytes=" +
                         std::to_string(maxInFlightBytes));

  std::vector<std::thread> workers;
  workers.reserve(maxWorkers);
  for (std::size_t i = 0; i < maxWorkers; ++i) {
    workers.emplace_back(worker);
  }
  for (auto &thread : workers) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (failed) {
    if (errorMessage != nullptr) {
      *errorMessage = failureMessage.empty()
                          ? "Parallel RAR5 extraction failed."
                          : failureMessage;
    }
    return false;
  }

  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count();
  appendDebugLogLineImpl("Finished concurrent 7-Zip RAR5 extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " files=" + std::to_string(emittedFiles) +
                         " workers=" + std::to_string(maxWorkers) +
                         " requestedWorkers=" +
                         std::to_string(requestedWorkers) +
                         " targetBytes=" +
                         byteCountForLog(totalTargetBytes) +
                         " maxTargetBytes=" +
                         byteCountForLog(maxTargetBytes) +
                         " chunkSize=" + std::to_string(targetChunkSize) +
                         " maxInFlightBytes=" +
                         std::to_string(maxInFlightBytes) +
                         " extractMs=" + std::to_string(extractMs) +
                         " workerOpenMs=" +
                         std::to_string(openMicros / 1000) +
                         " workerExtractMs=" +
                         std::to_string(extractMicros / 1000) +
                         " callbackMs=" +
                         std::to_string(callbackMicros / 1000) +
                         " acquireMs=" +
                         std::to_string(acquireMicros / 1000));
  return true;
}

bool extractSevenZipArchiveFully(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputFolder,
    const std::shared_ptr<const CachedIndex> &index,
    const std::stop_token *stopToken,
    const UnzipProgressCallback &progressCallback,
    const PauseCallback &pauseCallback,
    std::string *errorMessage) {
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat == 0) {
    return false;
  }
  bool archiveCacheHit = false;
  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, index->sevenZipFormat, &archiveCacheHit, &openMs,
      errorMessage);
  if (archiveState == nullptr) {
    return false;
  }

  const PauseCallback inputPauseCallback = [stopToken, pauseCallback] {
    return !stopRequested(stopToken) && pauseIfNeeded(pauseCallback);
  };
  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  SevenZipPauseCallbackScope pauseScope(archiveState->inputStream,
                                        std::move(inputPauseCallback));
  IInArchive *archive = archiveState->archive.Interface();
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Cached 7-Zip archive is unavailable.";
    }
    return false;
  }

  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  std::unordered_map<UInt32, Entry> entries;
  entries.reserve(index->entries.size());
  std::uint64_t fileCount = 0;
  for (const Entry &entry : index->entries) {
    if (entry.order > std::numeric_limits<UInt32>::max()) {
      continue;
    }
    const auto itemIndex = static_cast<UInt32>(entry.order);
    if (itemIndex >= itemCount) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      return false;
    }
    if (entry.directory) {
      std::error_code error;
      if (!createDirectoriesForUnzip(outputFolder / entry.path,
                                     "Could not create unzip folder",
                                     errorMessage, error)) {
        return false;
      }
    } else {
      ++fileCount;
    }
    entries.emplace(itemIndex, entry);
  }

  auto *callback = new SevenZipFullExtractCallback(
      outputFolder, std::move(entries), fileCount, stopToken, progressCallback,
      pauseCallback);
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  appendDebugLogLineImpl("Starting full 7-Zip unzip: " +
                         pathForLog(archivePath) +
                         " files=" + std::to_string(fileCount) +
                         " archiveCache=" +
                         (archiveCacheHit ? "hit" : "miss") +
                         " openMs=" + std::to_string(openMs));
  using Clock = std::chrono::steady_clock;
  const auto extractStart = Clock::now();
  result = archive->Extract(nullptr, static_cast<UInt32>(-1), 0,
                            callbackHandle);
  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            extractStart)
          .count();
  if (result != S_OK || callback->failed() || callback->cancelled()) {
    if (errorMessage != nullptr) {
      if (callback->cancelled() || stopRequested(stopToken)) {
        *errorMessage = "Unzip cancelled";
      } else if (result != S_OK) {
        *errorMessage = sevenZipResultMessage(result);
      } else {
        *errorMessage =
            "7-Zip archive extraction failed with operation result: " +
            std::to_string(callback->operationResult());
      }
    }
    return false;
  }

  appendDebugLogLineImpl("Finished full 7-Zip unzip: " +
                         pathForLog(archivePath) +
                         " files=" + std::to_string(fileCount) +
                         " extractMs=" + std::to_string(extractMs));
  return true;
}
#endif

std::optional<std::filesystem::path>
resolveInnerPath(const std::filesystem::path &archivePath,
                 const std::filesystem::path &innerPath) {
  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr) {
    return std::nullopt;
  }
  if (const Entry *entry = findIndexedEntry(*index, innerPath);
      entry != nullptr && !entry->directory) {
    return entry->path;
  }
  return std::nullopt;
}

std::uint64_t fnv1a64(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = digits[value & 0xf];
    value >>= 4;
  }
  return out;
}

bool stopRequested(const std::stop_token *stopToken) {
  return stopToken != nullptr && stopToken->stop_requested();
}

bool pauseIfNeeded(const PauseCallback &pauseCallback,
                   std::string *errorMessage) {
  if (pauseCallback == nullptr || pauseCallback()) {
    return true;
  }
  if (errorMessage != nullptr && errorMessage->empty()) {
    *errorMessage = "Operation cancelled";
  }
  return false;
}

bool unzipCheckpoint(const std::stop_token *stopToken,
                     const PauseCallback &pauseCallback,
                     std::string *errorMessage) {
  if (!stopRequested(stopToken) && pauseIfNeeded(pauseCallback)) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = "Unzip cancelled";
  }
  return false;
}

bool emitFileData(FileData &&file, const FileDataCallback &onFile,
                  std::string *errorMessage) {
  if (!onFile) {
    if (errorMessage != nullptr && errorMessage->empty()) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }
  if (onFile(std::move(file))) {
    return true;
  }
  if (errorMessage != nullptr && errorMessage->empty()) {
    *errorMessage = "Archive file consumer cancelled.";
  }
  return false;
}

bool archiveReadCancelled(const std::string &errorMessage) {
  return errorMessage == "Operation cancelled" ||
         errorMessage == "Archive file consumer cancelled.";
}

void reportUnzipProgress(const UnzipProgressCallback &callback,
                         double fraction, std::uint64_t current,
                         std::uint64_t total, std::string message) {
  if (!callback) {
    return;
  }
  callback(UnzipProgress{.fraction = std::clamp(fraction, 0.0, 1.0),
                         .current = current,
                         .total = total,
                         .message = std::move(message)});
}

std::string readableUnzipFolderName(const std::filesystem::path &archivePath,
                                    const std::filesystem::path &folderPath) {
  std::string name;
  if (!folderPath.empty()) {
    name = folderPath.filename().generic_string();
  }
  if (name.empty() || name == "." || name == "..") {
    name = archivePath.stem().generic_string();
  }
  if (name.empty()) {
    name = "Unzipped Chart";
  }

  for (char &ch : name) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
        ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
      ch = '_';
    }
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
    name.pop_back();
  }
  if (name.empty()) {
    name = "Unzipped Chart";
  }
  return name;
}

bool unzipMarkerMatches(const std::filesystem::path &markerPath,
                        const std::string &key) {
  std::ifstream marker(markerPath, std::ios::binary);
  if (!marker) {
    return false;
  }
  std::string firstLine;
  std::getline(marker, firstLine);
  return firstLine == key;
}

std::filesystem::path archiveCacheRoot() {
  return std::filesystem::temp_directory_path() / "AsoBMaShowArchiveCache";
}

} // namespace

bool isArchiveSupportAvailable() {
  return ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_UNARR != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP != 0;
}

void setCachePathNormalizer(CachePathNormalizer normalizer) {
  std::lock_guard<std::mutex> lock(gCachePathNormalizerMutex);
  gCachePathNormalizer = std::move(normalizer);
}

void setArchiveIndexCacheDirectory(std::filesystem::path directory) {
  std::lock_guard<std::mutex> lock(gIndexCacheDirectoryMutex);
  gArchiveIndexCacheDirectory = std::move(directory);
}

std::filesystem::path archiveIndexCacheDirectory() {
  std::lock_guard<std::mutex> lock(gIndexCacheDirectoryMutex);
  return gArchiveIndexCacheDirectory;
}

std::size_t pruneArchiveIndexCache(
    const std::vector<std::filesystem::path> &liveArchivePaths) {
  return pruneArchiveIndexCacheImpl(liveArchivePaths);
}

#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
void clearArchiveIndexCacheForTesting() {
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    gIndexCache.clear();
  }
  {
    std::lock_guard<std::mutex> lock(gIndexBuildMutex);
    gIndexBuildInFlight.clear();
    gIndexBuildActive.clear();
    gIndexBuildDone.clear();
    gIndexBuildFailed.clear();
  }
  setArchiveIndexCacheDirectory({});
}
#endif

bool hasSupportedArchiveExtension(const std::filesystem::path &path) {
  return !archiveExtensionFromPath(path).empty();
}

void appendDebugLogLine(const std::string &message) {
  appendDebugLogLineImpl(message);
}

#if defined(ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS)
void setStreamingEntryObserverForTesting(
    StreamingEntryObserverForTesting observer) {
  std::lock_guard lock(gStreamingEntryObserverMutex);
  gStreamingEntryObserver = std::move(observer);
}

void resetSingleFlightWaiterCountForTesting() {
  gSingleFlightWaiterCountForTesting.store(0, std::memory_order_relaxed);
}

std::uint32_t singleFlightWaiterCountForTesting() {
  return gSingleFlightWaiterCountForTesting.load(std::memory_order_relaxed);
}
#endif

std::uint64_t debugLogRevision() {
  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  return gDebugLogRevision;
}

std::vector<std::string> debugLogLines() {
  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  return {gDebugLogLines.begin(), gDebugLogLines.end()};
}

std::string debugLogText() {
  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  if (gDebugLogLines.empty()) {
    return "No parsing logs yet.";
  }

  std::string text;
  for (const std::string &line : gDebugLogLines) {
    if (!text.empty()) {
      text += '\n';
    }
    text += line;
  }
  return text;
}

bool splitVirtualPath(const std::filesystem::path &path,
                      std::filesystem::path &archivePath,
                      std::filesystem::path &innerPath) {
  archivePath.clear();
  innerPath.clear();
  if (path.empty()) {
    return false;
  }

  std::filesystem::path current;
  bool foundArchive = false;
  for (const auto &part : path.lexically_normal()) {
    if (!foundArchive) {
      current /= part;
      if (hasSupportedArchiveExtension(current)) {
        std::error_code error;
        if (std::filesystem::is_regular_file(current, error) && !error) {
          archivePath = current;
          foundArchive = true;
        }
      }
      continue;
    }
    innerPath /= part;
  }

  if (!foundArchive || innerPath.empty()) {
    archivePath.clear();
    innerPath.clear();
    return false;
  }
  return true;
}

bool isVirtualPath(const std::filesystem::path &path) {
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    return true;
  }
#endif
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  return splitVirtualPath(path, archivePath, innerPath);
}

std::filesystem::path makeVirtualPath(const std::filesystem::path &archivePath,
                                      const std::filesystem::path &innerPath) {
  if (innerPath.empty()) {
    return archivePath;
  }
  return archivePath / innerPath;
}

bool listEntries(const std::filesystem::path &archivePath,
                 std::vector<Entry> &entries, std::string *errorMessage,
                 PauseCallback pauseCallback) {
  entries.clear();
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    return false;
  }

  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr) {
    return false;
  }
  entries = index->entries;
  return true;
}

bool readArchiveEntries(const std::filesystem::path &archivePath,
                        const std::vector<std::filesystem::path> &innerPaths,
                        std::vector<FileData> &files, std::string *errorMessage,
                        PauseCallback pauseCallback) {
  files.clear();
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    appendDebugLogLineImpl("Archive batch read unsupported: " +
                           pathForLog(archivePath));
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    appendDebugLogLineImpl("Archive batch read unavailable: " +
                           pathForLog(archivePath));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                            &zipError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP batch read failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (hasRarArchiveExtension(archivePath) &&
      readUnarrRarEntriesByOffset(archivePath, innerPaths, std::nullopt, files,
                                  &unarrError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via unarr RAR random access: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasRarArchiveExtension(archivePath) && !unarrError.empty()) {
    appendDebugLogLineImpl("unarr RAR batch read failed: " +
                           pathForLog(archivePath) + ": " + unarrError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (hasSevenZipArchiveExtension(archivePath) &&
      readSevenZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                                 &sevenZipError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via 7-Zip SDK: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasSevenZipArchiveExtension(archivePath) && !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip batch read failed: " +
                           pathForLog(archivePath) + ": " + sevenZipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string cachedOrderError;
  if (readArchiveEntriesByCachedOrder(archivePath, innerPaths, std::nullopt,
                                      files, &cachedOrderError,
                                      pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via cached libarchive order: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (!cachedOrderError.empty()) {
    appendDebugLogLineImpl("Cached-order archive batch read failed: " +
                           pathForLog(archivePath) + ": " + cachedOrderError);
  }
  files.clear();
  const bool read =
      readArchiveEntriesUncached(archivePath, innerPaths, std::nullopt, files,
                                 errorMessage, pauseCallback);
  appendDebugLogLineImpl(
      std::string(read ? "Read archive batch via libarchive scan: "
                       : "Archive batch read failed: ") +
      pathForLog(archivePath) + " targets=" +
      std::to_string(innerPaths.size()) + " files=" +
      std::to_string(files.size()) +
      ((!read && errorMessage != nullptr && !errorMessage->empty())
           ? ": " + *errorMessage
           : ""));
  return read;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  appendDebugLogLineImpl("Archive support is not compiled in for batch read: " +
                         pathForLog(archivePath));
  return false;
#endif
}

bool readArchiveEntriesStreaming(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    FileDataCallback onFile,
    std::string *errorMessage,
    PauseCallback pauseCallback) {
  if (!onFile) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    appendDebugLogLineImpl("Archive streaming read unsupported: " +
                           pathForLog(archivePath));
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    appendDebugLogLineImpl("Archive streaming read unavailable: " +
                           pathForLog(archivePath));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndexStreaming(archivePath, innerPaths, std::nullopt,
                                     onFile, &zipError, pauseCallback)) {
    appendDebugLogLineImpl("Streamed archive batch via miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()));
    return true;
  }
  if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP streaming read failed: " +
                           pathForLog(archivePath) + ": " + zipError);
    if (archiveReadCancelled(zipError)) {
      if (errorMessage != nullptr) {
        *errorMessage = zipError;
      }
      return false;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (hasRarArchiveExtension(archivePath) &&
      readUnarrRarEntriesByOffsetStreaming(
          archivePath, innerPaths, std::nullopt, onFile, &unarrError,
          pauseCallback)) {
    appendDebugLogLineImpl(
        "Streamed archive batch via unarr RAR random access: " +
        pathForLog(archivePath) +
        " targets=" + std::to_string(innerPaths.size()));
    return true;
  }
  if (hasRarArchiveExtension(archivePath) && !unarrError.empty()) {
    appendDebugLogLineImpl("unarr RAR streaming read failed: " +
                           pathForLog(archivePath) + ": " + unarrError);
    if (archiveReadCancelled(unarrError)) {
      if (errorMessage != nullptr) {
        *errorMessage = unarrError;
      }
      return false;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (hasSevenZipArchiveExtension(archivePath) &&
      readSevenZipEntriesByIndexStreaming(archivePath, innerPaths,
                                          std::nullopt, onFile, &sevenZipError,
                                          pauseCallback)) {
    appendDebugLogLineImpl("Streamed archive batch via 7-Zip SDK: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()));
    return true;
  }
  if (hasSevenZipArchiveExtension(archivePath) && !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip streaming read failed: " +
                           pathForLog(archivePath) + ": " + sevenZipError);
    if (archiveReadCancelled(sevenZipError)) {
      if (errorMessage != nullptr) {
        *errorMessage = sevenZipError;
      }
      return false;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string cachedOrderError;
  if (readArchiveEntriesByCachedOrderStreaming(
          archivePath, innerPaths, std::nullopt, onFile, &cachedOrderError,
          pauseCallback)) {
    appendDebugLogLineImpl(
        "Streamed archive batch via cached libarchive order: " +
        pathForLog(archivePath) +
        " targets=" + std::to_string(innerPaths.size()));
    return true;
  }
  if (!cachedOrderError.empty()) {
    appendDebugLogLineImpl("Cached-order archive streaming read failed: " +
                           pathForLog(archivePath) + ": " + cachedOrderError);
    if (archiveReadCancelled(cachedOrderError)) {
      if (errorMessage != nullptr) {
        *errorMessage = cachedOrderError;
      }
      return false;
    }
  }
  const bool read = readArchiveEntriesUncachedStreaming(
      archivePath, innerPaths, std::nullopt, onFile, errorMessage,
      pauseCallback);
  appendDebugLogLineImpl(
      std::string(read ? "Streamed archive batch via libarchive scan: "
                       : "Archive streaming read failed: ") +
      pathForLog(archivePath) + " targets=" +
      std::to_string(innerPaths.size()) +
      ((!read && errorMessage != nullptr && !errorMessage->empty())
           ? ": " + *errorMessage
           : ""));
  return read;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  appendDebugLogLineImpl(
      "Archive support is not compiled in for streaming read: " +
      pathForLog(archivePath));
  return false;
#endif
}

bool readArchiveEntriesConcurrently(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    FileDataCallback onFile,
    std::size_t maxWorkers,
    std::uint64_t maxInFlightBytes,
    std::string *errorMessage,
    PauseCallback pauseCallback) {
  if (!onFile) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file consumer is unavailable.";
    }
    return false;
  }
  if (maxWorkers <= 1) {
    if (errorMessage != nullptr) {
      *errorMessage = "Parallel archive reading needs more than one worker.";
    }
    return false;
  }
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndexConcurrent(archivePath, innerPaths, std::nullopt,
                                      onFile, maxWorkers, maxInFlightBytes,
                                      &zipError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via concurrent miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " workers=" + std::to_string(maxWorkers));
    return true;
  }
  if (hasZipArchiveExtension(archivePath)) {
    if (!zipError.empty()) {
      appendDebugLogLineImpl("Concurrent miniz ZIP read failed: " +
                             pathForLog(archivePath) + ": " + zipError);
    }
    if (errorMessage != nullptr) {
      *errorMessage = zipError.empty()
                          ? "Archive is not a random-access ZIP archive."
                          : zipError;
    }
    return false;
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  const RarSignature rarArchiveSignature =
      hasRarArchiveExtension(archivePath) ? rarSignature(archivePath)
                                          : RarSignature::Unknown;
  if (rarArchiveSignature == RarSignature::Rar4 &&
      readUnarrRarEntriesByOffsetConcurrent(
          archivePath, innerPaths, std::nullopt, onFile, maxWorkers,
          maxInFlightBytes, &unarrError, pauseCallback)) {
    appendDebugLogLineImpl(
        "Read archive batch via concurrent unarr RAR random access: " +
        pathForLog(archivePath) +
        " targets=" + std::to_string(innerPaths.size()) +
        " workers=" + std::to_string(maxWorkers));
    return true;
  }
  if (rarArchiveSignature == RarSignature::Rar4) {
    if (!unarrError.empty()) {
      appendDebugLogLineImpl("Concurrent unarr RAR read failed: " +
                             pathForLog(archivePath) + ": " + unarrError);
    }
    if (errorMessage != nullptr) {
      *errorMessage =
          unarrError.empty()
              ? "Archive is not a non-solid random-access RAR4 archive."
              : unarrError;
    }
    return false;
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  const bool isRar5Archive =
      hasRarArchiveExtension(archivePath) &&
      rarSignature(archivePath) == RarSignature::Rar5;
  if (isRar5Archive) {
    SevenZipRar5BatchStats rar5Stats;
    std::string statsError;
    if (sevenZipRar5BatchStats(archivePath, innerPaths, rar5Stats, &statsError,
                               pauseCallback) &&
        rar5Stats.targets > 0 &&
        (rar5Stats.hasSolid ||
         rar5Stats.totalBytes <= kSevenZipRar5SingleHandleMaxTargetBytes)) {
      appendDebugLogLineImpl(
          "Using single-handle 7-Zip RAR5 batch extraction: " +
          pathForLog(archivePath) +
          " targets=" + std::to_string(rar5Stats.targets) +
          " workers=1" +
          " requestedWorkers=" + std::to_string(maxWorkers) +
          " targetBytes=" + byteCountForLog(rar5Stats.totalBytes) +
          " maxTargetBytes=" + byteCountForLog(rar5Stats.maxBytes) +
          " reason=" +
          (rar5Stats.hasSolid ? "solid-archive" : "small-targets"));
      if (readSevenZipEntriesByIndexStreaming(
              archivePath, innerPaths, std::nullopt, onFile, &sevenZipError,
              pauseCallback)) {
        appendDebugLogLineImpl(
            "Read archive batch via single-handle 7-Zip RAR5: " +
            pathForLog(archivePath) +
            " targets=" + std::to_string(rar5Stats.targets) +
            " workers=1" +
            " requestedWorkers=" + std::to_string(maxWorkers));
        return true;
      }
      appendDebugLogLineImpl("Single-handle 7-Zip RAR5 read failed: " +
                             pathForLog(archivePath) + ": " + sevenZipError);
      if (errorMessage != nullptr) {
        *errorMessage =
            sevenZipError.empty() ? "7-Zip RAR5 batch read failed."
                                  : sevenZipError;
      }
      return false;
    }
    if (!statsError.empty()) {
      appendDebugLogLineImpl("7-Zip RAR5 batch stats failed: " +
                             pathForLog(archivePath) + ": " + statsError);
    }
  }
  if (isRar5Archive &&
      readSevenZipEntriesByIndexConcurrent(
          archivePath, innerPaths, std::nullopt, onFile, maxWorkers,
          maxInFlightBytes, &sevenZipError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive batch via concurrent 7-Zip RAR5: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " requestedWorkers=" + std::to_string(maxWorkers));
    return true;
  }
  if (isRar5Archive) {
    if (!sevenZipError.empty()) {
      appendDebugLogLineImpl("Concurrent 7-Zip RAR5 read failed: " +
                             pathForLog(archivePath) + ": " + sevenZipError);
    }
    if (errorMessage != nullptr) {
      *errorMessage =
          sevenZipError.empty()
              ? "Archive is not a non-solid random-access RAR5 archive."
              : sevenZipError;
    }
    return false;
  }
#endif

  if (errorMessage != nullptr) {
    *errorMessage =
        "Archive format does not support confident parallel reading.";
  }
  return false;
}

bool readArchiveEntriesInRange(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const EntryRange &range, std::vector<FileData> &files,
    std::string *errorMessage, PauseCallback pauseCallback) {
  files.clear();
  if (range.end < range.start) {
    return true;
  }
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    appendDebugLogLineImpl("Archive ranged read unsupported: " +
                           pathForLog(archivePath));
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + pathForLog(archivePath);
    }
    appendDebugLogLineImpl("Archive ranged read unavailable: " +
                           pathForLog(archivePath));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndex(archivePath, innerPaths, range, files, &zipError,
                            pauseCallback)) {
    appendDebugLogLineImpl("Read archive range via miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP ranged read failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (hasRarArchiveExtension(archivePath) &&
      readUnarrRarEntriesByOffset(archivePath, innerPaths, range, files,
                                  &unarrError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive range via unarr RAR random access: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasRarArchiveExtension(archivePath) && !unarrError.empty()) {
    appendDebugLogLineImpl("unarr RAR ranged read failed: " +
                           pathForLog(archivePath) + ": " + unarrError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (hasSevenZipArchiveExtension(archivePath) &&
      readSevenZipEntriesByIndex(archivePath, innerPaths, range, files,
                                 &sevenZipError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive range via 7-Zip SDK: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasSevenZipArchiveExtension(archivePath) && !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip ranged read failed: " +
                           pathForLog(archivePath) + ": " + sevenZipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string cachedOrderError;
  if (readArchiveEntriesByCachedOrder(archivePath, innerPaths, range, files,
                                      &cachedOrderError, pauseCallback)) {
    appendDebugLogLineImpl("Read archive range via cached libarchive order: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (!cachedOrderError.empty()) {
    appendDebugLogLineImpl("Cached-order archive ranged read failed: " +
                           pathForLog(archivePath) + ": " + cachedOrderError);
  }
  files.clear();
  const bool read =
      readArchiveEntriesUncached(archivePath, innerPaths, range, files,
                                 errorMessage, pauseCallback);
  appendDebugLogLineImpl(
      std::string(read ? "Read archive range via libarchive scan: "
                       : "Archive ranged read failed: ") +
      pathForLog(archivePath) + " targets=" +
      std::to_string(innerPaths.size()) + " files=" +
      std::to_string(files.size()) + " range=" +
      std::to_string(range.start) + "-" + std::to_string(range.end) +
      ((!read && errorMessage != nullptr && !errorMessage->empty())
           ? ": " + *errorMessage
           : ""));
  return read;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  appendDebugLogLineImpl("Archive support is not compiled in for ranged read: " +
                         pathForLog(archivePath));
  return false;
#endif
}

std::optional<EntryRange>
entryRangeForFolder(const std::filesystem::path &folderPath) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(folderPath, archivePath, innerPath)) {
    std::error_code error;
    if (!hasSupportedArchiveExtension(folderPath) ||
        !std::filesystem::is_regular_file(folderPath, error) || error) {
      return std::nullopt;
    }
    archivePath = folderPath;
    innerPath.clear();
  }

  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr || index->entries.empty()) {
    return std::nullopt;
  }
  if (innerPath.empty()) {
    return EntryRange{.start = index->entries.front().order,
                      .end = index->entries.back().order};
  }

  EntryRange range;
  bool found = false;
  for (const Entry &entry : index->entries) {
    if (!pathIsInsideFolder(entry.path, innerPath)) {
      continue;
    }
    if (!found) {
      range.start = entry.order;
      range.end = entry.order;
      found = true;
      continue;
    }
    range.start = std::min(range.start, entry.order);
    range.end = std::max(range.end, entry.order);
  }
  if (!found) {
    return std::nullopt;
  }
  return range;
}

bool exists(const std::filesystem::path &path) {
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string errorMessage;
    return ExistsAndroidTreeFile(path, errorMessage);
  }
#endif
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
  }
  return resolveInnerPath(archivePath, innerPath).has_value();
}

bool isInSolidArchiveFolder(const std::filesystem::path &path) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return false;
  }

  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr || index->entries.empty()) {
    return false;
  }

  const std::filesystem::path virtualFolder =
      makeVirtualPath(archivePath, innerPath.parent_path());
  const auto range = entryRangeForFolder(virtualFolder);
  if (!range.has_value()) {
    if (const Entry *entry = findIndexedEntry(*index, innerPath)) {
      return entry->solid;
    }
    return false;
  }

  for (const Entry &entry : index->entries) {
    if (entry.order < range->start || entry.order > range->end) {
      continue;
    }
    if (!entry.directory && entry.solid) {
      return true;
    }
  }
  return false;
}

SourcePreference sourcePreferenceForPath(const std::filesystem::path &path) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return {};
  }

  std::uintmax_t archiveSize = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, archiveSize, mtime)) {
    return {.priority = 3, .archiveSize = 0};
  }

  SourcePreference preference{
      .priority = 1,
      .archiveSize = static_cast<std::uint64_t>(
          std::min<std::uintmax_t>(
              archiveSize, std::numeric_limits<std::uint64_t>::max())),
  };

  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr || index->entries.empty()) {
    preference.priority = 3;
    return preference;
  }

  const std::filesystem::path virtualFolder =
      makeVirtualPath(archivePath, innerPath.parent_path());
  const auto range = entryRangeForFolder(virtualFolder);
  if (!range.has_value()) {
    if (const Entry *entry = findIndexedEntry(*index, innerPath)) {
      preference.priority = entry->solid ? 2 : 1;
      return preference;
    }
    preference.priority = 3;
    return preference;
  }

  for (const Entry &entry : index->entries) {
    if (entry.order < range->start || entry.order > range->end) {
      continue;
    }
    if (!entry.directory && entry.solid) {
      preference.priority = 2;
      return preference;
    }
  }
  return preference;
}

bool readFile(const std::filesystem::path &path,
              std::vector<unsigned char> &bytes, std::string *errorMessage) {
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string androidError;
    if (!ReadAndroidTreeFile(path, bytes, androidError)) {
      if (errorMessage != nullptr) {
        *errorMessage = androidError;
      }
      return false;
    }
    return true;
  }
#endif
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return readRegularFile(path, bytes, errorMessage);
  }
  if (isSystemEntryPath(innerPath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry is system metadata: " +
                      innerPath.generic_string();
    }
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  if (hasZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readZipEntriesByName(archivePath, {innerPath}, files, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif

  const auto resolvedInner = resolveInnerPath(archivePath, innerPath);
  if (!resolvedInner.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry not found: " + innerPath.generic_string();
    }
    appendDebugLogLineImpl("Archive entry not found: " +
                           pathForLog(makeVirtualPath(archivePath,
                                                      innerPath)));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  if (hasZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readZipEntriesByIndex(archivePath, {*resolvedInner}, std::nullopt,
                              files, nullptr, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  if (hasRarArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readUnarrRarEntriesByOffset(archivePath, {*resolvedInner}, std::nullopt,
                                    files, nullptr, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  if (hasSevenZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readSevenZipEntriesByIndex(archivePath, {*resolvedInner}, std::nullopt,
                                   files, nullptr, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  if (const auto range =
          entryRangeForFolder(makeVirtualPath(archivePath,
                                              resolvedInner->parent_path()))) {
    std::vector<FileData> files;
    std::string rangeError;
    if (readArchiveEntriesInRange(archivePath, {*resolvedInner}, *range, files,
                                  &rangeError) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }

  return readArchiveEntry(archivePath, *resolvedInner, bytes, errorMessage);
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  return false;
#endif
}

bool readFileBounded(const std::filesystem::path &path,
                     std::vector<unsigned char> &bytes,
                     std::size_t maximumBytes, std::string *errorMessage,
                     std::stop_token stop) {
  bytes.clear();
  if (stop.stop_requested()) return false;
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return readRegularFileBounded(path, bytes, maximumBytes, errorMessage,
                                  stop);
  }
  if (isSystemEntryPath(innerPath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry is system metadata: " +
                      innerPath.generic_string();
    }
    return false;
  }
  const auto index = cachedIndexForArchive(
      archivePath, errorMessage, [stop] { return !stop.stop_requested(); });
  if (stop.stop_requested()) return false;
  if (index == nullptr) return false;
  const Entry *entry = findIndexedEntry(*index, innerPath);
  if (entry == nullptr || entry->directory) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry not found: " + innerPath.generic_string();
    }
    return false;
  }
  if (entry->size > maximumBytes) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry exceeds bounded read limit: " +
                      entry->path.generic_string();
    }
    return false;
  }
  // Read through the offset-based random-access batch reader (miniz ZIP seek,
  // unarr RAR4, or 7-Zip index) so a single entry is decompressed without
  // streaming through the whole archive. The libarchive fallback below stays
  // only for formats the fast backends cannot index.
#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  // The ZIP backend is fed through a chunked streaming decompress so the bound
  // is enforced as data is produced, not after a lying central directory has
  // already forced a full oversized allocation.
  if (hasZipArchiveExtension(archivePath)) {
    std::string zipError;
    bool zipOversize = false;
    if (readZipEntryBounded(archivePath, *entry, bytes, maximumBytes, &zipError,
                            [stop] { return !stop.stop_requested(); },
                            &zipOversize)) {
      return true;
    }
    if (zipOversize || archiveReadCancelled(zipError)) {
      if (errorMessage != nullptr) {
        *errorMessage = zipError;
      }
      bytes.clear();
      return false;
    }
    bytes.clear();
  }
#endif
  std::vector<FileData> files;
  std::string batchError;
  if (readArchiveEntries(archivePath, {entry->path}, files, &batchError,
                         [stop] { return !stop.stop_requested(); }) &&
      files.size() == 1) {
    if (files.front().bytes.size() > maximumBytes) {
      // The RAR/7-Zip indexed readers size their output to the declared entry
      // size, which a lying header can under-declare and then decompress
      // larger. Fail cleanly instead of falling through to a fallback read
      // that could hand the truncated buffer straight back.
      bytes.clear();
      if (errorMessage != nullptr) {
        *errorMessage = "Archive entry exceeds bounded read limit: " +
                        entry->path.generic_string();
      }
      return false;
    }
    bytes = std::move(files.front().bytes);
    return true;
  }
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  return readArchiveEntry(archivePath, entry->path, bytes, errorMessage,
                          [stop] { return !stop.stop_requested(); },
                          maximumBytes);
#else
  // The indexed size remains a pre-extraction bound for backends unavailable
  // to libarchive; the platform build used by ImageView includes libarchive.
  if (!readFile(path, bytes, errorMessage) || bytes.size() > maximumBytes) {
    bytes.clear();
    if (errorMessage != nullptr && errorMessage->empty()) {
      *errorMessage = "Archive entry exceeds bounded read limit.";
    }
    return false;
  }
  return true;
#endif
}

std::string cacheKeyForPath(const std::filesystem::path &path) {
  const std::filesystem::path normalized = path.lexically_normal();
  std::string key = cachePathKey(normalized);

  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (splitVirtualPath(path, archivePath, innerPath)) {
    key += "|archive:";
    key += archiveKey(archivePath);
    key += '|';
    key += fileStateKey(archivePath);
    return key;
  }

  key += "|file:";
  key += fileStateKey(normalized);
  return key;
}

std::optional<std::filesystem::path>
unzipVirtualFolderForChart(const std::filesystem::path &chartPath,
                           const std::filesystem::path &destinationRoot,
                           std::string *errorMessage,
                           const std::stop_token *stopToken,
                           UnzipProgressCallback progressCallback) {
  reportUnzipProgress(progressCallback, 0.02, 0, 0, "Preparing unzip");
  std::filesystem::path archivePath;
  std::filesystem::path chartInnerPath;
  if (!splitVirtualPath(chartPath, archivePath, chartInnerPath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Selected chart is not inside an archive.";
    }
    return std::nullopt;
  }
  if (stopRequested(stopToken)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Unzip cancelled";
    }
    return std::nullopt;
  }

  const std::filesystem::path folderInnerPath = chartInnerPath.parent_path();
  const std::filesystem::path virtualFolder =
      makeVirtualPath(archivePath, folderInnerPath);
  reportUnzipProgress(progressCallback, 0.04, 0, 0,
                      "Reading archive index");
  const auto range = entryRangeForFolder(virtualFolder);
  if (!range.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not locate archive folder for selected chart.";
    }
    return std::nullopt;
  }

  const auto index = cachedIndexForArchive(archivePath, errorMessage);
  if (index == nullptr) {
    return std::nullopt;
  }
  if (stopRequested(stopToken)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Unzip cancelled";
    }
    return std::nullopt;
  }

  const Entry *chartEntry = findIndexedEntry(*index, chartInnerPath);
  if (chartEntry == nullptr || chartEntry->directory) {
    if (errorMessage != nullptr) {
      *errorMessage = "Selected chart was not found in the archive.";
    }
    return std::nullopt;
  }

  std::vector<std::filesystem::path> innerPaths;
  innerPaths.reserve(range->end - range->start + 1);
  for (const Entry &entry : index->entries) {
    if (entry.order < range->start || entry.order > range->end ||
        entry.directory) {
      continue;
    }
    if (!folderInnerPath.empty() &&
        !pathIsInsideFolder(entry.path, folderInnerPath)) {
      continue;
    }
    innerPaths.push_back(entry.path);
  }

  if (innerPaths.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive folder does not contain files to unzip.";
    }
    return std::nullopt;
  }
  reportUnzipProgress(progressCallback, 0.08, 0, innerPaths.size(),
                      "Preparing output folder");

  std::error_code error;
  if (!createDirectoriesForUnzip(destinationRoot,
                                 "Could not create unzip folder", errorMessage,
                                 error)) {
    return std::nullopt;
  }

  const std::string key = cacheKeyForPath(virtualFolder);

  auto relativePathForEntry =
      [&folderInnerPath](const std::filesystem::path &entryPath)
      -> std::optional<std::filesystem::path> {
    const std::string entryName = normalizeEntryName(entryPath.generic_string());
    const std::string folderName =
        normalizeEntryName(folderInnerPath.generic_string());
    std::string relative = entryName;
    if (!folderName.empty()) {
      if (entryName.size() <= folderName.size() ||
          entryName.compare(0, folderName.size(), folderName) != 0 ||
          entryName[folderName.size()] != '/') {
        return std::nullopt;
      }
      relative = entryName.substr(folderName.size() + 1);
    }

    std::filesystem::path safePath;
    if (!safeEntryPath(relative, safePath)) {
      return std::nullopt;
    }
    return safePath;
  };

  const auto chartRelative = relativePathForEntry(chartEntry->path);
  if (!chartRelative.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Selected chart path is not safe to unzip.";
    }
    return std::nullopt;
  }

  const std::string baseName =
      readableUnzipFolderName(archivePath, folderInnerPath);
  std::filesystem::path outputFolder;
  std::filesystem::path outputChartPath;
  std::filesystem::path markerPath;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const std::string folderName =
        attempt == 0 ? baseName : baseName + " " + std::to_string(attempt + 1);
    const std::filesystem::path candidate = destinationRoot / folderName;
    const std::filesystem::path candidateMarker =
        candidate / ".asobmashow_unzip_complete";
    const std::filesystem::path candidateChart = candidate / *chartRelative;

    bool candidateExists = false;
    if (!pathExistsForUnzip(candidate, "Could not check unzip output folder",
                            candidateExists, errorMessage, error)) {
      return std::nullopt;
    }
    if (!candidateExists) {
      outputFolder = candidate;
      outputChartPath = candidateChart;
      markerPath = candidateMarker;
      break;
    }

    if (!unzipMarkerMatches(candidateMarker, key)) {
      continue;
    }
    bool candidateChartExists = false;
    if (!pathExistsForUnzip(candidateChart, "Could not check unzipped chart",
                            candidateChartExists, errorMessage, error)) {
      return std::nullopt;
    }
    if (candidateChartExists) {
      reportUnzipProgress(progressCallback, 1.0, innerPaths.size(),
                          innerPaths.size(), "Using existing unzipped folder");
      appendDebugLogLineImpl("Using existing unzipped archive folder: " +
                             pathForLog(candidate));
      return candidateChart;
    }
  }

  if (outputFolder.empty()) {
    outputFolder = destinationRoot / (baseName + " " + hex64(fnv1a64(key)));
    outputChartPath = outputFolder / *chartRelative;
    markerPath = outputFolder / ".asobmashow_unzip_complete";
  }
  error.clear();
  std::filesystem::remove_all(outputFolder, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not replace incomplete unzip folder: " +
                      error.message();
    }
    return std::nullopt;
  }
  if (!createDirectoriesForUnzip(outputFolder,
                                 "Could not create unzip output folder",
                                 errorMessage, error)) {
    return std::nullopt;
  }

  appendDebugLogLineImpl("Unzipping archive folder: " +
                         pathForLog(virtualFolder) + " files=" +
                         std::to_string(innerPaths.size()) + " output=" +
                         pathForLog(outputFolder));
  if (stopRequested(stopToken)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Unzip cancelled";
    }
    return std::nullopt;
  }
  reportUnzipProgress(progressCallback, 0.12, 0, innerPaths.size(),
                      "Reading archive files");
  std::vector<FileData> files;
  if (!readArchiveEntriesInRange(archivePath, innerPaths, *range, files,
                                 errorMessage)) {
    return std::nullopt;
  }
  if (stopRequested(stopToken)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Unzip cancelled";
    }
    return std::nullopt;
  }

  std::uint64_t writtenCount = 0;
  for (const FileData &file : files) {
    if (stopRequested(stopToken)) {
      if (errorMessage != nullptr) {
        *errorMessage = "Unzip cancelled";
      }
      return std::nullopt;
    }
    reportUnzipProgress(progressCallback,
                        0.35 + 0.6 * (static_cast<double>(writtenCount) /
                                      std::max<std::size_t>(files.size(), 1)),
                        writtenCount, files.size(), "Writing unzipped files");
    const auto relative = relativePathForEntry(file.path);
    if (!relative.has_value()) {
      continue;
    }
    const std::filesystem::path outputPath = outputFolder / *relative;
    if (!createDirectoriesForUnzip(outputPath.parent_path(),
                                   "Could not create unzip subfolder",
                                   errorMessage, error)) {
      return std::nullopt;
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not write unzipped file: " +
                        pathForLog(outputPath);
      }
      return std::nullopt;
    }
    if (!file.bytes.empty()) {
      output.write(reinterpret_cast<const char *>(file.bytes.data()),
                   static_cast<std::streamsize>(file.bytes.size()));
    }
    if (!output) {
      if (errorMessage != nullptr) {
        *errorMessage = "Failed while writing unzipped file: " +
                        pathForLog(outputPath);
      }
      return std::nullopt;
    }
    ++writtenCount;
  }
  reportUnzipProgress(progressCallback, 0.96, files.size(), files.size(),
                      "Finalizing unzip");

  std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
  if (!marker) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not finalize unzip folder: " +
                      pathForLog(markerPath);
    }
    return std::nullopt;
  }
  marker << key << '\n' << pathForLog(chartPath) << '\n';
  appendDebugLogLineImpl("Finished unzipping archive folder: " +
                         pathForLog(outputFolder) + " files=" +
                         std::to_string(files.size()));
  reportUnzipProgress(progressCallback, 1.0, files.size(), files.size(),
                      "Unzip complete");
  return outputChartPath;
}

bool extractArchiveFullyWithBatchReader(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputFolder,
    const std::vector<Entry> &entries,
    const std::stop_token *stopToken,
    const UnzipProgressCallback &progressCallback,
    const PauseCallback &pauseCallback,
    std::string *errorMessage) {
  static constexpr std::size_t kMaxBatchFiles = 128;
  static constexpr std::uint64_t kMaxBatchBytes = 64ull * 1024ull * 1024ull;

  struct FileEntryRef {
    const Entry *entry = nullptr;
  };

  std::vector<FileEntryRef> filesToExtract;
  filesToExtract.reserve(entries.size());
  for (const Entry &entry : entries) {
    if (entry.directory) {
      continue;
    }
    std::filesystem::path safePath;
    if (!safeEntryPath(entry.path.generic_string(), safePath) ||
        isSystemEntryPath(safePath)) {
      continue;
    }
    filesToExtract.push_back(FileEntryRef{.entry = &entry});
  }

  if (filesToExtract.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive does not contain files to unzip.";
    }
    return false;
  }

  appendDebugLogLineImpl("Starting batched full unzip: " +
                         pathForLog(archivePath) + " output=" +
                         pathForLog(outputFolder) + " files=" +
                         std::to_string(filesToExtract.size()));

  std::uint64_t writtenCount = 0;
  std::size_t nextFile = 0;
  while (nextFile < filesToExtract.size()) {
    if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
      return false;
    }

    std::vector<std::filesystem::path> batchPaths;
    batchPaths.reserve(kMaxBatchFiles);
    const std::size_t batchStartOrder = filesToExtract[nextFile].entry->order;
    std::size_t batchEndOrder = batchStartOrder;
    std::uint64_t batchBytes = 0;
    do {
      const Entry &entry = *filesToExtract[nextFile].entry;
      batchPaths.push_back(entry.path);
      batchEndOrder = entry.order;
      const std::uint64_t remaining =
          std::numeric_limits<std::uint64_t>::max() - batchBytes;
      batchBytes += std::min(entry.size, remaining);
      ++nextFile;
    } while (nextFile < filesToExtract.size() &&
             batchPaths.size() < kMaxBatchFiles &&
             batchBytes < kMaxBatchBytes);

    std::vector<FileData> batchFiles;
    const EntryRange range{.start = batchStartOrder, .end = batchEndOrder};
    const auto keepGoing = [stopToken, pauseCallback]() {
      return !stopRequested(stopToken) && pauseIfNeeded(pauseCallback);
    };
    if (!readArchiveEntriesInRange(archivePath, batchPaths, range, batchFiles,
                                   errorMessage, keepGoing)) {
      if (errorMessage != nullptr &&
          (*errorMessage == "Operation cancelled" ||
           stopRequested(stopToken))) {
        *errorMessage = "Unzip cancelled";
      }
      return false;
    }
    if (batchFiles.empty() && !batchPaths.empty()) {
      if (errorMessage != nullptr) {
        *errorMessage = "Archive extraction read no files.";
      }
      return false;
    }

    for (const FileData &file : batchFiles) {
      if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
        return false;
      }

      std::filesystem::path relativePath;
      if (!safeEntryPath(file.path.generic_string(), relativePath) ||
          isSystemEntryPath(relativePath)) {
        continue;
      }

      std::error_code error;
      const std::filesystem::path outputPath = outputFolder / relativePath;
      if (!createDirectoriesForUnzip(outputPath.parent_path(),
                                     "Could not create unzip subfolder",
                                     errorMessage, error)) {
        return false;
      }

      std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
      if (!output) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not write unzipped file: " +
                          pathForLog(outputPath);
        }
        return false;
      }
      if (!file.bytes.empty()) {
        output.write(reinterpret_cast<const char *>(file.bytes.data()),
                     static_cast<std::streamsize>(file.bytes.size()));
      }
      if (!output) {
        if (errorMessage != nullptr) {
          *errorMessage = "Failed while writing unzipped file: " +
                          pathForLog(outputPath);
        }
        return false;
      }

      ++writtenCount;
      reportUnzipProgress(
          progressCallback,
          0.12 + 0.84 * (static_cast<double>(writtenCount) /
                         static_cast<double>(filesToExtract.size())),
          writtenCount, filesToExtract.size(), "Writing unzipped files");
    }
  }

  appendDebugLogLineImpl("Finished batched full unzip: " +
                         pathForLog(outputFolder) + " files=" +
                         std::to_string(writtenCount));
  return true;
}

std::optional<UnzipArchiveResult>
unzipArchiveFully(const std::filesystem::path &archivePath,
                  const std::filesystem::path &destinationRoot,
                  std::string *errorMessage,
                  const std::stop_token *stopToken,
                  UnzipProgressCallback progressCallback,
                  PauseCallback pauseCallback) {
  reportUnzipProgress(progressCallback, 0.02, 0, 0, "Preparing unzip");
  if (archivePath.empty() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Selected item is not a supported archive.";
    }
    return std::nullopt;
  }

  std::error_code error;
  if (!std::filesystem::is_regular_file(archivePath, error) || error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Selected archive path is not a file.";
    }
    return std::nullopt;
  }
  if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
    return std::nullopt;
  }

  reportUnzipProgress(progressCallback, 0.04, 0, 0,
                      "Reading archive index");
  const auto index =
      cachedIndexForArchive(archivePath, errorMessage, pauseCallback);
  if (index == nullptr) {
    return std::nullopt;
  }

  std::uint64_t fileCount = 0;
  std::uint64_t uncompressedSize = 0;
  for (const Entry &entry : index->entries) {
    if (entry.directory) {
      continue;
    }
    ++fileCount;
    const std::uint64_t remaining =
        std::numeric_limits<std::uint64_t>::max() - uncompressedSize;
    uncompressedSize += std::min(entry.size, remaining);
  }
  if (fileCount == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive does not contain files to unzip.";
    }
    return std::nullopt;
  }

  reportUnzipProgress(progressCallback, 0.06, 0, fileCount,
                      "Preparing output folder");
  if (!createDirectoriesForUnzip(destinationRoot,
                                 "Could not create unzip folder", errorMessage,
                                 error)) {
    return std::nullopt;
  }

  const std::string key = cacheKeyForPath(archivePath);
  const std::string baseName = readableUnzipFolderName(archivePath, {});
  std::filesystem::path outputFolder;
  std::filesystem::path markerPath;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const std::string folderName =
        attempt == 0 ? baseName : baseName + " " + std::to_string(attempt + 1);
    const std::filesystem::path candidate = destinationRoot / folderName;
    const std::filesystem::path candidateMarker =
        candidate / ".asobmashow_unzip_complete";

    bool candidateExists = false;
    if (!pathExistsForUnzip(candidate, "Could not check unzip output folder",
                            candidateExists, errorMessage, error)) {
      return std::nullopt;
    }
    if (!candidateExists) {
      outputFolder = candidate;
      markerPath = candidateMarker;
      break;
    }
    if (unzipMarkerMatches(candidateMarker, key)) {
      reportUnzipProgress(progressCallback, 1.0, fileCount, fileCount,
                          "Using existing unzipped folder");
      appendDebugLogLineImpl("Using existing full unzip folder: " +
                             pathForLog(candidate));
      return UnzipArchiveResult{.outputFolder = candidate,
                                .fileCount = fileCount,
                                .uncompressedSize = uncompressedSize};
    }
  }

  if (outputFolder.empty()) {
    outputFolder = destinationRoot / (baseName + " " + hex64(fnv1a64(key)));
    markerPath = outputFolder / ".asobmashow_unzip_complete";
  }
  error.clear();
  std::filesystem::remove_all(outputFolder, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not replace incomplete unzip folder: " +
                      error.message();
    }
    return std::nullopt;
  }
  if (!createDirectoriesForUnzip(outputFolder,
                                 "Could not create unzip output folder",
                                 errorMessage, error)) {
    return std::nullopt;
  }
  if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
    return std::nullopt;
  }

  appendDebugLogLineImpl("Full unzip requested: " + pathForLog(archivePath) +
                         " output=" + pathForLog(outputFolder) +
                         " files=" + std::to_string(fileCount) +
                         " estimatedUnpacked=" +
                         byteCountForLog(uncompressedSize));
  bool extracted = false;
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  if (index->backend == ArchiveIndexBackend::SevenZip) {
    extracted = extractSevenZipArchiveFully(archivePath, outputFolder, index,
                                            stopToken, progressCallback,
                                            pauseCallback, errorMessage);
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  if (!extracted && !stopRequested(stopToken) &&
      index->backend == ArchiveIndexBackend::LibArchive) {
    extracted = extractArchiveFullyWithLibarchive(
        archivePath, outputFolder, index, stopToken, progressCallback,
        pauseCallback, errorMessage);
  }
#endif
  if (!extracted && !stopRequested(stopToken)) {
    extracted = extractArchiveFullyWithBatchReader(
        archivePath, outputFolder, index->entries, stopToken, progressCallback,
        pauseCallback, errorMessage);
  }
  if (!extracted) {
    if (errorMessage != nullptr && errorMessage->empty()) {
      *errorMessage = stopRequested(stopToken)
                          ? "Unzip cancelled"
                          : "Full unzip is not available for this archive.";
    }
    return std::nullopt;
  }
  if (!unzipCheckpoint(stopToken, pauseCallback, errorMessage)) {
    return std::nullopt;
  }

  reportUnzipProgress(progressCallback, 0.98, fileCount, fileCount,
                      "Finalizing unzip");
  std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
  if (!marker) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not finalize unzip folder: " +
                      pathForLog(markerPath);
    }
    return std::nullopt;
  }
  marker << key << '\n' << pathForLog(archivePath) << '\n';
  reportUnzipProgress(progressCallback, 1.0, fileCount, fileCount,
                      "Unzip complete");
  appendDebugLogLineImpl("Finished full unzip: " + pathForLog(outputFolder) +
                         " files=" + std::to_string(fileCount));
  return UnzipArchiveResult{.outputFolder = outputFolder,
                            .fileCount = fileCount,
                            .uncompressedSize = uncompressedSize};
}

std::optional<std::filesystem::path>
findFileWithExtensions(const std::filesystem::path &basePath,
                       const std::vector<std::string_view> &extensions) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(basePath, archivePath, innerPath)) {
    if (archive_file::exists(basePath)) {
      return basePath;
    }
    for (std::string_view ext : extensions) {
      std::filesystem::path candidate = basePath;
      candidate.replace_extension(std::string(ext));
      if (archive_file::exists(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  if (const auto resolved = resolveInnerPath(archivePath, innerPath)) {
    return makeVirtualPath(archivePath, *resolved);
  }
  for (std::string_view ext : extensions) {
    std::filesystem::path candidateInner = innerPath;
    candidateInner.replace_extension(std::string(ext));
    if (const auto resolved = resolveInnerPath(archivePath, candidateInner)) {
      return makeVirtualPath(archivePath, *resolved);
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path>
materializeFile(const std::filesystem::path &path, std::string *errorMessage,
                const std::atomic_bool *cancelled) {
  if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Materialize cancelled.";
    }
    return std::nullopt;
  }
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Android SAF files are not copied into the temporary "
                      "cache. Use the Android file descriptor bridge instead.";
    }
    return std::nullopt;
  }
#endif
  if (!isVirtualPath(path)) {
    return path;
  }

  std::vector<unsigned char> bytes;
  if (!readFile(path, bytes, errorMessage)) {
    return std::nullopt;
  }
  return materializeFileBytes(path, bytes, errorMessage, cancelled);
}

std::optional<std::filesystem::path>
materializeFileBytes(const std::filesystem::path &path,
                     const std::vector<unsigned char> &bytes,
                     std::string *errorMessage,
                     const std::atomic_bool *cancelled) {
  if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Materialize cancelled.";
    }
    return std::nullopt;
  }

  std::filesystem::path cacheRoot = archiveCacheRoot();
  std::lock_guard<std::mutex> lock(gTemporaryCacheMutex);
  std::error_code error;
  std::filesystem::create_directories(cacheRoot, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not create archive cache: " + error.message();
    }
    return std::nullopt;
  }

  std::filesystem::path output = materializedFileCachePath(path);
  bool needsWrite = true;
  const bool outputExists = std::filesystem::exists(output, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check cached archive entry: " +
                      error.message();
    }
    return std::nullopt;
  }
  if (outputExists) {
    const std::uintmax_t size = std::filesystem::file_size(output, error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not read cached archive entry size: " +
                        error.message();
      }
      return std::nullopt;
    }
    needsWrite = size != bytes.size();
  }
  if (needsWrite) {
    if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
      if (errorMessage != nullptr) {
        *errorMessage = "Materialize cancelled.";
      }
      return std::nullopt;
    }

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not create cached archive entry: " +
                        pathForLog(output);
      }
      return std::nullopt;
    }
    if (!bytes.empty()) {
      constexpr std::size_t kMaterializeWriteChunkBytes = 1024 * 1024;
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        if (cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed)) {
          if (errorMessage != nullptr) {
            *errorMessage = "Materialize cancelled.";
          }
          file.close();
          std::filesystem::remove(output, error);
          return std::nullopt;
        }
        const std::size_t chunkBytes =
            std::min(kMaterializeWriteChunkBytes, bytes.size() - offset);
        file.write(reinterpret_cast<const char *>(bytes.data() + offset),
                   static_cast<std::streamsize>(chunkBytes));
        if (!file) {
          break;
        }
        offset += chunkBytes;
      }
    }
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not write cached archive entry: " +
                        pathForLog(output);
      }
      return std::nullopt;
    }
  }
  return output;
}

std::filesystem::path
materializedFileCachePath(const std::filesystem::path &path) {
  const std::string key = cacheKeyForPath(path);
  return archiveCacheRoot() /
         (hex64(fnv1a64(key)) + path.extension().string());
}

bool cleanupTemporaryCache(TemporaryCacheCleanupResult &result,
                           const std::vector<std::filesystem::path>
                               &protectedPaths,
                           std::string *errorMessage) {
  result = {};
  result.path = archiveCacheRoot();

  std::lock_guard<std::mutex> lock(gTemporaryCacheMutex);
  std::error_code error;
  const bool exists = std::filesystem::exists(result.path, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check archive cache: " + error.message();
    }
    return false;
  }
  if (!exists) {
    return true;
  }

  result.cacheExisted = true;
  std::unordered_set<std::string> protectedKeys;
  protectedKeys.reserve(protectedPaths.size());
  for (const auto &protectedPath : protectedPaths) {
    if (!protectedPath.empty()) {
      protectedKeys.insert(cachePathKey(protectedPath));
    }
  }

  std::vector<std::filesystem::path> cacheEntries;
  std::filesystem::directory_iterator it(
      result.path, std::filesystem::directory_options::skip_permission_denied,
      error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read archive cache: " + error.message();
    }
    return false;
  }

  const std::filesystem::directory_iterator end;
  while (it != end) {
    cacheEntries.push_back(it->path());
    it.increment(error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not scan archive cache: " + error.message();
      }
      return false;
    }
  }

  for (const auto &entryPath : cacheEntries) {
    if (protectedKeys.contains(cachePathKey(entryPath))) {
      ++result.skippedEntries;
      continue;
    }

    const std::uint64_t entryBytes = directoryByteSize(entryPath);
    const std::uintmax_t removedEntries =
        std::filesystem::remove_all(entryPath, error);
    if (error) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not remove archive cache entry: " +
                        error.message();
      }
      return false;
    }
    result.removedBytes += entryBytes;
    result.removedEntries += clampFileSizeForResult(removedEntries);
  }

  error.clear();
  std::filesystem::remove(result.path, error);
  if (error && error != std::errc::directory_not_empty) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not remove empty archive cache folder: " +
                      error.message();
    }
    return false;
  }
  appendDebugLogLineImpl("Cleaned archive temporary cache: " +
                         pathForLog(result.path) +
                         " entries=" +
                         std::to_string(result.removedEntries) +
                         " skipped=" +
                         std::to_string(result.skippedEntries) +
                         " bytes=" +
                         std::to_string(result.removedBytes));
  return true;
}

bool measureTemporaryCache(TemporaryCacheUsageResult &result,
                           std::string *errorMessage,
                           const std::stop_token *stopToken) {
  result = {};
  result.path = archiveCacheRoot();

  std::error_code error;
  const bool exists = std::filesystem::exists(result.path, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not check archive cache: " + error.message();
    }
    return false;
  }
  if (!exists) {
    return true;
  }

  result.cacheExisted = true;
  if (!directoryStats(result.path, result.bytes, result.entries, stopToken)) {
    if (stopRequested(stopToken)) {
      if (errorMessage != nullptr) {
        *errorMessage = "Archive cache measurement cancelled.";
      }
      return false;
    }
    if (errorMessage != nullptr) {
      *errorMessage = "Could not measure archive cache.";
    }
    return false;
  }
  return true;
}

void parseChart(bms_parser::Parser &parser, const std::filesystem::path &path,
                bms_parser::Chart **chart, bool addReadyMeasure,
                bool metaOnly, std::atomic_bool &cancelled) {
  if (chart == nullptr) {
    return;
  }
  *chart = nullptr;

#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    appendDebugLogLineImpl("Reading Android SAF chart: " + pathForLog(path));
    if (!readFile(path, bytes, &errorMessage)) {
      SDL_Log("Failed to read Android SAF chart %s: %s",
              pathForLog(path).c_str(), errorMessage.c_str());
      appendDebugLogLineImpl("Failed to read Android SAF chart: " +
                             pathForLog(path) + ": " + errorMessage);
      return;
    }
    parser.Parse(bytes, chart, addReadyMeasure, metaOnly, cancelled);
    if (*chart != nullptr) {
      (*chart)->Meta.BmsPath = path;
      (*chart)->Meta.Folder = path.parent_path();
      appendDebugLogLineImpl("Parsed Android SAF chart: " + pathForLog(path) +
                             " measures=" +
                             std::to_string((*chart)->Measures.size()));
    }
    return;
  }
#endif

  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    parser.Parse(path, chart, addReadyMeasure, metaOnly, cancelled);
    return;
  }

  std::vector<unsigned char> bytes;
  std::string errorMessage;
  appendDebugLogLineImpl("Reading archive chart: " + pathForLog(path));
  if (!readFile(path, bytes, &errorMessage)) {
    SDL_Log("Failed to read chart from archive %s: %s",
            pathForLog(path).c_str(), errorMessage.c_str());
    appendDebugLogLineImpl("Failed to read archive chart: " +
                           pathForLog(path) + ": " + errorMessage);
    return;
  }

  parser.Parse(bytes, chart, addReadyMeasure, metaOnly, cancelled);
  if (*chart != nullptr) {
    (*chart)->Meta.BmsPath = path;
    (*chart)->Meta.Folder =
        makeVirtualPath(archivePath, innerPath.parent_path());
    appendDebugLogLineImpl("Parsed archive chart: " + pathForLog(path) +
                           " measures=" +
                           std::to_string((*chart)->Measures.size()));
  } else if (cancelled.load()) {
    appendDebugLogLineImpl("Archive chart parse cancelled: " +
                           pathForLog(path));
  } else {
    appendDebugLogLineImpl("Archive chart parser returned null: " +
                           pathForLog(path));
  }
}

} // namespace archive_file
