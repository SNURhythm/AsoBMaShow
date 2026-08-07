#include "SkinArchiveImporter.h"

#include "../../FileChecksum.h"
#include "SkinPathPolicy.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <Aclapi.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

#include "SkinIOSFileOpenCompatibility.h"

namespace skin {
namespace {

namespace fs = std::filesystem;

enum class MemberKind : std::uint8_t {
  Regular,
  Directory,
};

struct ArchiveMember {
  std::string archivePath;
  std::string normalizedPath;
  std::string collisionKey;
  std::string installedPath;
  std::string installedCollisionKey;
  MemberKind kind = MemberKind::Regular;
  std::uint64_t declaredSize = 0;
  std::uint32_t expectedCrc32 = 0;
  std::uint16_t compressionMethod = 0;
  std::string streamedSha256;

  auto operator<=>(const ArchiveMember &) const = default;
};

struct RawZipMember {
  std::uint64_t uncompressedBytes = 0;
  std::uint64_t compressedBytes = 0;
  std::uint32_t crc32 = 0;
  std::uint16_t compressionMethod = 0;
};

using RawZipMembers = std::map<std::string, RawZipMember, std::less<>>;

struct ArchiveInventory {
  std::vector<ArchiveMember> members;
  std::optional<std::string> wrapper;
  std::uint64_t fileCount = 0;
  std::uint64_t totalBytes = 0;
};

std::uint16_t little16(const unsigned char *bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t little32(const unsigned char *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath = {});
void observe(const std::shared_ptr<const SkinImportIoObserver> &observer,
             SkinImportIoOperation operation, const fs::path &path);

class OwnedArchiveFile {
public:
  OwnedArchiveFile() : file_(std::tmpfile()) {}
  OwnedArchiveFile(const OwnedArchiveFile &) = delete;
  OwnedArchiveFile &operator=(const OwnedArchiveFile &) = delete;
  OwnedArchiveFile(OwnedArchiveFile &&other) noexcept
      : file_(std::exchange(other.file_, nullptr)), bytes_(other.bytes_) {}
  OwnedArchiveFile &operator=(OwnedArchiveFile &&other) noexcept {
    if (this != &other) {
      if (file_ != nullptr) {
        std::fclose(file_);
      }
      file_ = std::exchange(other.file_, nullptr);
      bytes_ = other.bytes_;
    }
    return *this;
  }
  ~OwnedArchiveFile() {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  [[nodiscard]] bool valid() const noexcept { return file_ != nullptr; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  void setBytes(std::uint64_t bytes) noexcept { bytes_ = bytes; }

  bool rewind() {
    std::clearerr(file_);
#if defined(_WIN32)
    return _fseeki64(file_, 0, SEEK_SET) == 0;
#else
    return ::fseeko(file_, 0, SEEK_SET) == 0;
#endif
  }

  bool readAt(std::uint64_t offset, std::span<unsigned char> bytes) {
    if (!rewindTo(offset)) {
      return false;
    }
    return std::fread(bytes.data(), 1, bytes.size(), file_) == bytes.size();
  }

  bool write(std::span<const char> bytes) {
    return std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size();
  }

  bool finishCopy() {
    if (std::fflush(file_) != 0) {
      return false;
    }
#if !defined(_WIN32)
    struct stat status{};
    const int descriptor = ::fileno(file_);
    if (descriptor < 0 || ::fchmod(descriptor, 0400) != 0 ||
        ::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0777) != 0400 ||
        status.st_nlink > 1) {
      return false;
    }
#endif
    return rewind();
  }
  FILE *get() const noexcept { return file_; }

private:
  bool rewindTo(std::uint64_t offset) {
    std::clearerr(file_);
#if defined(_WIN32)
    return offset <= static_cast<std::uint64_t>(INT64_MAX) &&
           _fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return offset <= static_cast<std::uint64_t>(INT64_MAX) &&
           ::fseeko(file_, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
  }

  FILE *file_ = nullptr;
  std::uint64_t bytes_ = 0;
};

std::optional<OwnedArchiveFile>
copyArchiveSource(const fs::path &path, std::stop_token stop, bool &cancelled,
                  const std::shared_ptr<const SkinImportIoObserver> &observer,
                  std::vector<SkinDiagnostic> &diagnostics) {
  OwnedArchiveFile owned;
  if (!owned.valid()) {
    diagnostics.push_back(
        diagnostic("skin_archive_owned_copy_failed",
                   "unable to allocate private storage for the skin ZIP"));
    return std::nullopt;
  }

  std::array<char, 64 * 1024> buffer{};
  std::uint64_t total = 0;
#if defined(_WIN32)
  const HANDLE source = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (source == INVALID_HANDLE_VALUE) {
    diagnostics.push_back(diagnostic(
        "skin_archive_input_invalid",
        "skin archive is missing or cannot be opened without following links"));
    return std::nullopt;
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION before{};
  LARGE_INTEGER size{};
  const bool regular =
      GetFileInformationByHandleEx(source, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      (tag.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
      GetFileInformationByHandle(source, &before) &&
      GetFileSizeEx(source, &size) && size.QuadPart >= 0;
  if (!regular || static_cast<std::uint64_t>(size.QuadPart) >
                      SkinPackagePolicy::maxArchiveBytes) {
    CloseHandle(source);
    diagnostics.push_back(diagnostic(
        "skin_archive_input_invalid",
        "skin archive is not a regular no-follow file or exceeds its limit"));
    return std::nullopt;
  }
  while (true) {
    if (stop.stop_requested()) {
      CloseHandle(source);
      cancelled = true;
      return std::nullopt;
    }
    DWORD read = 0;
    if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &read, nullptr)) {
      CloseHandle(source);
      diagnostics.push_back(diagnostic("skin_archive_input_read_failed",
                                       "unable to copy the opened skin ZIP"));
      return std::nullopt;
    }
    if (read == 0) {
      break;
    }
    if (total > SkinPackagePolicy::maxArchiveBytes - read ||
        !owned.write(std::span(buffer.data(), read))) {
      CloseHandle(source);
      diagnostics.push_back(diagnostic("skin_archive_owned_copy_failed",
                                       "unable to copy the opened skin ZIP"));
      return std::nullopt;
    }
    total += read;
    observe(observer, SkinImportIoOperation::OwnedArchiveCopyChunk, path);
    if (stop.stop_requested()) {
      CloseHandle(source);
      cancelled = true;
      return std::nullopt;
    }
  }
  BY_HANDLE_FILE_INFORMATION after{};
  const bool stable =
      GetFileInformationByHandle(source, &after) &&
      before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
      before.nFileIndexHigh == after.nFileIndexHigh &&
      before.nFileIndexLow == after.nFileIndexLow &&
      before.nFileSizeHigh == after.nFileSizeHigh &&
      before.nFileSizeLow == after.nFileSizeLow &&
      before.ftLastWriteTime.dwHighDateTime ==
          after.ftLastWriteTime.dwHighDateTime &&
      before.ftLastWriteTime.dwLowDateTime ==
          after.ftLastWriteTime.dwLowDateTime &&
      total == static_cast<std::uint64_t>(size.QuadPart);
  CloseHandle(source);
  if (!stable) {
    diagnostics.push_back(diagnostic("skin_archive_source_changed",
                                     "skin archive changed while copying"));
    return std::nullopt;
  }
#else
  const int source =
      ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat before{};
  if (source < 0 || ::fstat(source, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) >
          SkinPackagePolicy::maxArchiveBytes) {
    if (source >= 0) {
      ::close(source);
    }
    diagnostics.push_back(diagnostic(
        "skin_archive_input_invalid",
        "skin archive is not a regular no-follow file or exceeds its limit"));
    return std::nullopt;
  }
  while (true) {
    if (stop.stop_requested()) {
      ::close(source);
      cancelled = true;
      return std::nullopt;
    }
    const ssize_t count = ::read(source, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      ::close(source);
      diagnostics.push_back(diagnostic("skin_archive_input_read_failed",
                                       "unable to copy the opened skin ZIP"));
      return std::nullopt;
    }
    if (count == 0) {
      break;
    }
    const auto chunk = static_cast<std::uint64_t>(count);
    if (total > SkinPackagePolicy::maxArchiveBytes - chunk ||
        !owned.write(
            std::span(buffer.data(), static_cast<std::size_t>(count)))) {
      ::close(source);
      diagnostics.push_back(diagnostic("skin_archive_owned_copy_failed",
                                       "unable to copy the opened skin ZIP"));
      return std::nullopt;
    }
    total += chunk;
    observe(observer, SkinImportIoOperation::OwnedArchiveCopyChunk, path);
    if (stop.stop_requested()) {
      ::close(source);
      cancelled = true;
      return std::nullopt;
    }
  }
  struct stat after{};
  bool stable =
      ::fstat(source, &after) == 0 && before.st_dev == after.st_dev &&
      before.st_ino == after.st_ino && before.st_size == after.st_size &&
      before.st_nlink == after.st_nlink && before.st_mode == after.st_mode &&
      total == static_cast<std::uint64_t>(before.st_size);
#if defined(__APPLE__)
  stable = stable && before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
           before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
  stable = stable && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
  ::close(source);
  if (!stable) {
    diagnostics.push_back(diagnostic("skin_archive_source_changed",
                                     "skin archive changed while copying"));
    return std::nullopt;
  }
#endif
  if (!owned.finishCopy()) {
    diagnostics.push_back(diagnostic("skin_archive_owned_copy_failed",
                                     "unable to finalize the owned skin ZIP"));
    return std::nullopt;
  }
  owned.setBytes(total);
  return owned;
}

struct ArchiveDeleter {
  void operator()(archive *value) const noexcept {
    if (value != nullptr) {
      archive_read_free(value);
    }
  }
};

using ArchiveHandle = std::unique_ptr<archive, ArchiveDeleter>;

SkinDiagnostic diagnostic(std::string code, std::string message,
                          std::string virtualPath) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::string archiveError(archive *reader, std::string_view fallback) {
  const char *message = archive_error_string(reader);
  return message == nullptr ? std::string(fallback) : std::string(message);
}

bool configureArchiveReader(archive *reader, OwnedArchiveFile &owned,
                            std::vector<SkinDiagnostic> &diagnostics) {
  if (archive_read_support_filter_none(reader) != ARCHIVE_OK ||
      archive_read_support_format_zip(reader) != ARCHIVE_OK ||
      archive_read_set_format_option(reader, "zip", "mac-ext", nullptr) !=
          ARCHIVE_OK ||
      !owned.rewind() ||
      archive_read_open_FILE(reader, owned.get()) != ARCHIVE_OK) {
    diagnostics.push_back(diagnostic(
        "skin_archive_open_failed",
        archiveError(reader, "unable to open the skin ZIP archive")));
    return false;
  }
  return true;
}

bool report(const SkinProgressCallback &callback, SkinProgress progress,
            std::vector<SkinDiagnostic> &diagnostics) {
  if (!callback) {
    return true;
  }
  try {
    callback(progress);
    return true;
  } catch (...) {
    diagnostics.push_back(
        diagnostic("skin_import_progress_callback_failed",
                   "skin package progress callback raised an exception"));
    return false;
  }
}

void observe(const std::shared_ptr<const SkinImportIoObserver> &observer,
             SkinImportIoOperation operation, const fs::path &path) {
  if (observer) {
    observer->reached(operation, path);
  }
}

std::string utf8Path(const fs::path &path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

fs::path pathFromUtf8(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const unsigned char byte : value) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return fs::path(utf8);
}

bool hasAppleDoubleComponent(std::string_view path) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component(
        path.data() + start,
        (end == std::string_view::npos ? path.size() : end) - start);
    if (component.starts_with("._")) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

bool supportedCompression(archive *reader) {
  const char *format = archive_format_name(reader);
  if (format == nullptr) {
    return false;
  }
  const std::string_view name(format);
  return name.ends_with("(uncompressed)") || name.ends_with("(deflation)");
}

bool addWithoutOverflow(std::uint64_t left, std::uint64_t right,
                        std::uint64_t maximum, std::uint64_t &sum) {
  if (right > maximum || left > maximum - right) {
    return false;
  }
  sum = left + right;
  return true;
}

std::optional<std::string>
hashOwnedArchive(OwnedArchiveFile &owned, std::stop_token stop, bool &cancelled,
                 const std::shared_ptr<const SkinImportIoObserver> &observer) {
  file_checksum::Sha256 hash;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::uint64_t offset = 0;
  while (offset < owned.bytes()) {
    if (stop.stop_requested()) {
      cancelled = true;
      return std::nullopt;
    }
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), owned.bytes() - offset));
    if (!owned.readAt(offset, std::span(buffer.data(), count))) {
      return std::nullopt;
    }
    hash.update(std::as_bytes(std::span(buffer.data(), count)));
    offset += count;
    observe(observer, SkinImportIoOperation::OwnedArchiveHashChunk, {});
    if (stop.stop_requested()) {
      cancelled = true;
      return std::nullopt;
    }
  }
  return hash.finalHex();
}

bool validateRawZipEnvelope(
    OwnedArchiveFile &owned, std::uint64_t archiveBytes,
    const SkinPackageId &package, std::stop_token stop, bool &cancelled,
    const std::shared_ptr<const SkinImportIoObserver> &observer,
    RawZipMembers &rawMembers, std::vector<SkinDiagnostic> &diagnostics) {
  constexpr std::uint64_t maximumTail = 65'557;
  const std::uint64_t tailSize = std::min(archiveBytes, maximumTail);
  if (tailSize < 22) {
    diagnostics.push_back(diagnostic("skin_archive_eocd_invalid",
                                     "skin ZIP has no complete end record"));
    return false;
  }
  std::vector<unsigned char> tail(static_cast<std::size_t>(tailSize));
  if (!owned.readAt(archiveBytes - tailSize, tail)) {
    diagnostics.push_back(diagnostic("skin_archive_input_read_failed",
                                     "unable to read skin ZIP metadata"));
    return false;
  }
  std::optional<std::size_t> eocd;
  for (std::size_t index = tail.size() - 22;; --index) {
    if ((index & 0x3ffU) == 0 && stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    if (tail[index] == 0x50 && tail[index + 1] == 0x4b &&
        tail[index + 2] == 0x05 && tail[index + 3] == 0x06) {
      const std::uint16_t comment = little16(tail.data() + index + 20);
      if (index + 22U + comment == tail.size()) {
        eocd = index;
        break;
      }
    }
    if (index == 0) {
      break;
    }
  }
  if (!eocd) {
    diagnostics.push_back(
        diagnostic("skin_archive_eocd_invalid",
                   "skin ZIP end record is missing, truncated, or ambiguous"));
    return false;
  }
  const unsigned char *end = tail.data() + *eocd;
  const std::uint16_t disk = little16(end + 4);
  const std::uint16_t directoryDisk = little16(end + 6);
  const std::uint16_t diskRecords = little16(end + 8);
  const std::uint16_t totalRecords = little16(end + 10);
  const std::uint32_t directoryBytes = little32(end + 12);
  const std::uint32_t directoryOffset = little32(end + 16);
  const std::uint64_t eocdOffset = archiveBytes - tailSize + *eocd;
  if (disk != 0 || directoryDisk != 0 || diskRecords != totalRecords ||
      directoryBytes == 0xffffffffU || directoryOffset == 0xffffffffU ||
      totalRecords > SkinPackagePolicy::maxArchiveMembers ||
      static_cast<std::uint64_t>(directoryOffset) + directoryBytes >
          archiveBytes ||
      static_cast<std::uint64_t>(directoryOffset) + directoryBytes !=
          eocdOffset) {
    diagnostics.push_back(diagnostic(
        "skin_archive_directory_invalid",
        "skin ZIP uses multi-disk/ZIP64 metadata or exceeds the member limit"));
    return false;
  }

  std::uint64_t cursor = directoryOffset;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> localRanges;
  localRanges.reserve(totalRecords);
  for (std::uint32_t index = 0; index < totalRecords; ++index) {
    if (stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    observe(observer, SkinImportIoOperation::RawZipRecord, {});
    if (stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    std::array<unsigned char, 46> central{};
    if (!owned.readAt(cursor, central) ||
        little32(central.data()) != 0x02014b50U) {
      diagnostics.push_back(
          diagnostic("skin_archive_directory_invalid",
                     "skin ZIP central directory is truncated or malformed"));
      return false;
    }
    const std::uint16_t flags = little16(central.data() + 8);
    const std::uint16_t method = little16(central.data() + 10);
    const std::uint32_t crc = little32(central.data() + 16);
    const std::uint32_t compressedBytes = little32(central.data() + 20);
    const std::uint32_t uncompressedBytes = little32(central.data() + 24);
    const std::uint16_t nameBytes = little16(central.data() + 28);
    const std::uint16_t extraBytes = little16(central.data() + 30);
    const std::uint16_t commentBytes = little16(central.data() + 32);
    const std::uint16_t startingDisk = little16(central.data() + 34);
    const std::uint32_t externalAttributes = little32(central.data() + 38);
    const std::uint32_t localOffset = little32(central.data() + 42);
    const std::uint64_t recordBytes =
        46ULL + nameBytes + extraBytes + commentBytes;
    if (recordBytes > directoryBytes ||
        cursor + recordBytes >
            static_cast<std::uint64_t>(directoryOffset) + directoryBytes ||
        nameBytes == 0 || startingDisk != 0 || (flags & 0x0001U) != 0 ||
        (method != 0 && method != 8)) {
      diagnostics.push_back(diagnostic(
          "skin_archive_member_metadata_invalid",
          "skin ZIP member metadata is unsafe, encrypted, or uses unsupported "
          "compression"));
      return false;
    }
    std::vector<unsigned char> name(nameBytes);
    if (!owned.readAt(cursor + 46, name)) {
      diagnostics.push_back(diagnostic("skin_archive_directory_invalid",
                                       "skin ZIP member name is truncated"));
      return false;
    }
    const std::string rawName(reinterpret_cast<const char *>(name.data()),
                              name.size());
    if (rawName.find('\0') != std::string::npos ||
        rawName.find('\\') != std::string::npos) {
      diagnostics.push_back(
          diagnostic("skin_archive_raw_path_invalid",
                     "skin ZIP member name contains NUL or a backslash"));
      return false;
    }
    std::string structuralName = rawName;
    const bool trailingSlash = structuralName.ends_with('/');
    if (trailingSlash) {
      structuralName.pop_back();
    }
    const auto normalized = normalizeEntryPath(package, structuralName);
    if (!normalized.entry || hasAppleDoubleComponent(structuralName)) {
      diagnostics.push_back(
          diagnostic("skin_archive_raw_path_invalid",
                     normalized.entry ? "AppleDouble sidecars are not allowed"
                                      : normalized.error,
                     structuralName));
      return false;
    }
    const unsigned char creatorSystem = central[5];
    if (creatorSystem == 3) {
      const mode_t type =
          static_cast<mode_t>((externalAttributes >> 16U) & AE_IFMT);
      if (type != 0 && type != AE_IFREG && type != AE_IFDIR) {
        diagnostics.push_back(
            diagnostic("skin_archive_special_entry_rejected",
                       "skin ZIP central metadata declares a special node",
                       structuralName));
        return false;
      }
      if ((type == AE_IFDIR) != trailingSlash && type != 0) {
        diagnostics.push_back(diagnostic(
            "skin_archive_member_metadata_invalid",
            "skin ZIP pathname and Unix node type disagree", structuralName));
        return false;
      }
    }

    std::array<unsigned char, 30> local{};
    if (!owned.readAt(localOffset, local) ||
        little32(local.data()) != 0x04034b50U ||
        little16(local.data() + 6) != flags ||
        little16(local.data() + 8) != method ||
        little16(local.data() + 26) != nameBytes) {
      diagnostics.push_back(diagnostic(
          "skin_archive_local_header_mismatch",
          "skin ZIP local and central headers disagree", structuralName));
      return false;
    }
    const std::uint32_t localCrc = little32(local.data() + 14);
    const std::uint32_t localCompressed = little32(local.data() + 18);
    const std::uint32_t localUncompressed = little32(local.data() + 22);
    const std::uint16_t localExtraBytes = little16(local.data() + 28);
    const bool descriptor = (flags & 0x0008U) != 0;
    const bool localValuesValid =
        !descriptor ? localCrc == crc && localCompressed == compressedBytes &&
                          localUncompressed == uncompressedBytes
                    : (localCrc == 0 || localCrc == crc) &&
                          (localCompressed == 0 ||
                           localCompressed == compressedBytes) &&
                          (localUncompressed == 0 ||
                           localUncompressed == uncompressedBytes);
    const std::uint64_t dataOffset = static_cast<std::uint64_t>(localOffset) +
                                     30U + nameBytes + localExtraBytes;
    const std::uint64_t dataEnd = dataOffset + compressedBytes;
    if (!localValuesValid || dataOffset > directoryOffset ||
        dataEnd < dataOffset || dataEnd > directoryOffset) {
      diagnostics.push_back(diagnostic(
          "skin_archive_local_header_mismatch",
          "skin ZIP local data extent or size metadata is inconsistent",
          structuralName));
      return false;
    }
    std::vector<unsigned char> localName(nameBytes);
    if (!owned.readAt(static_cast<std::uint64_t>(localOffset) + 30,
                      localName) ||
        localName != name) {
      diagnostics.push_back(diagnostic(
          "skin_archive_local_header_mismatch",
          "skin ZIP local and central member names disagree", structuralName));
      return false;
    }
    localRanges.emplace_back(localOffset, dataEnd);
    if (!rawMembers
             .emplace(rawName,
                      RawZipMember{.uncompressedBytes = uncompressedBytes,
                                   .compressedBytes = compressedBytes,
                                   .crc32 = crc,
                                   .compressionMethod = method})
             .second) {
      diagnostics.push_back(diagnostic(
          "skin_archive_path_collision",
          "skin ZIP repeats the same raw member name", structuralName));
      return false;
    }
    cursor += recordBytes;
  }
  if (cursor != static_cast<std::uint64_t>(directoryOffset) + directoryBytes) {
    diagnostics.push_back(diagnostic(
        "skin_archive_directory_invalid",
        "skin ZIP central directory record count or size is inconsistent"));
    return false;
  }
  std::ranges::sort(localRanges);
  for (std::size_t index = 1; index < localRanges.size(); ++index) {
    if (localRanges[index].first < localRanges[index - 1].second) {
      diagnostics.push_back(
          diagnostic("skin_archive_local_header_mismatch",
                     "skin ZIP local members overlap or reuse storage"));
      return false;
    }
  }
  return true;
}

bool addStructuralNode(std::map<std::string, std::pair<std::string, MemberKind>,
                                std::less<>> &nodes,
                       std::string normalizedPath, std::string collisionKey,
                       MemberKind kind,
                       std::vector<SkinDiagnostic> &diagnostics) {
  const auto existing = nodes.find(collisionKey);
  if (existing == nodes.end()) {
    nodes.emplace(std::move(collisionKey),
                  std::pair{std::move(normalizedPath), kind});
    return true;
  }
  if (existing->second.first != normalizedPath ||
      existing->second.second != kind) {
    diagnostics.push_back(diagnostic(
        "skin_archive_path_collision",
        "archive contains colliding paths or a file/directory collision",
        std::move(normalizedPath)));
    return false;
  }
  return true;
}

bool validateInstalledStructure(ArchiveInventory &inventory,
                                const SkinPackageId &package,
                                std::vector<SkinDiagnostic> &diagnostics) {
  std::map<std::string, std::pair<std::string, MemberKind>, std::less<>> nodes;
  std::set<std::string, std::less<>> explicitNodes;
  for (ArchiveMember &member : inventory.members) {
    std::string installed = member.normalizedPath;
    if (inventory.wrapper) {
      if (member.kind == MemberKind::Directory &&
          installed == *inventory.wrapper) {
        member.installedPath.clear();
        member.installedCollisionKey.clear();
        continue;
      }
      const std::string prefix = *inventory.wrapper + "/";
      if (!installed.starts_with(prefix)) {
        diagnostics.push_back(diagnostic(
            "skin_archive_wrapper_mismatch",
            "archive explicit directory is outside the inferred wrapper",
            installed));
        return false;
      }
      installed.erase(0, prefix.size());
    }
    const auto normalized = normalizeEntryPath(package, installed);
    if (!normalized.entry) {
      diagnostics.push_back(diagnostic("skin_archive_post_strip_path_invalid",
                                       normalized.error, installed));
      return false;
    }
    member.installedPath = normalized.entry->packageRelativePath;
    member.installedCollisionKey = normalized.entry->collisionKey;

    if (!explicitNodes.insert(member.installedCollisionKey).second) {
      diagnostics.push_back(diagnostic(
          "skin_archive_path_collision",
          "archive contains a duplicate or colliding normalized path",
          member.installedPath));
      return false;
    }

    std::size_t slash = 0;
    while ((slash = member.installedPath.find('/', slash)) !=
           std::string::npos) {
      const std::string parent = member.installedPath.substr(0, slash);
      const auto normalizedParent = normalizeEntryPath(package, parent);
      assert(normalizedParent.entry.has_value());
      if (!addStructuralNode(nodes, normalizedParent.entry->packageRelativePath,
                             normalizedParent.entry->collisionKey,
                             MemberKind::Directory, diagnostics)) {
        return false;
      }
      ++slash;
    }
    if (!addStructuralNode(nodes, member.installedPath,
                           member.installedCollisionKey, member.kind,
                           diagnostics)) {
      return false;
    }
  }
  return true;
}

std::optional<ArchiveInventory>
inventoryArchive(OwnedArchiveFile &owned, const SkinPackageId &package,
                 const RawZipMembers &rawMembers, std::stop_token stop,
                 const SkinProgressCallback &callback,
                 std::vector<SkinDiagnostic> &diagnostics) {
  ArchiveHandle reader(archive_read_new());
  if (!reader || !configureArchiveReader(reader.get(), owned, diagnostics)) {
    return std::nullopt;
  }

  ArchiveInventory inventory;
  std::uint64_t memberCount = 0;
  std::map<std::string, std::pair<std::string, MemberKind>, std::less<>> seen;
  while (true) {
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    archive_entry *entry = nullptr;
    const int status = archive_read_next_header(reader.get(), &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status != ARCHIVE_OK || entry == nullptr ||
        (archive_format(reader.get()) & ARCHIVE_FORMAT_BASE_MASK) !=
            ARCHIVE_FORMAT_ZIP) {
      diagnostics.push_back(diagnostic(
          "skin_archive_inventory_failed",
          archiveError(reader.get(), "unable to inventory the skin ZIP")));
      return std::nullopt;
    }
    ++memberCount;
    if (memberCount > SkinPackagePolicy::maxArchiveMembers) {
      diagnostics.push_back(
          diagnostic("skin_archive_member_limit_exceeded",
                     "archive exceeds the package member-count limit"));
      return std::nullopt;
    }
    if (!supportedCompression(reader.get())) {
      diagnostics.push_back(
          diagnostic("skin_archive_compression_unsupported",
                     "skin ZIP entries must use store or deflate compression"));
      return std::nullopt;
    }
    const char *rawName = archive_entry_pathname_utf8(entry);
    if (rawName == nullptr) {
      rawName = archive_entry_pathname(entry);
    }
    if (rawName == nullptr) {
      diagnostics.push_back(diagnostic("skin_archive_path_invalid",
                                       "archive entry has no UTF-8 path"));
      return std::nullopt;
    }
    std::string authoredPath(rawName);
    const auto rawMetadata = rawMembers.find(authoredPath);
    if (rawMetadata == rawMembers.end()) {
      diagnostics.push_back(diagnostic(
          "skin_archive_inventory_failed",
          "archive reader exposed a member absent from raw inventory",
          authoredPath));
      return std::nullopt;
    }
    const mode_t fileType = archive_entry_filetype(entry);
    const bool directory = fileType == AE_IFDIR;
    const bool regular = fileType == AE_IFREG;
    if ((!directory && !regular) || archive_entry_symlink(entry) != nullptr ||
        archive_entry_hardlink(entry) != nullptr ||
        archive_entry_sparse_count(entry) != 0 ||
        archive_entry_is_encrypted(entry) > 0) {
      diagnostics.push_back(diagnostic(
          "skin_archive_special_entry_rejected",
          "archive contains a link, sparse, encrypted, or special entry",
          authoredPath));
      return std::nullopt;
    }
    if (directory) {
      if (!authoredPath.ends_with('/')) {
        diagnostics.push_back(
            diagnostic("skin_archive_directory_path_invalid",
                       "explicit archive directories must end with a slash",
                       authoredPath));
        return std::nullopt;
      }
      authoredPath.pop_back();
    }
    if (hasAppleDoubleComponent(authoredPath)) {
      diagnostics.push_back(
          diagnostic("skin_archive_appledouble_rejected",
                     "AppleDouble sidecars are not allowed in skin packages",
                     authoredPath));
      return std::nullopt;
    }
    const auto normalized = normalizeEntryPath(package, authoredPath);
    if (!normalized.entry) {
      diagnostics.push_back(diagnostic("skin_archive_path_invalid",
                                       normalized.error, authoredPath));
      return std::nullopt;
    }
    const MemberKind kind =
        directory ? MemberKind::Directory : MemberKind::Regular;
    const auto collision = seen.find(normalized.entry->collisionKey);
    if (collision != seen.end()) {
      diagnostics.push_back(diagnostic(
          "skin_archive_path_collision",
          "archive contains a duplicate or colliding normalized path",
          normalized.entry->packageRelativePath));
      return std::nullopt;
    }
    seen.emplace(normalized.entry->collisionKey,
                 std::pair{normalized.entry->packageRelativePath, kind});

    std::uint64_t declaredSize = 0;
    if (regular) {
      if (archive_entry_size_is_set(entry) == 0 ||
          archive_entry_size(entry) < 0) {
        diagnostics.push_back(
            diagnostic("skin_archive_size_invalid",
                       "regular archive entry has no valid declared size",
                       normalized.entry->packageRelativePath));
        return std::nullopt;
      }
      declaredSize = static_cast<std::uint64_t>(archive_entry_size(entry));
      if (declaredSize != rawMetadata->second.uncompressedBytes) {
        diagnostics.push_back(
            diagnostic("skin_archive_member_metadata_invalid",
                       "raw and decoded ZIP sizes disagree",
                       normalized.entry->packageRelativePath));
        return std::nullopt;
      }
      if (declaredSize > SkinPackagePolicy::maxRegularFileBytes) {
        diagnostics.push_back(
            diagnostic("skin_archive_file_too_large",
                       "regular archive entry exceeds the package file limit",
                       normalized.entry->packageRelativePath));
        return std::nullopt;
      }
      if (++inventory.fileCount > SkinPackagePolicy::maxFiles ||
          !addWithoutOverflow(inventory.totalBytes, declaredSize,
                              SkinPackagePolicy::maxExpandedBytes,
                              inventory.totalBytes)) {
        diagnostics.push_back(diagnostic(
            "skin_archive_package_limit_exceeded",
            "archive exceeds the package file-count or expanded-byte limit",
            normalized.entry->packageRelativePath));
        return std::nullopt;
      }
    } else if (archive_entry_size_is_set(entry) != 0 &&
               archive_entry_size(entry) != 0) {
      diagnostics.push_back(
          diagnostic("skin_archive_directory_size_invalid",
                     "explicit archive directory has nonzero data",
                     normalized.entry->packageRelativePath));
      return std::nullopt;
    }

    inventory.members.push_back(
        {.archivePath = std::string(rawName),
         .normalizedPath = normalized.entry->packageRelativePath,
         .collisionKey = normalized.entry->collisionKey,
         .kind = kind,
         .declaredSize = declaredSize,
         .expectedCrc32 = rawMetadata->second.crc32,
         .compressionMethod = rawMetadata->second.compressionMethod});
    if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
      diagnostics.push_back(diagnostic(
          "skin_archive_inventory_failed",
          archiveError(reader.get(), "unable to skip inventoried ZIP data"),
          normalized.entry->packageRelativePath));
      return std::nullopt;
    }
    if (!report(callback,
                {.phase = SkinProgressPhase::Inspecting,
                 .completedBytes = inventory.totalBytes,
                 .totalBytes = inventory.totalBytes,
                 .completedFiles = memberCount},
                diagnostics)) {
      return std::nullopt;
    }
  }
  if (archive_read_has_encrypted_entries(reader.get()) > 0) {
    diagnostics.push_back(
        diagnostic("skin_archive_encrypted_rejected",
                   "encrypted skin ZIP entries are not supported"));
    return std::nullopt;
  }
  if (archive_read_close(reader.get()) != ARCHIVE_OK) {
    diagnostics.push_back(
        diagnostic("skin_archive_inventory_failed",
                   archiveError(reader.get(), "skin ZIP ended unexpectedly")));
    return std::nullopt;
  }
  if (inventory.fileCount == 0) {
    diagnostics.push_back(
        diagnostic("skin_archive_empty", "skin ZIP contains no regular files"));
    return std::nullopt;
  }

  std::optional<std::string> wrapper;
  bool canStrip = true;
  for (const ArchiveMember &member : inventory.members) {
    if (member.kind != MemberKind::Regular) {
      continue;
    }
    const std::size_t slash = member.normalizedPath.find('/');
    if (slash == std::string::npos) {
      canStrip = false;
      break;
    }
    const std::string first = member.normalizedPath.substr(0, slash);
    if (!wrapper) {
      wrapper = first;
    } else if (*wrapper != first) {
      canStrip = false;
      break;
    }
  }
  if (!canStrip) {
    wrapper.reset();
  }
  inventory.wrapper = std::move(wrapper);
  if (!validateInstalledStructure(inventory, package, diagnostics)) {
    return std::nullopt;
  }
  return inventory;
}

std::string uniqueStagingName() {
  static std::atomic_uint64_t serial{0};
  return "import-" + std::to_string(++serial);
}

#if defined(_WIN32)
class PrivateWindowsSecurity {
public:
  PrivateWindowsSecurity() = default;
  PrivateWindowsSecurity(const PrivateWindowsSecurity &) = delete;
  PrivateWindowsSecurity &operator=(const PrivateWindowsSecurity &) = delete;
  ~PrivateWindowsSecurity() {
    if (accessControlList_ != nullptr) {
      LocalFree(accessControlList_);
    }
    if (token_ != nullptr) {
      CloseHandle(token_);
    }
  }

  bool initialize() {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_)) {
      return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token_, TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
      return false;
    }
    tokenUser_.resize(bytes);
    if (!GetTokenInformation(token_, TokenUser, tokenUser_.data(), bytes,
                             &bytes)) {
      return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(userSid());
    if (SetEntriesInAclW(1, &access, nullptr, &accessControlList_) !=
            ERROR_SUCCESS ||
        !InitializeSecurityDescriptor(&descriptor_,
                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor_, userSid(), FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor_, TRUE, accessControlList_,
                                   FALSE) ||
        !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) {
      return false;
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = &descriptor_;
    attributes_.bInheritHandle = FALSE;
    return true;
  }

  SECURITY_ATTRIBUTES *attributes() noexcept { return &attributes_; }

  bool verify(HANDLE handle) const {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result =
        GetSecurityInfo(handle, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    void *rawAce = nullptr;
    bool valid =
        result == ERROR_SUCCESS && descriptor != nullptr && owner != nullptr &&
        EqualSid(owner, userSid()) != FALSE && dacl != nullptr &&
        dacl->AceCount == 1 &&
        GetSecurityDescriptorControl(descriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0 && GetAce(dacl, 0, &rawAce) &&
        rawAce != nullptr;
    if (valid) {
      const auto *header = static_cast<const ACE_HEADER *>(rawAce);
      const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
      valid = header->AceType == ACCESS_ALLOWED_ACE_TYPE &&
              header->AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS &&
              EqualSid(const_cast<DWORD *>(&ace->SidStart), userSid()) != FALSE;
    }
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    return valid;
  }

private:
  PSID userSid() const noexcept {
    if (tokenUser_.empty()) {
      return nullptr;
    }
    return reinterpret_cast<const TOKEN_USER *>(tokenUser_.data())->User.Sid;
  }

  HANDLE token_ = nullptr;
  std::vector<std::byte> tokenUser_;
  PACL accessControlList_ = nullptr;
  SECURITY_DESCRIPTOR descriptor_{};
  SECURITY_ATTRIBUTES attributes_{};
};
#endif

class SecureStagingTree {
public:
  SecureStagingTree(const SecureStagingTree &) = delete;
  SecureStagingTree &operator=(const SecureStagingTree &) = delete;
  ~SecureStagingTree() { cleanup(); }

  static std::shared_ptr<SecureStagingTree>
  create(const SkinStorageRoots &roots,
         const std::shared_ptr<const SkinImportIoObserver> &observer,
         std::vector<SkinDiagnostic> &diagnostics) {
    if (roots.visiblePackages.empty() || !roots.visiblePackages.is_absolute()) {
      diagnostics.push_back(
          diagnostic("skin_import_visible_root_invalid",
                     "visible skin package storage is unavailable"));
      return {};
    }
    auto tree = std::shared_ptr<SecureStagingTree>(new SecureStagingTree());
    tree->observer_ = observer;
    if (!tree->allocate(roots.visiblePackages.parent_path() /
                            ".skin-import-staging",
                        diagnostics)) {
      return {};
    }
    observe(observer, SkinImportIoOperation::VisibleRootIssued, tree->path_);
    return tree;
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

  bool renameTo(const fs::path &destination,
                std::vector<SkinDiagnostic> &diagnostics,
                bool notifyObserver = true) {
    std::error_code pathError;
    const fs::path absolute =
        fs::absolute(destination, pathError).lexically_normal();
    const fs::path parent = absolute.parent_path();
    const fs::path leafPath = absolute.filename();
    if (pathError || !absolute.is_absolute() || parent.empty() ||
        leafPath.empty() || leafPath == fs::path(".") ||
        leafPath == fs::path("..")) {
      diagnostics.push_back(
          diagnostic("skin_import_publication_destination_invalid",
                     "visible package publication destination is invalid"));
      return false;
    }
    if (notifyObserver) {
      observe(observer_, SkinImportIoOperation::BeforeVisiblePublication,
              absolute);
    }
#if defined(_WIN32)
    if (!verifyIdentity(diagnostics)) {
      return false;
    }
    std::vector<HANDLE> destinationHandles;
    fs::path destinationParent;
    if (!openExistingAbsoluteWindowsDirectoryChain(parent, destinationHandles,
                                                   destinationParent)) {
      diagnostics.push_back(
          diagnostic("skin_import_publication_parent_invalid",
                     "visible package publication parent is unavailable"));
      return false;
    }
    const std::wstring leaf = leafPath.native();
    const std::size_t leafBytes = leaf.size() * sizeof(wchar_t);
    const std::size_t allocation =
        offsetof(FILE_RENAME_INFO, FileName) + leafBytes;
    std::vector<std::byte> storage(allocation);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = destinationHandles.back();
    rename->FileNameLength = static_cast<DWORD>(leafBytes);
    std::memcpy(rename->FileName, leaf.data(), leafBytes);
    const bool renamed =
        SetFileInformationByHandle(issuedRootHandle_, FileRenameInfo, rename,
                                   static_cast<DWORD>(allocation)) != 0;
    if (!renamed) {
      closeWindowsHandles(destinationHandles);
      diagnostics.push_back(
          diagnostic("skin_import_publication_rename_failed",
                     "unable to publish the exact prepared visible package"));
      return false;
    }
    closeWindowsHandles(ancestryHandles_);
    ancestryHandles_ = std::move(destinationHandles);
    parentPath_ = std::move(destinationParent);
#else
    const int destinationParent = openAbsoluteDirectory(parent, false);
    const std::string leaf = leafPath.string();
    struct stat expected{};
    struct stat actual{};
    const bool sourceMatches =
        rootFd_ >= 0 && parentFd_ >= 0 && destinationParent >= 0 &&
        ::fstat(rootFd_, &expected) == 0 && S_ISDIR(expected.st_mode) &&
        ::fstatat(parentFd_, name_.c_str(), &actual, AT_SYMLINK_NOFOLLOW) ==
            0 &&
        S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev &&
        expected.st_ino == actual.st_ino;
    bool renamed = false;
    if (sourceMatches) {
#if defined(__APPLE__)
      renamed = ::renameatx_np(parentFd_, name_.c_str(), destinationParent,
                               leaf.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
      renamed =
          ::syscall(SYS_renameat2, parentFd_, name_.c_str(), destinationParent,
                    leaf.c_str(), RENAME_NOREPLACE) == 0;
#else
      errno = ENOTSUP;
#endif
    }
    if (!renamed) {
      if (destinationParent >= 0) {
        ::close(destinationParent);
      }
      diagnostics.push_back(
          diagnostic("skin_import_publication_rename_failed",
                     "unable to publish the exact prepared visible package"));
      return false;
    }
    const bool sourceParentSynchronized = ::fsync(parentFd_) == 0;
    const bool destinationParentSynchronized = ::fsync(destinationParent) == 0;
    const bool synchronized =
        sourceParentSynchronized && destinationParentSynchronized;
    ::close(parentFd_);
    parentFd_ = destinationParent;
    name_ = leaf;
    path_ = absolute;
    if (!synchronized) {
      diagnostics.push_back(
          diagnostic("skin_import_publication_sync_failed",
                     "unable to synchronize visible package publication"));
      return false;
    }
#endif
#if defined(_WIN32)
    name_ = utf8Path(leafPath);
    path_ = absolute;
#endif
    return true;
  }

  void releaseOwnership() noexcept {
#if defined(_WIN32)
    if (issuedRootHandle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(issuedRootHandle_);
      issuedRootHandle_ = INVALID_HANDLE_VALUE;
    }
    closeWindowsHandles(ancestryHandles_);
#else
    for (const RetainedDirectory &retained : retainedDirectories_) {
      ::close(retained.fd);
    }
    retainedDirectories_.clear();
    activeParentFd_ = -1;
    activeParentPath_.clear();
    activeLeaf_.clear();
    if (rootFd_ >= 0) {
      ::close(rootFd_);
      rootFd_ = -1;
    }
    if (parentFd_ >= 0) {
      ::close(parentFd_);
      parentFd_ = -1;
    }
#endif
    path_.clear();
  }

  bool verifyIdentity(std::vector<SkinDiagnostic> &diagnostics) const {
#if defined(_WIN32)
    const HANDLE current = CreateFileW(
        path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (current == INVALID_HANDLE_VALUE) {
      diagnostics.push_back(
          diagnostic("skin_import_staging_identity_changed",
                     "visible staging root disappeared or was replaced"));
      return false;
    }
    BY_HANDLE_FILE_INFORMATION expected{};
    BY_HANDLE_FILE_INFORMATION actual{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool same =
        GetFileInformationByHandle(issuedRootHandle_, &expected) &&
        GetFileInformationByHandle(current, &actual) &&
        GetFileInformationByHandleEx(current, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        expected.dwVolumeSerialNumber == actual.dwVolumeSerialNumber &&
        expected.nFileIndexHigh == actual.nFileIndexHigh &&
        expected.nFileIndexLow == actual.nFileIndexLow;
    CloseHandle(current);
#else
    struct stat expected{};
    struct stat actual{};
    const bool same = ::fstat(rootFd_, &expected) == 0 &&
                      ::fstatat(parentFd_, name_.c_str(), &actual,
                                AT_SYMLINK_NOFOLLOW) == 0 &&
                      S_ISDIR(actual.st_mode) &&
                      expected.st_dev == actual.st_dev &&
                      expected.st_ino == actual.st_ino;
#endif
    if (!same) {
      diagnostics.push_back(diagnostic(
          "skin_import_staging_identity_changed",
          "visible staging root changed while it was being prepared"));
    }
    return same;
  }

  bool
  createDirectory(std::string_view relative,
                  const std::shared_ptr<const SkinImportIoObserver> &observer,
                  std::vector<SkinDiagnostic> &diagnostics) {
    observe(observer, SkinImportIoOperation::BeforeVisibleDirectory,
            path_ / pathFromUtf8(relative));
#if defined(_WIN32)
    std::vector<HANDLE> opened;
    if (openWindowsDirectories(relative, true, opened)) {
      closeWindowsHandles(opened);
      return true;
    }
    closeWindowsHandles(opened);
#else
    const int directory = openDirectory(relative, true);
    if (directory >= 0) {
      ::close(directory);
      return true;
    }
#endif
    diagnostics.push_back(
        diagnostic("skin_import_visible_copy_failed",
                   "unable to create a safe visible staging directory",
                   std::string(relative)));
    return false;
  }

#if defined(_WIN32)
  HANDLE createFile(std::string_view relative,
                    const std::shared_ptr<const SkinImportIoObserver> &observer,
                    std::vector<SkinDiagnostic> &diagnostics) {
    const fs::path target = path_ / pathFromUtf8(relative);
    const std::size_t separator = relative.rfind('/');
    const std::string_view parent = separator == std::string_view::npos
                                        ? std::string_view{}
                                        : relative.substr(0, separator);
    std::vector<HANDLE> opened;
    if (!openWindowsDirectories(parent, true, opened)) {
      closeWindowsHandles(opened);
      diagnostics.push_back(diagnostic("skin_import_visible_copy_failed",
                                       "unable to create a safe staging parent",
                                       std::string(relative)));
      return INVALID_HANDLE_VALUE;
    }
    observe(observer, SkinImportIoOperation::BeforeVisibleFile, target);
    const HANDLE file = CreateFileW(
        target.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        security_.attributes(), CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    closeWindowsHandles(opened);
    if (file != INVALID_HANDLE_VALUE && !security_.verify(file)) {
      CloseHandle(file);
      diagnostics.push_back(diagnostic(
          "skin_import_visible_copy_failed",
          "created visible staging file does not have private ownership",
          std::string(relative)));
      return INVALID_HANDLE_VALUE;
    }
    if (file == INVALID_HANDLE_VALUE) {
      diagnostics.push_back(diagnostic("skin_import_visible_copy_failed",
                                       "unable to exclusively create a safe "
                                       "visible staging file",
                                       std::string(relative)));
    }
    return file;
  }
#else
  int createFile(std::string_view relative,
                 const std::shared_ptr<const SkinImportIoObserver> &observer,
                 std::vector<SkinDiagnostic> &diagnostics) {
    const std::size_t separator = relative.rfind('/');
    const std::string_view parentPath = separator == std::string_view::npos
                                            ? std::string_view{}
                                            : relative.substr(0, separator);
    const std::string leaf(relative.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    int parent = openDirectory(parentPath, true);
    if (parent < 0 || activeParentFd_ >= 0) {
      if (parent >= 0) {
        ::close(parent);
      }
      diagnostics.push_back(diagnostic("skin_import_visible_copy_failed",
                                       "unable to create a safe staging parent",
                                       std::string(relative)));
      return -1;
    }
    const std::size_t parentDepth =
        parentPath.empty()
            ? 0
            : 1 + static_cast<std::size_t>(std::ranges::count(parentPath, '/'));
    parent = retainDirectoryForCleanup(parent, parentDepth);
    if (parent < 0) {
      diagnostics.push_back(
          diagnostic("skin_import_visible_copy_failed",
                     "unable to retain a safe staging-parent capability",
                     std::string(relative)));
      return -1;
    }
    observe(observer, SkinImportIoOperation::BeforeVisibleFile,
            path_ / pathFromUtf8(relative));
    if (!directoryMatchesRelative(parentPath, parent)) {
      diagnostics.push_back(
          diagnostic("skin_import_staging_identity_changed",
                     "visible staging parent moved before file creation",
                     std::string(relative)));
      return -1;
    }
    activeParentFd_ = parent;
    activeParentPath_ = std::string(parentPath);
    activeLeaf_ = leaf;
    const int file =
        ::openat(parent, leaf.c_str(),
                 O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) {
      activeParentFd_ = -1;
      activeParentPath_.clear();
      activeLeaf_.clear();
      diagnostics.push_back(diagnostic("skin_import_visible_copy_failed",
                                       "unable to exclusively create a safe "
                                       "visible staging file",
                                       std::string(relative)));
    }
    return file;
  }
#endif

  bool finishFile(std::string_view relative,
                  std::vector<SkinDiagnostic> &diagnostics) {
#if defined(_WIN32)
    return true;
#else
    const std::size_t separator = relative.rfind('/');
    const std::string_view parentPath = separator == std::string_view::npos
                                            ? std::string_view{}
                                            : relative.substr(0, separator);
    const std::string_view leaf = relative.substr(
        separator == std::string_view::npos ? 0 : separator + 1);
    const int parent = activeParentFd_;
    activeParentFd_ = -1;
    const bool expected =
        parent >= 0 && activeParentPath_ == parentPath && activeLeaf_ == leaf;
    activeParentPath_.clear();
    activeLeaf_.clear();
    if (expected && directoryMatchesRelative(parentPath, parent)) {
      return true;
    }
    if (parent >= 0) {
      ::unlinkat(parent, std::string(leaf).c_str(), 0);
    }
    diagnostics.push_back(
        diagnostic("skin_import_staging_identity_changed",
                   "visible staging parent moved while writing a file",
                   std::string(relative)));
    return false;
#endif
  }

#if defined(_WIN32)
  HANDLE openFileForRead(std::string_view relative) const {
    const fs::path target = path_ / pathFromUtf8(relative);
    HANDLE file = CreateFileW(
        target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool safe = file != INVALID_HANDLE_VALUE &&
                      GetFileInformationByHandleEx(file, FileAttributeTagInfo,
                                                   &tag, sizeof(tag)) &&
                      (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                             FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
                      GetFileInformationByHandle(file, &information) &&
                      information.nNumberOfLinks == 1 && security_.verify(file);
    if (!safe && file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      file = INVALID_HANDLE_VALUE;
    }
    return file;
  }
#else
  int openFileForRead(std::string_view relative) const {
    const std::size_t separator = relative.rfind('/');
    const std::string_view parentPath = separator == std::string_view::npos
                                            ? std::string_view{}
                                            : relative.substr(0, separator);
    const std::string leaf(relative.substr(
        separator == std::string_view::npos ? 0 : separator + 1));
    const int parent = openDirectory(parentPath, false);
    if (parent < 0) {
      return -1;
    }
    const int file = ::openat(parent, leaf.c_str(),
                              O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    ::close(parent);
    return file;
  }
#endif

private:
  SecureStagingTree() = default;

#if defined(_WIN32)
  static void closeWindowsHandles(std::vector<HANDLE> &handles) noexcept {
    for (const HANDLE handle : handles) {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
    handles.clear();
  }

  static bool safeWindowsDirectory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    return handle != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                        sizeof(tag)) &&
           (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
  }

  static bool
  openExistingAbsoluteWindowsDirectoryChain(const fs::path &path,
                                            std::vector<HANDLE> &handles,
                                            fs::path &canonicalPath) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    if (error || !absolute.is_absolute()) {
      return false;
    }
    fs::path current = absolute.root_path();
    HANDLE root = CreateFileW(
        current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!safeWindowsDirectory(root)) {
      if (root != INVALID_HANDLE_VALUE) {
        CloseHandle(root);
      }
      return false;
    }
    handles.push_back(root);
    const fs::path relative = absolute.lexically_relative(current);
    for (const fs::path &component : relative) {
      current /= component;
      HANDLE next = CreateFileW(
          current.c_str(),
          FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (!safeWindowsDirectory(next)) {
        if (next != INVALID_HANDLE_VALUE) {
          CloseHandle(next);
        }
        closeWindowsHandles(handles);
        return false;
      }
      handles.push_back(next);
    }
    canonicalPath = absolute;
    return true;
  }

  bool openWindowsDirectories(std::string_view relative, bool create,
                              std::vector<HANDLE> &handles) const {
    fs::path current = path_;
    std::size_t start = 0;
    while (start < relative.size()) {
      const std::size_t separator = relative.find('/', start);
      current /= pathFromUtf8(relative.substr(
          start, separator == std::string_view::npos ? std::string_view::npos
                                                     : separator - start));
      if (create &&
          !CreateDirectoryW(current.c_str(), security_.attributes()) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
      HANDLE next = CreateFileW(
          current.c_str(),
          FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (!safeWindowsDirectory(next) || !security_.verify(next)) {
        if (next != INVALID_HANDLE_VALUE) {
          CloseHandle(next);
        }
        return false;
      }
      handles.push_back(next);
      if (separator == std::string_view::npos) {
        break;
      }
      start = separator + 1;
    }
    return true;
  }

  static bool markWindowsDeletion(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                      sizeof(disposition)) != 0;
  }

  static bool clearWindowsDirectory(const fs::path &directory) noexcept {
    WIN32_FIND_DATAW data{};
    const fs::path pattern = directory / L"*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
      return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool ok = true;
    do {
      const std::wstring_view name(data.cFileName);
      if (name == L"." || name == L"..") {
        continue;
      }
      const fs::path childPath = directory / data.cFileName;
      const bool directoryEntry =
          (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      HANDLE child = CreateFileW(
          childPath.c_str(),
          DELETE | FILE_READ_ATTRIBUTES |
              (directoryEntry ? FILE_LIST_DIRECTORY : 0),
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT |
              (directoryEntry ? FILE_FLAG_BACKUP_SEMANTICS : 0),
          nullptr);
      if (child == INVALID_HANDLE_VALUE) {
        ok = false;
        break;
      }
      FILE_ATTRIBUTE_TAG_INFO tag{};
      const bool metadata =
          GetFileInformationByHandleEx(child, FileAttributeTagInfo, &tag,
                                       sizeof(tag)) != 0;
      const bool reparse =
          metadata && (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      if (!metadata ||
          (directoryEntry && !reparse && !clearWindowsDirectory(childPath)) ||
          !markWindowsDeletion(child)) {
        ok = false;
      }
      CloseHandle(child);
      if (!ok) {
        break;
      }
    } while (FindNextFileW(find, &data));
    const DWORD endError = GetLastError();
    FindClose(find);
    return ok && endError == ERROR_NO_MORE_FILES;
  }
#endif

#if !defined(_WIN32)
  struct RetainedDirectory {
    int fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
    std::size_t depth = 0;
  };

  int retainDirectoryForCleanup(int directory, std::size_t depth) {
    struct stat status{};
    struct stat rootStatus{};
    if (::fstat(directory, &status) != 0 || !S_ISDIR(status.st_mode) ||
        ::fstat(rootFd_, &rootStatus) != 0) {
      ::close(directory);
      return -1;
    }
    if (status.st_dev == rootStatus.st_dev &&
        status.st_ino == rootStatus.st_ino) {
      ::close(directory);
      return rootFd_;
    }
    for (RetainedDirectory &retained : retainedDirectories_) {
      if (status.st_dev == retained.device && status.st_ino == retained.inode) {
        retained.depth = std::max(retained.depth, depth);
        ::close(directory);
        return retained.fd;
      }
    }
    retainedDirectories_.push_back({.fd = directory,
                                    .device = status.st_dev,
                                    .inode = status.st_ino,
                                    .depth = depth});
    return directory;
  }

  bool directoryMatchesRelative(std::string_view relative,
                                int expectedDirectory) const {
    const int current = openDirectory(relative, false);
    struct stat expected{};
    struct stat actual{};
    const bool same =
        current >= 0 && ::fstat(expectedDirectory, &expected) == 0 &&
        ::fstat(current, &actual) == 0 && expected.st_dev == actual.st_dev &&
        expected.st_ino == actual.st_ino;
    if (current >= 0) {
      ::close(current);
    }
    return same;
  }

  static int openAbsoluteDirectory(const fs::path &path, bool create) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    if (error || !absolute.is_absolute()) {
      return -1;
    }
    int current = ::open(absolute.root_path().c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (current < 0) {
      return -1;
    }
    const fs::path relative = absolute.lexically_relative(absolute.root_path());
    for (const fs::path &componentPath : relative) {
      const std::string component = componentPath.string();
      int next = ::openat(current, component.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && create && errno == ENOENT &&
          ::mkdirat(current, component.c_str(), 0700) == 0) {
        next = ::openat(current, component.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      }
      ::close(current);
      if (next < 0) {
        return -1;
      }
      current = next;
    }
    return current;
  }

  int openDirectory(std::string_view relative, bool create) const {
    const int duplicate = ::dup(rootFd_);
    if (duplicate < 0) {
      return -1;
    }
    int current = duplicate;
    std::size_t start = 0;
    while (start < relative.size()) {
      const std::size_t separator = relative.find('/', start);
      const std::string component(relative.substr(
          start, separator == std::string_view::npos ? std::string_view::npos
                                                     : separator - start));
      int next = ::openat(current, component.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && create && errno == ENOENT &&
          ::mkdirat(current, component.c_str(), 0700) == 0) {
        next = ::openat(current, component.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      }
      ::close(current);
      if (next < 0) {
        return -1;
      }
      current = next;
      if (separator == std::string_view::npos) {
        break;
      }
      start = separator + 1;
    }
    return current;
  }

  static bool clearDirectory(int directory) {
    const int duplicate = ::dup(directory);
    DIR *stream = duplicate >= 0 ? ::fdopendir(duplicate) : nullptr;
    if (stream == nullptr) {
      if (duplicate >= 0) {
        ::close(duplicate);
      }
      return false;
    }
    bool ok = true;
    while (const dirent *entry = ::readdir(stream)) {
      const std::string_view name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      struct stat status{};
      if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
          0) {
        ok = false;
        continue;
      }
      if (S_ISDIR(status.st_mode)) {
        const int child =
            ::openat(directory, entry->d_name,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0 || !clearDirectory(child) ||
            ::unlinkat(directory, entry->d_name, AT_REMOVEDIR) != 0) {
          ok = false;
        }
        if (child >= 0) {
          ::close(child);
        }
      } else if (::unlinkat(directory, entry->d_name, 0) != 0) {
        ok = false;
      }
    }
    ::closedir(stream);
    return ok;
  }
#endif

  bool allocate(const fs::path &parentPath,
                std::vector<SkinDiagnostic> &diagnostics) {
#if defined(_WIN32)
    fs::path ancestryPath;
    if (!security_.initialize() ||
        !openExistingAbsoluteWindowsDirectoryChain(
            parentPath.parent_path(), ancestryHandles_, ancestryPath)) {
      closeWindowsHandles(ancestryHandles_);
      diagnostics.push_back(
          diagnostic("skin_import_staging_create_failed",
                     "unable to create private visible staging storage"));
      return false;
    }
    parentPath_ = ancestryPath / parentPath.filename();
    if (!CreateDirectoryW(parentPath_.c_str(), security_.attributes()) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      closeWindowsHandles(ancestryHandles_);
      diagnostics.push_back(
          diagnostic("skin_import_staging_create_failed",
                     "unable to create private visible staging storage"));
      return false;
    }
    HANDLE stagingParent = CreateFileW(
        parentPath_.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!safeWindowsDirectory(stagingParent) ||
        !security_.verify(stagingParent)) {
      if (stagingParent != INVALID_HANDLE_VALUE) {
        CloseHandle(stagingParent);
      }
      closeWindowsHandles(ancestryHandles_);
      diagnostics.push_back(
          diagnostic("skin_import_staging_create_failed",
                     "visible staging parent is not owner-private"));
      return false;
    }
    ancestryHandles_.push_back(stagingParent);
    for (int attempt = 0; attempt < 128; ++attempt) {
      name_ = uniqueStagingName();
      path_ = parentPath_ / name_;
      if (!CreateDirectoryW(path_.c_str(), security_.attributes())) {
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
          continue;
        }
        break;
      }
      issuedRootHandle_ = CreateFileW(
          path_.c_str(),
          FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      const bool safeIssuedDirectory = safeWindowsDirectory(issuedRootHandle_);
      if (safeIssuedDirectory && security_.verify(issuedRootHandle_)) {
        return true;
      }
      if (issuedRootHandle_ != INVALID_HANDLE_VALUE) {
        // A safe no-share-delete handle pins the exact directory identity. ACL
        // verification failure forbids recursive cleanup, but delete-pending
        // through this handle can safely dispose of the still-empty directory.
        // Invalid or non-directory handles leave a protected orphan for later
        // identity-aware recovery; pathname deletion is never a fallback.
        if (safeIssuedDirectory) {
          markWindowsDeletion(issuedRootHandle_);
        }
        CloseHandle(issuedRootHandle_);
        issuedRootHandle_ = INVALID_HANDLE_VALUE;
      }
      break;
    }
#else
    parentFd_ = openAbsoluteDirectory(parentPath, true);
    if (parentFd_ >= 0) {
      struct stat parentStatus{};
      // Documents/Skins is deliberately user-editable. iOS File Provider
      // roots can reject chmod even though creating and renaming a child is
      // allowed, so do not turn a visible package root into owner-only
      // storage. The descriptor was opened component-by-component without
      // following links; retaining that descriptor preserves the identity
      // boundary without imposing POSIX ownership or mode requirements.
      if (::fstat(parentFd_, &parentStatus) != 0 ||
          !S_ISDIR(parentStatus.st_mode)) {
        ::close(parentFd_);
        parentFd_ = -1;
      }
    }
    if (parentFd_ >= 0) {
      for (int attempt = 0; attempt < 128; ++attempt) {
        name_ = uniqueStagingName();
        if (::mkdirat(parentFd_, name_.c_str(), 0700) != 0) {
          if (errno == EEXIST) {
            continue;
          }
          break;
        }
        rootFd_ = ::openat(parentFd_, name_.c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (rootFd_ >= 0) {
          path_ = parentPath / name_;
          return true;
        }
        if (rootFd_ >= 0) {
          ::close(rootFd_);
          rootFd_ = -1;
        }
        ::unlinkat(parentFd_, name_.c_str(), AT_REMOVEDIR);
        break;
      }
    }
#endif
    diagnostics.push_back(diagnostic(
        "skin_import_staging_create_failed",
        "unable to create issued visible package staging: " +
            std::string(std::strerror(errno))));
    return false;
  }

  void cleanup() noexcept {
#if defined(_WIN32)
    if (issuedRootHandle_ == INVALID_HANDLE_VALUE) {
      closeWindowsHandles(ancestryHandles_);
      return;
    }
    clearWindowsDirectory(path_);
    markWindowsDeletion(issuedRootHandle_);
    CloseHandle(issuedRootHandle_);
    issuedRootHandle_ = INVALID_HANDLE_VALUE;
    closeWindowsHandles(ancestryHandles_);
#else
    std::ranges::sort(retainedDirectories_,
                      [](const auto &left, const auto &right) {
                        return left.depth > right.depth;
                      });
    for (const RetainedDirectory &retained : retainedDirectories_) {
      clearDirectory(retained.fd);
      ::close(retained.fd);
    }
    retainedDirectories_.clear();
    activeParentFd_ = -1;
    activeParentPath_.clear();
    activeLeaf_.clear();
    if (rootFd_ < 0) {
      if (parentFd_ >= 0) {
        ::close(parentFd_);
        parentFd_ = -1;
      }
      return;
    }
    clearDirectory(rootFd_);
    struct stat expected{};
    ::fstat(rootFd_, &expected);
    ::close(rootFd_);
    rootFd_ = -1;
    const int duplicate = ::dup(parentFd_);
    DIR *stream = duplicate >= 0 ? ::fdopendir(duplicate) : nullptr;
    if (stream != nullptr) {
      while (const dirent *entry = ::readdir(stream)) {
        const std::string_view candidate(entry->d_name);
        if (candidate == "." || candidate == "..") {
          continue;
        }
        struct stat actual{};
        if (::fstatat(parentFd_, entry->d_name, &actual, AT_SYMLINK_NOFOLLOW) ==
                0 &&
            S_ISDIR(actual.st_mode) && actual.st_dev == expected.st_dev &&
            actual.st_ino == expected.st_ino) {
          ::unlinkat(parentFd_, entry->d_name, AT_REMOVEDIR);
          break;
        }
      }
      ::closedir(stream);
    } else if (duplicate >= 0) {
      ::close(duplicate);
    }
    ::close(parentFd_);
    parentFd_ = -1;
#endif
  }

  fs::path path_;
  fs::path parentPath_;
  std::string name_;
  std::shared_ptr<const SkinImportIoObserver> observer_;
#if defined(_WIN32)
  PrivateWindowsSecurity security_;
  std::vector<HANDLE> ancestryHandles_;
  HANDLE issuedRootHandle_ = INVALID_HANDLE_VALUE;
#else
  int parentFd_ = -1;
  int rootFd_ = -1;
  std::vector<RetainedDirectory> retainedDirectories_;
  int activeParentFd_ = -1;
  std::string activeParentPath_;
  std::string activeLeaf_;
#endif
};

bool archiveMemberMatches(const ArchiveMember &expected, archive_entry *entry,
                          archive *reader) {
  const char *rawName = archive_entry_pathname_utf8(entry);
  if (rawName == nullptr) {
    rawName = archive_entry_pathname(entry);
  }
  if (rawName == nullptr || expected.archivePath != rawName ||
      (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) !=
          ARCHIVE_FORMAT_ZIP ||
      !supportedCompression(reader) || archive_entry_is_encrypted(entry) > 0 ||
      archive_entry_symlink(entry) != nullptr ||
      archive_entry_hardlink(entry) != nullptr ||
      archive_entry_sparse_count(entry) != 0) {
    return false;
  }
  const mode_t expectedType =
      expected.kind == MemberKind::Regular ? AE_IFREG : AE_IFDIR;
  return archive_entry_filetype(entry) == expectedType &&
         archive_entry_size_is_set(entry) != 0 &&
         archive_entry_size(entry) ==
             static_cast<la_int64_t>(expected.declaredSize);
}

std::uint32_t updateCrc32(std::uint32_t crc, std::span<const char> bytes) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t index = 0; index < values.size(); ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0 ? 0xedb88320U ^ (value >> 1U) : value >> 1U;
      }
      values[index] = value;
    }
    return values;
  }();
  for (const unsigned char byte : bytes) {
    crc = table[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
  }
  return crc;
}

bool extractArchive(OwnedArchiveFile &owned, ArchiveInventory &inventory,
                    SecureStagingTree &staging, std::stop_token stop,
                    const SkinProgressCallback &callback,
                    const std::shared_ptr<const SkinImportIoObserver> &observer,
                    std::vector<SkinDiagnostic> &diagnostics, bool &cancelled) {
  ArchiveHandle reader(archive_read_new());
  if (!reader || !configureArchiveReader(reader.get(), owned, diagnostics)) {
    return false;
  }
  std::array<char, 64 * 1024> buffer{};
  std::uint64_t completedBytes = 0;
  std::uint64_t completedFiles = 0;
  std::size_t index = 0;
  while (true) {
    if (stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    archive_entry *entry = nullptr;
    const int status = archive_read_next_header(reader.get(), &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status != ARCHIVE_OK || entry == nullptr ||
        index >= inventory.members.size() ||
        !archiveMemberMatches(inventory.members[index], entry, reader.get())) {
      diagnostics.push_back(diagnostic(
          "skin_archive_changed_or_corrupt",
          archiveError(reader.get(),
                       "skin ZIP changed after inventory or is corrupt")));
      return false;
    }
    ArchiveMember &member = inventory.members[index++];
    if (member.installedPath.empty()) {
      if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
        diagnostics.push_back(diagnostic(
            "skin_archive_extract_failed",
            archiveError(reader.get(), "unable to skip wrapper directory")));
        return false;
      }
      continue;
    }
    if (member.kind == MemberKind::Directory) {
      if (!staging.createDirectory(member.installedPath, observer,
                                   diagnostics) ||
          archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
        diagnostics.push_back(
            diagnostic("skin_archive_extract_failed",
                       "unable to create explicit package directory",
                       member.installedPath));
        return false;
      }
      continue;
    }

#if defined(_WIN32)
    HANDLE output =
        staging.createFile(member.installedPath, observer, diagnostics);
    if (output == INVALID_HANDLE_VALUE) {
#else
    int output =
        staging.createFile(member.installedPath, observer, diagnostics);
    if (output < 0) {
#endif
      diagnostics.push_back(diagnostic(
          "skin_archive_extract_failed",
          "unable to create extracted package file", member.installedPath));
      return false;
    }
    std::uint64_t fileBytes = 0;
    std::uint32_t crc32 = 0xffffffffU;
    file_checksum::Sha256 streamedHash;
    while (true) {
      if (stop.stop_requested()) {
#if defined(_WIN32)
        CloseHandle(output);
#else
        ::close(output);
#endif
        cancelled = true;
        return false;
      }
      const la_ssize_t count =
          archive_read_data(reader.get(), buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
#if defined(_WIN32)
        CloseHandle(output);
#else
        ::close(output);
#endif
        diagnostics.push_back(
            diagnostic("skin_archive_crc_or_read_failed",
                       archiveError(reader.get(),
                                    "unable to verify extracted package data"),
                       member.installedPath));
        return false;
      }
      const auto chunk = static_cast<std::uint64_t>(count);
      std::uint64_t nextFile = 0;
      std::uint64_t nextTotal = 0;
      if (!addWithoutOverflow(fileBytes, chunk, member.declaredSize,
                              nextFile) ||
          !addWithoutOverflow(completedBytes, chunk,
                              SkinPackagePolicy::maxExpandedBytes, nextTotal)) {
#if defined(_WIN32)
        CloseHandle(output);
#else
        ::close(output);
#endif
        diagnostics.push_back(diagnostic(
            "skin_archive_stream_limit_exceeded",
            "archive data exceeds its inventoried size", member.installedPath));
        return false;
      }
#if defined(_WIN32)
      DWORD written = 0;
      const bool wrote =
          WriteFile(output, buffer.data(), static_cast<DWORD>(count), &written,
                    nullptr) &&
          written == static_cast<DWORD>(count);
#else
      std::size_t written = 0;
      bool wrote = true;
      while (written < static_cast<std::size_t>(count)) {
        const ssize_t amount =
            ::write(output, buffer.data() + written,
                    static_cast<std::size_t>(count) - written);
        if (amount < 0 && errno == EINTR) {
          continue;
        }
        if (amount <= 0) {
          wrote = false;
          break;
        }
        written += static_cast<std::size_t>(amount);
      }
#endif
      if (!wrote) {
#if defined(_WIN32)
        CloseHandle(output);
#else
        ::close(output);
#endif
        diagnostics.push_back(diagnostic(
            "skin_archive_extract_failed",
            "unable to write extracted package data", member.installedPath));
        return false;
      }
      fileBytes = nextFile;
      completedBytes = nextTotal;
      crc32 = updateCrc32(
          crc32, std::span(buffer.data(), static_cast<std::size_t>(count)));
      streamedHash.update(std::as_bytes(
          std::span(buffer.data(), static_cast<std::size_t>(count))));
      if (!report(callback,
                  {.phase = SkinProgressPhase::Copying,
                   .completedBytes = completedBytes,
                   .totalBytes = inventory.totalBytes,
                   .completedFiles = completedFiles},
                  diagnostics)) {
#if defined(_WIN32)
        CloseHandle(output);
#else
        ::close(output);
#endif
        return false;
      }
    }
#if defined(_WIN32)
    const bool flushed = FlushFileBuffers(output) != 0;
    const bool handleClosed = CloseHandle(output) != 0;
#else
    const bool flushed = ::fsync(output) == 0;
    const bool handleClosed = ::close(output) == 0;
#endif
    if (!staging.finishFile(member.installedPath, diagnostics)) {
      return false;
    }
    if (!flushed || !handleClosed || fileBytes != member.declaredSize ||
        (crc32 ^ 0xffffffffU) != member.expectedCrc32) {
      diagnostics.push_back(
          diagnostic("skin_archive_crc_or_read_failed",
                     "extracted package data does not match its size or CRC",
                     member.installedPath));
      return false;
    }
    member.streamedSha256 = streamedHash.finalHex();
    observe(observer, SkinImportIoOperation::AfterVisibleFileWritten,
            staging.path() / pathFromUtf8(member.installedPath));
    ++completedFiles;
  }
  if (index != inventory.members.size() ||
      completedBytes != inventory.totalBytes ||
      archive_read_has_encrypted_entries(reader.get()) > 0 ||
      archive_read_close(reader.get()) != ARCHIVE_OK) {
    diagnostics.push_back(diagnostic(
        "skin_archive_changed_or_corrupt",
        archiveError(reader.get(),
                     "skin ZIP ended before its inventory was extracted")));
    return false;
  }
  return true;
}

template <typename Integer>
void hashBigEndian(file_checksum::Sha256 &hash, Integer value) {
  std::array<std::byte, sizeof(Integer)> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - index - 1] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
  hash.update(bytes);
}

void hashText(file_checksum::Sha256 &hash, std::string_view text) {
  hash.update(std::as_bytes(std::span(text.data(), text.size())));
}

std::optional<std::string>
digestArchivePayload(const ArchiveInventory &inventory,
                     const SecureStagingTree &staging, std::stop_token stop,
                     bool &cancelled,
                     std::vector<SkinDiagnostic> &diagnostics) {
  std::vector<const ArchiveMember *> files;
  files.reserve(static_cast<std::size_t>(inventory.fileCount));
  for (const ArchiveMember &member : inventory.members) {
    if (member.kind == MemberKind::Regular) {
      files.push_back(&member);
    }
  }
  std::ranges::sort(files, {}, [](const ArchiveMember *member) {
    return member->installedPath;
  });

  file_checksum::Sha256 hash;
  hashText(hash, "ASOBMSKIN-TREE-V1");
  const std::array<std::byte, 1> terminator{std::byte{0}};
  hash.update(terminator);
  hashBigEndian(hash, static_cast<std::uint64_t>(files.size()));
  std::array<char, 64 * 1024> buffer{};
  for (const ArchiveMember *member : files) {
    if (stop.stop_requested()) {
      cancelled = true;
      return std::nullopt;
    }
    hashBigEndian(hash,
                  static_cast<std::uint32_t>(member->installedPath.size()));
    hashText(hash, member->installedPath);
    hashBigEndian(hash, member->declaredSize);
#if defined(_WIN32)
    HANDLE input = staging.openFileForRead(member->installedPath);
    if (input == INVALID_HANDLE_VALUE) {
#else
    int input = staging.openFileForRead(member->installedPath);
    struct stat inputStatus{};
    if (input < 0 || ::fstat(input, &inputStatus) != 0 ||
        !S_ISREG(inputStatus.st_mode) || inputStatus.st_nlink != 1) {
      if (input >= 0) {
        ::close(input);
      }
#endif
      diagnostics.push_back(
          diagnostic("skin_archive_payload_digest_failed",
                     "unable to reopen a safely extracted archive payload",
                     member->installedPath));
      return std::nullopt;
    }
    std::uint64_t readTotal = 0;
    bool readOk = true;
    file_checksum::Sha256 stagedFileHash;
    while (readTotal < member->declaredSize) {
      if (stop.stop_requested()) {
#if defined(_WIN32)
        CloseHandle(input);
#else
        ::close(input);
#endif
        cancelled = true;
        return std::nullopt;
      }
      const std::size_t requested =
          static_cast<std::size_t>(std::min<std::uint64_t>(
              buffer.size(), member->declaredSize - readTotal));
#if defined(_WIN32)
      DWORD count = 0;
      readOk = ReadFile(input, buffer.data(), static_cast<DWORD>(requested),
                        &count, nullptr) != 0;
#else
      ssize_t count = ::read(input, buffer.data(), requested);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      readOk = count >= 0;
#endif
      if (!readOk || count == 0) {
        break;
      }
      hash.update(std::as_bytes(
          std::span(buffer.data(), static_cast<std::size_t>(count))));
      stagedFileHash.update(std::as_bytes(
          std::span(buffer.data(), static_cast<std::size_t>(count))));
      readTotal += static_cast<std::uint64_t>(count);
    }
    char extra = 0;
#if defined(_WIN32)
    DWORD extraCount = 0;
    const bool exact = readOk && readTotal == member->declaredSize &&
                       ReadFile(input, &extra, 1, &extraCount, nullptr) &&
                       extraCount == 0;
    CloseHandle(input);
#else
    ssize_t extraCount = -1;
    do {
      extraCount = ::read(input, &extra, 1);
    } while (extraCount < 0 && errno == EINTR);
    const bool exact =
        readOk && readTotal == member->declaredSize && extraCount == 0;
    ::close(input);
#endif
    if (!exact) {
      diagnostics.push_back(
          diagnostic("skin_archive_payload_digest_failed",
                     "extracted archive payload changed size while hashing",
                     member->installedPath));
      return std::nullopt;
    }
    if (member->streamedSha256.empty() ||
        stagedFileHash.finalHex() != member->streamedSha256) {
      diagnostics.push_back(
          diagnostic("skin_archive_payload_digest_mismatch",
                     "staged archive member does not match streamed ZIP bytes",
                     member->installedPath));
      return std::nullopt;
    }
  }
  return hash.finalHex();
}

bool copyStableCandidate(
    const fs::path &source, SecureStagingTree &staging, std::stop_token stop,
    const std::shared_ptr<const SkinImportIoObserver> &observer,
    std::vector<SkinDiagnostic> &diagnostics, bool &cancelled) {
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(source, error), end;
       !error && iterator != end; ++iterator) {
    if (stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    const fs::path relative = iterator->path().lexically_relative(source);
    const std::string relativeUtf8 = utf8Path(relative);
    if (iterator->is_directory(error)) {
      if (!staging.createDirectory(relativeUtf8, observer, diagnostics)) {
        error = std::make_error_code(std::errc::io_error);
      }
    } else if (iterator->is_regular_file(error)) {
      std::ifstream input(iterator->path(), std::ios::binary);
#if defined(_WIN32)
      HANDLE output = staging.createFile(relativeUtf8, observer, diagnostics);
      const bool outputValid = output != INVALID_HANDLE_VALUE;
#else
      int output = staging.createFile(relativeUtf8, observer, diagnostics);
      const bool outputValid = output >= 0;
#endif
      if (input && outputValid) {
        file_checksum::Sha256 sourceHash;
        std::array<char, 64 * 1024> buffer{};
        bool writeOk = true;
        while (input && writeOk) {
          if (stop.stop_requested()) {
#if defined(_WIN32)
            CloseHandle(output);
#else
            ::close(output);
#endif
            cancelled = true;
            return false;
          }
          input.read(buffer.data(),
                     static_cast<std::streamsize>(buffer.size()));
          const std::streamsize count = input.gcount();
          if (count > 0) {
            sourceHash.update(std::as_bytes(
                std::span(buffer.data(), static_cast<std::size_t>(count))));
#if defined(_WIN32)
            DWORD written = 0;
            writeOk = WriteFile(output, buffer.data(),
                                static_cast<DWORD>(count), &written, nullptr) &&
                      written == static_cast<DWORD>(count);
#else
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(count)) {
              const ssize_t amount =
                  ::write(output, buffer.data() + written,
                          static_cast<std::size_t>(count) - written);
              if (amount < 0 && errno == EINTR) {
                continue;
              }
              if (amount <= 0) {
                writeOk = false;
                break;
              }
              written += static_cast<std::size_t>(amount);
            }
#endif
          }
        }
        if (!input.eof() || !writeOk) {
          error = std::make_error_code(std::errc::io_error);
        } else {
          file_checksum::Sha256 copiedHash;
#if defined(_WIN32)
          LARGE_INTEGER beginning{};
          bool verificationOk =
              SetFilePointerEx(output, beginning, nullptr, FILE_BEGIN) != 0;
#else
          bool verificationOk = ::lseek(output, 0, SEEK_SET) == 0;
#endif
          while (verificationOk) {
            if (stop.stop_requested()) {
#if defined(_WIN32)
              CloseHandle(output);
#else
              ::close(output);
#endif
              cancelled = true;
              return false;
            }
#if defined(_WIN32)
            DWORD count = 0;
            verificationOk = ReadFile(output, buffer.data(),
                                      static_cast<DWORD>(buffer.size()), &count,
                                      nullptr) != 0;
#else
            ssize_t count = ::read(output, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
              continue;
            }
            verificationOk = count >= 0;
#endif
            if (!verificationOk || count == 0) {
              break;
            }
            if (count > 0) {
              copiedHash.update(std::as_bytes(
                  std::span(buffer.data(), static_cast<std::size_t>(count))));
            }
          }
          if (!verificationOk ||
              sourceHash.finalHex() != copiedHash.finalHex()) {
            error = std::make_error_code(std::errc::io_error);
          }
        }
#if defined(_WIN32)
        if (!FlushFileBuffers(output)) {
          error = std::make_error_code(std::errc::io_error);
        }
        if (!CloseHandle(output)) {
          error = std::make_error_code(std::errc::io_error);
        }
#else
        if (::fsync(output) != 0) {
          error = std::error_code(errno, std::generic_category());
        }
        ::close(output);
#endif
        if (!staging.finishFile(relativeUtf8, diagnostics)) {
          error = std::make_error_code(std::errc::io_error);
        }
      } else {
        if (outputValid) {
#if defined(_WIN32)
          CloseHandle(output);
#else
          ::close(output);
#endif
        }
        error = std::make_error_code(std::errc::io_error);
      }
    } else {
      error = std::make_error_code(std::errc::invalid_argument);
    }
    if (error) {
      break;
    }
  }
  if (error) {
    diagnostics.push_back(
        diagnostic("skin_import_visible_copy_failed",
                   "unable to copy the stable package into visible staging"));
    return false;
  }
  return true;
}

std::optional<std::vector<SkinEntryId>>
discoverEntries(const SkinRevisionReadView &view, std::stop_token stop,
                std::vector<SkinDiagnostic> &diagnostics, bool &cancelled) {
  std::vector<SkinEntryId> entries;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(view.root(), error), end;
       !error && iterator != end; ++iterator) {
    if (stop.stop_requested()) {
      cancelled = true;
      return std::nullopt;
    }
    const fs::path relative = iterator->path().lexically_relative(view.root());
    const std::string relativeUtf8 = utf8Path(relative);
    if (hasAppleDoubleComponent(relativeUtf8)) {
      diagnostics.push_back(
          diagnostic("skin_package_appledouble_rejected",
                     "AppleDouble sidecars are not allowed in skin packages",
                     relativeUtf8));
      return std::nullopt;
    }
    if (!iterator->is_regular_file(error)) {
      continue;
    }
    if (relative.extension() != ".luaskin") {
      continue;
    }
    const auto normalized =
        normalizeEntryPath(view.revision().package, relativeUtf8);
    if (!normalized.entry) {
      diagnostics.push_back(diagnostic("skin_entry_path_invalid",
                                       normalized.error, relativeUtf8));
      return std::nullopt;
    }
    entries.push_back(std::move(*normalized.entry));
  }
  if (error) {
    diagnostics.push_back(diagnostic(
        "skin_entry_inventory_failed",
        "unable to inventory entries in the stable private candidate"));
    return std::nullopt;
  }
  std::ranges::sort(entries, {}, &SkinEntryId::packageRelativePath);
  return entries;
}

void appendSnapshotFailure(SnapshotTreeResult &snapshot,
                           PreparePackageResult &result) {
  result.cancelled = snapshot.cancelled;
  result.diagnostics.insert(
      result.diagnostics.end(),
      std::make_move_iterator(snapshot.diagnostics.begin()),
      std::make_move_iterator(snapshot.diagnostics.end()));
}

} // namespace

struct PreparedPackage::State {
  State(PreparedSkinRevision revisionValue,
        std::vector<SkinEntryId> entriesValue, fs::path visibleValue,
        std::shared_ptr<void> visibleOwnerValue)
      : revision(std::move(revisionValue)), entries(std::move(entriesValue)),
        visibleStagingRoot(std::move(visibleValue)),
        visibleOwner(std::move(visibleOwnerValue)) {}

  PreparedSkinRevision revision;
  std::vector<SkinEntryId> entries;
  fs::path visibleStagingRoot;
  std::shared_ptr<void> visibleOwner;
};

PreparedPackage::PreparedPackage(PreparedSkinRevision revision,
                                 std::vector<SkinEntryId> entries,
                                 fs::path visibleStagingRoot,
                                 std::shared_ptr<void> visibleOwner)
    : state_(std::make_unique<State>(std::move(revision), std::move(entries),
                                     std::move(visibleStagingRoot),
                                     std::move(visibleOwner))) {}

PreparedPackage::PreparedPackage(PreparedPackage &&) noexcept = default;
PreparedPackage &
PreparedPackage::operator=(PreparedPackage &&) noexcept = default;
PreparedPackage::~PreparedPackage() = default;

const SkinPackageId &PreparedPackage::packageId() const noexcept {
  assert(state_ != nullptr);
  return state_->revision.revision().package;
}

const SkinRevision &PreparedPackage::candidateRevision() const noexcept {
  assert(state_ != nullptr);
  return state_->revision.revision();
}

std::span<const SkinEntryId> PreparedPackage::entries() const noexcept {
  assert(state_ != nullptr);
  return state_->entries;
}

const fs::path &PreparedPackage::visibleStagingRoot() const noexcept {
  assert(state_ != nullptr);
  return state_->visibleStagingRoot;
}

SkinRevisionReadView PreparedPackage::readView() const noexcept {
  assert(state_ != nullptr);
  return state_->revision.readView();
}

std::optional<SkinRevisionLease>
PreparedPackage::publishRevision(std::string &error) {
  assert(state_ != nullptr);
  return std::move(state_->revision).publish(error);
}

bool PreparedPackage::renameVisibleStagingTo(
    const fs::path &destination, std::vector<SkinDiagnostic> &diagnostics) {
  assert(state_ != nullptr);
  const auto owner =
      std::static_pointer_cast<SecureStagingTree>(state_->visibleOwner);
  if (!owner || !owner->renameTo(destination, diagnostics)) {
    return false;
  }
  state_->visibleStagingRoot = destination;
  state_->revision.relocateLiveSourceTo(destination);
  return true;
}

bool PreparedPackage::relocateVisibleOwnershipTo(
    const fs::path &destination, std::vector<SkinDiagnostic> &diagnostics) {
  assert(state_ != nullptr);
  const auto owner =
      std::static_pointer_cast<SecureStagingTree>(state_->visibleOwner);
  if (!owner || !owner->renameTo(destination, diagnostics, false)) {
    return false;
  }
  state_->visibleStagingRoot = destination;
  state_->revision.relocateLiveSourceTo(destination);
  return true;
}

void PreparedPackage::releaseVisibleOwnership() noexcept {
  if (!state_) {
    return;
  }
  const auto owner =
      std::static_pointer_cast<SecureStagingTree>(state_->visibleOwner);
  if (owner) {
    owner->releaseOwnership();
  }
  state_->visibleOwner.reset();
}

SkinArchiveImporter::SkinArchiveImporter(
    SkinStorageRoots roots, const SkinAliasDetector &aliases,
    std::shared_ptr<const SkinImportIoObserver> observer)
    : roots_(std::move(roots)), aliases_(aliases),
      observer_(std::move(observer)) {}

PreparePackageResult SkinArchiveImporter::prepareArchive(
    const fs::path &archivePath, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback callback) {
  PreparePackageResult result;
  const auto normalizedPackage = normalizePackageId(package.directoryName);
  if (!normalizedPackage.package ||
      normalizedPackage.package->collisionKey != package.collisionKey) {
    result.diagnostics.push_back(diagnostic(
        "skin_import_package_invalid", "skin package identity is invalid"));
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto owned = copyArchiveSource(archivePath, stop, result.cancelled, observer_,
                                 result.diagnostics);
  if (!owned) {
    return result;
  }
  RawZipMembers rawMembers;
  if (!validateRawZipEnvelope(
          *owned, owned->bytes(), *normalizedPackage.package, stop,
          result.cancelled, observer_, rawMembers, result.diagnostics)) {
    return result;
  }
  const auto beforeDigest =
      hashOwnedArchive(*owned, stop, result.cancelled, observer_);
  if (!beforeDigest) {
    if (!result.cancelled) {
      result.diagnostics.push_back(diagnostic("skin_archive_input_read_failed",
                                              "unable to hash owned skin ZIP"));
    }
    return result;
  }
  auto inventory =
      inventoryArchive(*owned, *normalizedPackage.package, rawMembers, stop,
                       callback, result.diagnostics);
  if (!inventory) {
    result.cancelled = stop.stop_requested();
    return result;
  }
  if (!report(callback,
              {.phase = SkinProgressPhase::Inspecting,
               .completedBytes = inventory->totalBytes,
               .totalBytes = inventory->totalBytes,
               .completedFiles = inventory->fileCount},
              result.diagnostics)) {
    return result;
  }
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }

  auto visibleStaging =
      SecureStagingTree::create(roots_, observer_, result.diagnostics);
  if (!visibleStaging) {
    return result;
  }
  if (!extractArchive(*owned, *inventory, *visibleStaging, stop, callback,
                      observer_, result.diagnostics, result.cancelled)) {
    return result;
  }
  const auto afterDigest =
      hashOwnedArchive(*owned, stop, result.cancelled, observer_);
  if (!afterDigest || *afterDigest != *beforeDigest) {
    if (!result.cancelled) {
      result.diagnostics.push_back(diagnostic(
          "skin_archive_source_changed",
          "owned skin archive changed between inventory and extraction"));
    }
    return result;
  }
  const auto payloadDigest = digestArchivePayload(
      *inventory, *visibleStaging, stop, result.cancelled, result.diagnostics);
  if (!payloadDigest) {
    return result;
  }
  if (!visibleStaging->verifyIdentity(result.diagnostics)) {
    return result;
  }
  observe(observer_, SkinImportIoOperation::BeforeVisibleSnapshot,
          visibleStaging->path());

  SkinTreeSnapshotter snapshotter(roots_, aliases_);
  auto snapshot =
      snapshotter.snapshot(visibleStaging->path(), *normalizedPackage.package,
                           stop, std::move(callback));
  if (!snapshot.prepared) {
    appendSnapshotFailure(snapshot, result);
    return result;
  }
  if (snapshot.prepared->revision().lowercaseSha256 != *payloadDigest) {
    result.diagnostics.push_back(diagnostic(
        "skin_archive_payload_digest_mismatch",
        "Task5 candidate does not match the streamed archive payload digest"));
    return result;
  }
  auto entries = discoverEntries(snapshot.prepared->readView(), stop,
                                 result.diagnostics, result.cancelled);
  if (!entries) {
    return result;
  }
  const fs::path visibleStagingPath = visibleStaging->path();
  result.prepared =
      PreparedPackage(std::move(*snapshot.prepared), std::move(*entries),
                      visibleStagingPath, std::move(visibleStaging));
  return result;
}

PreparePackageResult SkinArchiveImporter::prepareFolder(
    const fs::path &folder, const SkinPackageId &package, std::stop_token stop,
    SkinProgressCallback callback) {
  PreparePackageResult result;
  const auto normalizedPackage = normalizePackageId(package.directoryName);
  if (!normalizedPackage.package ||
      normalizedPackage.package->collisionKey != package.collisionKey) {
    result.diagnostics.push_back(diagnostic(
        "skin_import_package_invalid", "skin package identity is invalid"));
    return result;
  }
  std::error_code identityError;
  bool isVisibleRoot = false;
  if (!roots_.visiblePackages.empty()) {
    const fs::path folderAbsolute = fs::absolute(folder, identityError);
    if (!identityError) {
      const fs::path visibleAbsolute =
          fs::absolute(roots_.visiblePackages, identityError);
      if (!identityError && folderAbsolute.lexically_normal() ==
                                visibleAbsolute.lexically_normal()) {
        isVisibleRoot = true;
      }
    }
    identityError.clear();
    if (!isVisibleRoot && fs::exists(folder, identityError) &&
        fs::exists(roots_.visiblePackages, identityError)) {
      identityError.clear();
      isVisibleRoot =
          fs::equivalent(folder, roots_.visiblePackages, identityError) &&
          !identityError;
    }
  }
  if (isVisibleRoot) {
    result.diagnostics.push_back(diagnostic(
        "skin_import_visible_root_rejected",
        "the canonical Skins root is not itself one package; choose one "
        "direct-child package folder"));
    return result;
  }
  SkinTreeSnapshotter snapshotter(roots_, aliases_);
  auto snapshot = snapshotter.snapshot(folder, *normalizedPackage.package, stop,
                                       std::move(callback));
  if (!snapshot.prepared) {
    appendSnapshotFailure(snapshot, result);
    return result;
  }
  auto entries = discoverEntries(snapshot.prepared->readView(), stop,
                                 result.diagnostics, result.cancelled);
  if (!entries) {
    return result;
  }
  auto visibleStaging =
      SecureStagingTree::create(roots_, observer_, result.diagnostics);
  if (!visibleStaging) {
    return result;
  }
  if (!copyStableCandidate(snapshot.prepared->stagingRoot(), *visibleStaging,
                           stop, observer_, result.diagnostics,
                           result.cancelled)) {
    return result;
  }
  if (!visibleStaging->verifyIdentity(result.diagnostics)) {
    return result;
  }
  const fs::path visibleStagingPath = visibleStaging->path();
  if (roots_.liveSources) {
    // The direct-source lease must describe the exact tree that will be
    // renamed into Documents/Skins. The incoming folder remains user-owned
    // and can be edited while it is being imported.
    auto staged = snapshotter.snapshot(visibleStagingPath,
                                       *normalizedPackage.package, stop, {});
    if (!staged.prepared) {
      appendSnapshotFailure(staged, result);
      return result;
    }
    auto stagedEntries = discoverEntries(staged.prepared->readView(), stop,
                                         result.diagnostics, result.cancelled);
    if (!stagedEntries) {
      return result;
    }
    result.prepared =
        PreparedPackage(std::move(*staged.prepared), std::move(*stagedEntries),
                        visibleStagingPath, std::move(visibleStaging));
    return result;
  }
  result.prepared =
      PreparedPackage(std::move(*snapshot.prepared), std::move(*entries),
                      visibleStagingPath, std::move(visibleStaging));
  return result;
}

} // namespace skin
