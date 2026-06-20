#include "ArchiveFile.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

#if __has_include(<archive.h>) && __has_include(<archive_entry.h>)
#include <archive.h>
#include <archive_entry.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 0
#endif

#if __has_include(<iconv.h>)
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
namespace {

std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
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

std::string archiveKey(const std::filesystem::path &path) {
  return path_t_to_utf8(fspath_to_path_t(path.lexically_normal()));
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

struct CachedIndex {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  std::vector<Entry> entries;
};

std::mutex gIndexMutex;
std::unordered_map<std::string, CachedIndex> gIndexCache;

bool readRegularFile(const std::filesystem::path &path,
                     std::vector<unsigned char> &bytes,
                     std::string *errorMessage) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open file: " + path.string();
    }
    return false;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size < 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read file size: " + path.string();
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
        *errorMessage = "Could not read file: " + path.string();
      }
      return false;
    }
  }
  return true;
}

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
  std::wstring wide(input);
#ifdef _WIN32
  return path_t_to_utf8(wide);
#else
  std::string output;
  output.reserve(wide.size());
  for (wchar_t ch : wide) {
    if (ch >= 0 && ch <= 0x7f) {
      output.push_back(static_cast<char>(ch));
    }
  }
  return output;
#endif
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

void configureArchiveReader(archive *archiveHandle) {
  archive_read_support_filter_all(archiveHandle);
  archive_read_support_format_zip(archiveHandle);
  for (const char *charset : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    const int status =
        archive_read_set_option(archiveHandle, "zip", "hdrcharset", charset);
    if (status == ARCHIVE_OK || status == ARCHIVE_WARN) {
      break;
    }
  }
}

archive *openArchive(const std::filesystem::path &archivePath,
                     std::string *errorMessage) {
  ensureArchiveFilenameLocale();
  archive *archiveHandle = archive_read_new();
  if (archiveHandle == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not initialize archive reader.";
    }
    return nullptr;
  }
  configureArchiveReader(archiveHandle);

  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  const int status =
      archive_read_open_filename(archiveHandle, archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage =
          "Could not open archive: " + archiveErrorString(archiveHandle, "");
    }
    archive_read_free(archiveHandle);
    return nullptr;
  }
  return archiveHandle;
}

bool listEntriesUncached(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         std::string *errorMessage) {
  entries.clear();
  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  for (;;) {
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
      archive_read_free(archiveHandle);
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

  archive_read_free(archiveHandle);
  return true;
}

bool readArchiveEntry(const std::filesystem::path &archivePath,
                      const std::filesystem::path &innerPath,
                      std::vector<unsigned char> &bytes,
                      std::string *errorMessage) {
  bytes.clear();
  const std::string target = normalizeEntryName(innerPath.generic_string());
  if (target.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry path is empty.";
    }
    return false;
  }

  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  for (;;) {
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
      archive_read_free(archiveHandle);
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

    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0) {
      bytes.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        archive_read_free(archiveHandle);
        return true;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        archive_read_free(archiveHandle);
        return false;
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
  }

  if (errorMessage != nullptr) {
    *errorMessage = "Archive entry not found: " + target;
  }
  archive_read_free(archiveHandle);
  return false;
}

bool readArchiveEntriesUncached(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
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

  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  while (!targets.empty()) {
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
      archive_read_free(archiveHandle);
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
    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0) {
      file.bytes.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    for (;;) {
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
        archive_read_free(archiveHandle);
        return false;
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    files.push_back(std::move(file));
    targets.erase(targetIt);
  }

  archive_read_free(archiveHandle);
  return true;
}

#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
struct ZipReadTarget {
  std::string normalized;
  std::filesystem::path entryPath;
  std::size_t order = 0;
};

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

bool readZipEntriesByIndex(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
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

  std::vector<Entry> entries;
  if (!listEntries(archivePath, entries, errorMessage)) {
    return false;
  }

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(targets.size());
  for (const Entry &entry : entries) {
    if (targets.empty()) {
      break;
    }
    if (range.has_value() &&
        (entry.order < range->start || entry.order > range->end)) {
      continue;
    }
    if (entry.directory) {
      continue;
    }

    const std::string normalized =
        normalizeEntryName(entry.path.generic_string());
    const auto targetIt = targets.find(normalized);
    if (targetIt == targets.end()) {
      continue;
    }

    readTargets.push_back({
        .normalized = normalized,
        .entryPath = entry.path,
        .order = entry.order,
    });
    targets.erase(targetIt);
  }
  if (readTargets.empty()) {
    return true;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
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
    if (target.order >= static_cast<std::size_t>(fileCount)) {
      return fail("ZIP index is out of range.");
    }

    const mz_uint fileIndex = static_cast<mz_uint>(target.order);
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    std::filesystem::path minizPath;
    if (!safeEntryPath(*filename, minizPath) ||
        normalizeEntryName(minizPath.generic_string()) != target.normalized) {
      return fail("ZIP central directory order did not match archive index.");
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
#endif

std::optional<std::filesystem::path>
resolveInnerPath(const std::filesystem::path &archivePath,
                 const std::filesystem::path &innerPath) {
  std::vector<Entry> entries;
  if (!listEntries(archivePath, entries)) {
    return std::nullopt;
  }

  const std::string target = normalizeEntryName(innerPath.generic_string());
  for (const auto &entry : entries) {
    if (!entry.directory &&
        normalizeEntryName(entry.path.generic_string()) == target) {
      return entry.path;
    }
  }

  const std::string lowerTarget = lowerCopy(target);
  for (const auto &entry : entries) {
    if (!entry.directory &&
        lowerCopy(normalizeEntryName(entry.path.generic_string())) ==
            lowerTarget) {
      return entry.path;
    }
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

std::filesystem::path archiveCacheRoot() {
  return std::filesystem::temp_directory_path() / "AsoBMaShowArchiveCache";
}

} // namespace

bool isArchiveSupportAvailable() {
  return ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE != 0;
}

bool hasSupportedArchiveExtension(const std::filesystem::path &path) {
  const std::string ext = lowerCopy(path.extension().string());
  return ext == ".zip" || ext == ".cbz";
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
                 std::vector<Entry> &entries, std::string *errorMessage) {
  entries.clear();
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
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return false;
  }

  const std::string key = archiveKey(archivePath);
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    const auto it = gIndexCache.find(key);
    if (it != gIndexCache.end() && it->second.size == size &&
        it->second.mtime == mtime) {
      entries = it->second.entries;
      return true;
    }
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::vector<Entry> loaded;
  if (!listEntriesUncached(archivePath, loaded, errorMessage)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    gIndexCache[key] = {.size = size, .mtime = mtime, .entries = loaded};
  }
  entries = std::move(loaded);
  return true;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  return false;
#endif
}

bool readArchiveEntries(const std::filesystem::path &archivePath,
                        const std::vector<std::filesystem::path> &innerPaths,
                        std::vector<FileData> &files,
                        std::string *errorMessage) {
  files.clear();
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
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (readZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                            &zipError)) {
    return true;
  }
  files.clear();
#endif
  return readArchiveEntriesUncached(archivePath, innerPaths, std::nullopt, files,
                                    errorMessage);
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  return false;
#endif
}

bool readArchiveEntriesInRange(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const EntryRange &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (range.end < range.start) {
    return true;
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
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (readZipEntriesByIndex(archivePath, innerPaths, range, files, &zipError)) {
    return true;
  }
  files.clear();
#endif
  return readArchiveEntriesUncached(archivePath, innerPaths, range, files,
                                    errorMessage);
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
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

  std::vector<Entry> entries;
  if (!listEntries(archivePath, entries) || entries.empty()) {
    return std::nullopt;
  }
  if (innerPath.empty()) {
    return EntryRange{.start = entries.front().order,
                      .end = entries.back().order};
  }

  EntryRange range;
  bool found = false;
  for (const Entry &entry : entries) {
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
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
  }
  return resolveInnerPath(archivePath, innerPath).has_value();
}

bool readFile(const std::filesystem::path &path,
              std::vector<unsigned char> &bytes, std::string *errorMessage) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return readRegularFile(path, bytes, errorMessage);
  }

  const auto resolvedInner = resolveInnerPath(archivePath, innerPath);
  if (!resolvedInner.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry not found: " + innerPath.generic_string();
    }
    return false;
  }

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

std::string cacheKeyForPath(const std::filesystem::path &path) {
  const std::filesystem::path normalized = path.lexically_normal();
  std::string key = path_t_to_utf8(fspath_to_path_t(normalized));

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
materializeFile(const std::filesystem::path &path, std::string *errorMessage) {
  if (!isVirtualPath(path)) {
    return path;
  }

  std::vector<unsigned char> bytes;
  if (!readFile(path, bytes, errorMessage)) {
    return std::nullopt;
  }
  return materializeFileBytes(path, bytes, errorMessage);
}

std::optional<std::filesystem::path>
materializeFileBytes(const std::filesystem::path &path,
                     const std::vector<unsigned char> &bytes,
                     std::string *errorMessage) {
  const std::string key = cacheKeyForPath(path);
  std::filesystem::path cacheRoot = archiveCacheRoot();
  std::error_code error;
  std::filesystem::create_directories(cacheRoot, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not create archive cache: " + error.message();
    }
    return std::nullopt;
  }

  std::filesystem::path output =
      cacheRoot / (hex64(fnv1a64(key)) + path.extension().string());
  bool needsWrite = true;
  if (std::filesystem::exists(output, error) && !error) {
    std::uintmax_t size = std::filesystem::file_size(output, error);
    needsWrite = error || size != bytes.size();
  }
  if (needsWrite) {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not create cached archive entry: " +
                        output.string();
      }
      return std::nullopt;
    }
    if (!bytes.empty()) {
      file.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    }
  }
  return output;
}

void parseChart(bms_parser::Parser &parser, const std::filesystem::path &path,
                bms_parser::Chart **chart, bool addReadyMeasure,
                bool metaOnly, std::atomic_bool &cancelled) {
  if (chart == nullptr) {
    return;
  }
  *chart = nullptr;

  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    parser.Parse(path, chart, addReadyMeasure, metaOnly, cancelled);
    return;
  }

  std::vector<unsigned char> bytes;
  std::string errorMessage;
  if (!readFile(path, bytes, &errorMessage)) {
    SDL_Log("Failed to read chart from archive %s: %s",
            path_t_to_utf8(fspath_to_path_t(path)).c_str(),
            errorMessage.c_str());
    return;
  }

  parser.Parse(bytes, chart, addReadyMeasure, metaOnly, cancelled);
  if (*chart != nullptr) {
    (*chart)->Meta.BmsPath = path;
    (*chart)->Meta.Folder =
        makeVirtualPath(archivePath, innerPath.parent_path());
  }
}

} // namespace archive_file
