#include "Internal.h"

#include "../BmsChartFile.h"
#include "../CanonicalDigest.h"

#if ASOBMSHOW_HAS_LIBARCHIVE
#include "../ArchiveRAII.h"
#endif

namespace asobmshow::bms_search {

bool safeArchivePath(const std::string &name, std::filesystem::path &outPath) {
  if (name.empty() || name.find('\0') != std::string::npos) {
    return false;
  }
  std::string normalized = replaceAll(name, "\\", "/");
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

bool ensureArchiveOutputDirectory(const std::filesystem::path &path,
                                  const char *failurePrefix,
                                  std::string &errorMessage) {
  std::error_code fsError;
  std::filesystem::create_directories(path, fsError);
  if (fsError) {
    errorMessage = std::string(failurePrefix) + ": " + fsError.message();
    return false;
  }
  return true;
}

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

#if ASOBMSHOW_HAS_ICONV
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

#if ASOBMSHOW_HAS_LIBARCHIVE
std::string archiveEntryPathnameUtf8(archive_entry *entry) {
  if (entry == nullptr) {
    return "";
  }
  if (const char *utf8Name = archive_entry_pathname_utf8(entry);
      utf8Name != nullptr && utf8Name[0] != '\0') {
    return utf8Name;
  }
  if (const wchar_t *wideName = archive_entry_pathname_w(entry);
      wideName != nullptr && wideName[0] != L'\0') {
    return ws2s_utf8(wideName);
  }

  const char *rawName = archive_entry_pathname(entry);
  if (rawName == nullptr || rawName[0] == '\0') {
    return "";
  }

  const std::string raw(rawName);
  if (isValidUtf8(raw)) {
    return raw;
  }

#if ASOBMSHOW_HAS_ICONV
  for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (const auto converted = convertTextToUtf8(raw, encoding)) {
      return *converted;
    }
  }
#endif
  return raw;
}
#endif

#if !ASOBMSHOW_HAS_LIBARCHIVE
bool hasZipSignature(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::array<unsigned char, 4> bytes{};
  file.read(reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  if (file.gcount() < 4) {
    return false;
  }
  return bytes == std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04} ||
         bytes == std::array<unsigned char, 4>{'P', 'K', 0x05, 0x06} ||
         bytes == std::array<unsigned char, 4>{'P', 'K', 0x07, 0x08};
}

bool extractZipArchive(const std::filesystem::path &archivePath,
                       const std::filesystem::path &outputPath,
                       std::string &errorMessage,
                       BmsSearchDownloadProgressCallback progressCallback) {
  if (!ensureArchiveOutputDirectory(
          outputPath, "Could not create output folder", errorMessage)) {
    return false;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = fspath_to_utf8(archivePath);
  if (!mz_zip_reader_init_file(&archive, archiveText.c_str(), 0)) {
    errorMessage = "Could not open ZIP archive.";
    return false;
  }

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  int extractedFiles = 0;
  bool ok = true;
  for (mz_uint i = 0; i < fileCount; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
      ok = false;
      errorMessage = "Could not read ZIP directory entry.";
      break;
    }

    std::filesystem::path relativePath;
    if (!safeArchivePath(stat.m_filename, relativePath)) {
      continue;
    }
    const std::filesystem::path destination = outputPath / relativePath;
    if (mz_zip_reader_is_file_a_directory(&archive, i)) {
      if (!ensureArchiveOutputDirectory(
              destination, "Could not create archive folder", errorMessage)) {
        ok = false;
        break;
      }
      continue;
    }

    if (!ensureArchiveOutputDirectory(
            destination.parent_path(), "Could not create archive folder",
            errorMessage)) {
      ok = false;
      break;
    }

    if (progressCallback) {
      progressCallback({.message = "Extracting " + fspath_to_utf8(relativePath),
                        .downloadedBytes = i,
                        .totalBytes = fileCount});
    }

    const std::string destinationText = fspath_to_utf8(destination);
    if (!mz_zip_reader_extract_to_file(&archive, i, destinationText.c_str(),
                                       0)) {
      ok = false;
      errorMessage = "Could not extract " + fspath_to_utf8(relativePath);
      break;
    }
    ++extractedFiles;
  }
  mz_zip_reader_end(&archive);

  if (!ok) {
    return false;
  }
  if (extractedFiles == 0) {
    errorMessage = "ZIP archive did not contain extractable files.";
    return false;
  }
  return true;
}
#endif

#if ASOBMSHOW_HAS_LIBARCHIVE
bool localeNameLooksUtf8(const char *localeName) {
  if (localeName == nullptr || localeName[0] == '\0') {
    return false;
  }
  const std::string lower = lowerCopy(localeName);
  return lower.find("utf-8") != std::string::npos ||
         lower.find("utf8") != std::string::npos;
}

bool localeNameLooksShiftJis(const char *localeName) {
  if (localeName == nullptr || localeName[0] == '\0') {
    return false;
  }
  const std::string lower = lowerCopy(localeName);
  return lower.find("shift") != std::string::npos ||
         lower.find("sjis") != std::string::npos ||
         lower.find("cp932") != std::string::npos ||
         lower.find("ms_kanji") != std::string::npos;
}

bool localeNameLooksArchiveCompatible(const char *localeName) {
  return localeNameLooksUtf8(localeName) ||
         localeNameLooksShiftJis(localeName);
}

void ensureArchiveFilenameLocale() {
  static std::once_flag localeInitFlag;
  std::call_once(localeInitFlag, []() {
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

    SDL_Log("Could not set UTF-8 or Shift-JIS LC_CTYPE locale for archive "
            "filenames.");
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
  for (const char *format : {"zip", "rar", "lha", "tar", "cab", "cpio"}) {
    const int status =
        archive_read_set_option(archiveHandle, format, "hdrcharset", charset);
    if (status == ARCHIVE_OK || status == ARCHIVE_WARN) {
      applied = true;
    }
  }
  return applied;
}

void preferJapaneseArchiveHeaderCharset(archive *archiveHandle) {
  for (const char *charset : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (trySetArchiveHeaderCharset(archiveHandle, charset)) {
      return;
    }
  }
}

bool extractArchiveWithLibarchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback) {
  if (!ensureArchiveOutputDirectory(
          outputPath, "Could not create output folder", errorMessage)) {
    return false;
  }

  ensureArchiveFilenameLocale();
  auto archiveHandle = makeArchiveReadHandle();
  if (archiveHandle == nullptr) {
    errorMessage = "Could not initialize archive reader.";
    return false;
  }
  archive_read_support_filter_all(archiveHandle.get());
  archive_read_support_format_all(archiveHandle.get());
  archive_read_support_format_raw(archiveHandle.get());
  preferJapaneseArchiveHeaderCharset(archiveHandle.get());

  const std::string archiveText = fspath_to_utf8(archivePath);
  int status = archive_read_open_filename(archiveHandle.get(),
                                          archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    errorMessage =
        "Could not open archive: " + archiveErrorString(archiveHandle.get(), "");
    return false;
  }

  bool ok = true;
  int extractedFiles = 0;
  int skippedInvalidPaths = 0;
  int skippedUnsupportedTypes = 0;
  int directoryEntries = 0;
  std::uint64_t entryIndex = 0;
  archive_entry *entry = nullptr;
  for (;;) {
    status = archive_read_next_header(archiveHandle.get(), &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      ok = false;
      errorMessage =
          "Could not read archive: " +
          archiveErrorString(archiveHandle.get(), "");
      break;
    }
    if (status == ARCHIVE_WARN) {
      SDL_Log("Continuing after archive warning: %s",
              archiveErrorString(archiveHandle.get(), "").c_str());
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle.get());
      continue;
    }

    ++entryIndex;
    const std::string entryName = archiveEntryPathnameUtf8(entry);
    const bool entryNameLooksDirectory =
        !entryName.empty() &&
        (entryName.back() == '/' || entryName.back() == '\\');

    std::filesystem::path relativePath;
    if (!safeArchivePath(entryName, relativePath)) {
      ++skippedInvalidPaths;
      archive_read_data_skip(archiveHandle.get());
      continue;
    }

    const std::filesystem::path destination = outputPath / relativePath;
    const auto fileType = archive_entry_filetype(entry);
    const bool fileTypeIsSet = archive_entry_filetype_is_set(entry) != 0;
    const bool unknownFileType = !fileTypeIsSet || fileType == 0;
    if (fileType == AE_IFDIR ||
        (unknownFileType && entryNameLooksDirectory)) {
      ++directoryEntries;
      if (!ensureArchiveOutputDirectory(
              destination, "Could not create archive folder", errorMessage)) {
        ok = false;
        break;
      }
      continue;
    }
    if (!unknownFileType && fileType != AE_IFREG) {
      ++skippedUnsupportedTypes;
      archive_read_data_skip(archiveHandle.get());
      continue;
    }

    if (!ensureArchiveOutputDirectory(
            destination.parent_path(), "Could not create archive folder",
            errorMessage)) {
      ok = false;
      break;
    }

    if (progressCallback) {
      progressCallback({.message = "Extracting " + fspath_to_utf8(relativePath),
                        .downloadedBytes = entryIndex,
                        .totalBytes = 0});
    }

    std::ofstream output(destination, std::ios::binary);
    if (!output) {
      ok = false;
      errorMessage = "Could not create " + fspath_to_utf8(relativePath);
      break;
    }

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
      const la_ssize_t bytes =
          archive_read_data(archiveHandle.get(), buffer.data(),
                            buffer.size());
      if (bytes == 0) {
        break;
      }
      if (bytes < 0) {
        ok = false;
        errorMessage = "Could not extract " + fspath_to_utf8(relativePath) +
                       ": " + archiveErrorString(archiveHandle.get(), "");
        break;
      }
      output.write(buffer.data(), static_cast<std::streamsize>(bytes));
      if (!output) {
        ok = false;
        errorMessage = "Could not write " + fspath_to_utf8(relativePath);
        break;
      }
    }
    if (!ok) {
      break;
    }
    ++extractedFiles;
  }

  if (!ok) {
    return false;
  }
  if (extractedFiles == 0) {
    errorMessage = "Archive did not contain extractable files.";
    if (entryIndex > 0) {
      errorMessage += " Entries=" + std::to_string(entryIndex) +
                      ", folders=" + std::to_string(directoryEntries) +
                      ", unsafe=" + std::to_string(skippedInvalidPaths) +
                      ", unsupported=" +
                      std::to_string(skippedUnsupportedTypes) + ".";
    }
    return false;
  }
  return true;
}
#endif

bool extractDownloadedArchive(
    const std::filesystem::path &archivePath,
    const std::filesystem::path &outputPath, std::string &errorMessage,
    BmsSearchDownloadProgressCallback progressCallback) {
#if ASOBMSHOW_HAS_LIBARCHIVE
  return extractArchiveWithLibarchive(archivePath, outputPath, errorMessage,
                                      progressCallback);
#else
  if (!hasZipSignature(archivePath)) {
    errorMessage =
        "Downloaded response was not a ZIP archive. Open the source manually.";
    return false;
  }
  return extractZipArchive(archivePath, outputPath, errorMessage,
                           progressCallback);
#endif
}

#if (TARGET_OS_IOS || TARGET_OS_SIMULATOR) && defined(DEBUG)
void writeArchiveEntryDiagnostics(const std::filesystem::path &archivePath,
                                  const std::filesystem::path &outputPath) {
#if ASOBMSHOW_HAS_LIBARCHIVE
  std::ofstream output(outputPath);
  if (!output) {
    return;
  }

  ensureArchiveFilenameLocale();
  auto archiveHandle = makeArchiveReadHandle();
  if (archiveHandle == nullptr) {
    output << "Could not initialize archive reader.\n";
    return;
  }
  archive_read_support_filter_all(archiveHandle.get());
  archive_read_support_format_all(archiveHandle.get());
  archive_read_support_format_raw(archiveHandle.get());
  preferJapaneseArchiveHeaderCharset(archiveHandle.get());

  const std::string archiveText = fspath_to_utf8(archivePath);
  int status = archive_read_open_filename(archiveHandle.get(),
                                          archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    output << "Could not open archive: "
           << archiveErrorString(archiveHandle.get(), "") << '\n';
    return;
  }

  std::uint64_t entryIndex = 0;
  archive_entry *entry = nullptr;
  for (;;) {
    status = archive_read_next_header(archiveHandle.get(), &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      output << "Could not read archive: "
             << archiveErrorString(archiveHandle.get(), "") << '\n';
      break;
    }
    ++entryIndex;
    output << entryIndex << '\t';
    if (entry == nullptr) {
      output << "entry=null\n";
      archive_read_data_skip(archiveHandle.get());
      continue;
    }
    output << "name=" << archiveEntryPathnameUtf8(entry)
           << "\ttype_set=" << archive_entry_filetype_is_set(entry)
           << "\ttype=" << archive_entry_filetype(entry)
           << "\tsize_set=" << archive_entry_size_is_set(entry)
           << "\tsize=" << archive_entry_size(entry) << '\n';
    archive_read_data_skip(archiveHandle.get());
  }
#else
  (void)archivePath;
  (void)outputPath;
#endif
}
#endif

bool containsBmsFile(const std::filesystem::path &root) {
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  for (const auto end = std::filesystem::recursive_directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    if (asobmshow::bms_chart_file::isBmsChartPath(iterator->path())) {
      return true;
    }
  }
  return false;
}

bool isHexStringOfLength(const std::string &value, size_t length) {
  return canonical_digest::isCanonicalLowerHex(value, length);
}

std::optional<std::vector<unsigned char>>
readFileBytes(const std::filesystem::path &path, std::string &errorMessage) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    errorMessage = "Could not read chart size: " + error.message();
    return std::nullopt;
  }
  const auto maxBufferedSize = std::min<std::uintmax_t>(
      static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max()),
      static_cast<std::uintmax_t>(
          std::numeric_limits<std::streamsize>::max()));
  if (size > maxBufferedSize) {
    errorMessage = "Chart file is too large to read for hash verification.";
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    errorMessage = "Could not open chart file for hash verification.";
    return std::nullopt;
  }

  std::vector<unsigned char> bytes(static_cast<size_t>(size));
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  if (file.gcount() != static_cast<std::streamsize>(bytes.size()) ||
      (!file && !file.eof())) {
    errorMessage = "Could not read chart file for hash verification.";
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::filesystem::path> findMatchingBmsChartByHash(
    const std::filesystem::path &root, const std::string &archiveKey,
    std::string &errorMessage) {
  const std::string key = lowerCopy(trimCopy(archiveKey));
  const bool matchSha256 =
      canonical_digest::isCanonicalLowerHex(key, 64);
  const bool matchMd5 = canonical_digest::isCanonicalLowerHex(key, 32);
  if (!matchSha256 && !matchMd5) {
    return std::nullopt;
  }

  bool sawBmsFile = false;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  for (const auto end = std::filesystem::recursive_directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    if (!asobmshow::bms_chart_file::isBmsChartPath(iterator->path())) {
      continue;
    }
    sawBmsFile = true;

    std::string readError;
    const auto bytes = readFileBytes(iterator->path(), readError);
    if (!bytes) {
      if (!readError.empty()) {
        SDL_Log("Skipping BMS hash verification for %s: %s",
                fspath_to_utf8(iterator->path()).c_str(), readError.c_str());
      }
      continue;
    }

    if (matchSha256 && lowerCopy(bms_parser::sha256(*bytes)) == key) {
      return iterator->path();
    }
    if (matchMd5) {
      const std::string text(bytes->begin(), bytes->end());
      if (lowerCopy(bms_parser::md5(text)) == key) {
        return iterator->path();
      }
    }
  }

  if (error) {
    errorMessage = "Could not scan extracted archive: " + error.message();
  } else if (sawBmsFile) {
    errorMessage = "Archive did not contain the selected BMS chart.";
  } else {
    errorMessage = "Archive did not contain a BMS chart file.";
  }
  return std::nullopt;
}

std::optional<std::string>
htmlBodyFromDownloadedFile(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > 4 * 1024 * 1024) {
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::string body(size, '\0');
  file.read(body.data(), static_cast<std::streamsize>(body.size()));
  if (file.gcount() != static_cast<std::streamsize>(body.size()) ||
      (!file && !file.eof())) {
    return std::nullopt;
  }

  const std::string leading =
      lowerCopy(trimCopy(body.substr(0, std::min<size_t>(body.size(), 512))));
  if (leading.starts_with("<!doctype html") ||
      leading.starts_with("<html") || leading.find("<html") != std::string::npos ||
      leading.find("<head") != std::string::npos ||
      leading.find("<body") != std::string::npos) {
    return body;
  }
  return std::nullopt;
}


} // namespace asobmshow::bms_search
