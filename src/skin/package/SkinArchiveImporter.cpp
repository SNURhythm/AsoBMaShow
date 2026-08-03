#include "SkinArchiveImporter.h"

#include "../../FileChecksum.h"
#include "SkinPathPolicy.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

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

  auto operator<=>(const ArchiveMember &) const = default;
};

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

bool readAt(std::ifstream &input, std::uint64_t offset,
            std::span<unsigned char> bytes) {
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!input) {
    return false;
  }
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return input.gcount() == static_cast<std::streamsize>(bytes.size());
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
                          std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

std::string archiveError(archive *reader, std::string_view fallback) {
  const char *message = archive_error_string(reader);
  return message == nullptr ? std::string(fallback) : std::string(message);
}

int openArchive(archive *reader, const fs::path &path) {
#if defined(_WIN32)
  return archive_read_open_filename_w(reader, path.c_str(), 64 * 1024);
#else
  return archive_read_open_filename(reader, path.c_str(), 64 * 1024);
#endif
}

bool configureArchiveReader(archive *reader, const fs::path &path,
                            std::vector<SkinDiagnostic> &diagnostics) {
  if (archive_read_support_filter_none(reader) != ARCHIVE_OK ||
      archive_read_support_format_zip(reader) != ARCHIVE_OK ||
      archive_read_set_format_option(reader, "zip", "mac-ext", nullptr) !=
          ARCHIVE_OK ||
      openArchive(reader, path) != ARCHIVE_OK) {
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

bool validateRawZipEnvelope(const fs::path &path, std::uint64_t archiveBytes,
                            const SkinPackageId &package,
                            std::vector<SkinDiagnostic> &diagnostics) {
  constexpr std::uint64_t maximumTail = 65'557;
  const std::uint64_t tailSize = std::min(archiveBytes, maximumTail);
  if (tailSize < 22) {
    diagnostics.push_back(diagnostic("skin_archive_eocd_invalid",
                                     "skin ZIP has no complete end record"));
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> tail(static_cast<std::size_t>(tailSize));
  if (!input || !readAt(input, archiveBytes - tailSize, tail)) {
    diagnostics.push_back(diagnostic("skin_archive_input_read_failed",
                                     "unable to read skin ZIP metadata"));
    return false;
  }
  std::optional<std::size_t> eocd;
  for (std::size_t index = tail.size() - 22;; --index) {
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
  if (disk != 0 || directoryDisk != 0 || diskRecords != totalRecords ||
      totalRecords == 0xffffU || directoryBytes == 0xffffffffU ||
      directoryOffset == 0xffffffffU ||
      totalRecords > SkinPackagePolicy::maxFiles ||
      static_cast<std::uint64_t>(directoryOffset) + directoryBytes >
          archiveBytes) {
    diagnostics.push_back(diagnostic(
        "skin_archive_directory_invalid",
        "skin ZIP uses multi-disk/ZIP64 metadata or exceeds the member limit"));
    return false;
  }

  std::uint64_t cursor = directoryOffset;
  for (std::uint16_t index = 0; index < totalRecords; ++index) {
    std::array<unsigned char, 46> central{};
    if (!readAt(input, cursor, central) ||
        little32(central.data()) != 0x02014b50U) {
      diagnostics.push_back(
          diagnostic("skin_archive_directory_invalid",
                     "skin ZIP central directory is truncated or malformed"));
      return false;
    }
    const std::uint16_t flags = little16(central.data() + 8);
    const std::uint16_t method = little16(central.data() + 10);
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
    if (!readAt(input, cursor + 46, name)) {
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
    if (!readAt(input, localOffset, local) ||
        little32(local.data()) != 0x04034b50U ||
        little16(local.data() + 6) != flags ||
        little16(local.data() + 8) != method ||
        little16(local.data() + 26) != nameBytes) {
      diagnostics.push_back(diagnostic(
          "skin_archive_local_header_mismatch",
          "skin ZIP local and central headers disagree", structuralName));
      return false;
    }
    std::vector<unsigned char> localName(nameBytes);
    if (!readAt(input, static_cast<std::uint64_t>(localOffset) + 30,
                localName) ||
        localName != name) {
      diagnostics.push_back(diagnostic(
          "skin_archive_local_header_mismatch",
          "skin ZIP local and central member names disagree", structuralName));
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
inventoryArchive(const fs::path &path, const SkinPackageId &package,
                 std::stop_token stop, const SkinProgressCallback &callback,
                 std::vector<SkinDiagnostic> &diagnostics) {
  ArchiveHandle reader(archive_read_new());
  if (!reader || !configureArchiveReader(reader.get(), path, diagnostics)) {
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
    if (memberCount > SkinPackagePolicy::maxFiles) {
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
         .declaredSize = declaredSize});
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

bool makeVisibleStagingRoot(const SkinStorageRoots &roots, fs::path &root,
                            std::vector<SkinDiagnostic> &diagnostics) {
  if (roots.visiblePackages.empty() || !roots.visiblePackages.is_absolute()) {
    diagnostics.push_back(
        diagnostic("skin_import_visible_root_invalid",
                   "visible skin package storage is unavailable"));
    return false;
  }
  std::error_code error;
  const fs::path parent =
      roots.visiblePackages.parent_path() / ".skin-import-staging";
  fs::create_directories(parent, error);
  if (!error) {
    root = parent / uniqueStagingName();
    fs::create_directory(root, error);
  }
  if (error) {
    diagnostics.push_back(
        diagnostic("skin_import_staging_create_failed",
                   "unable to create unpublished visible package staging"));
    root.clear();
    return false;
  }
  return true;
}

struct MutableTreeCleanup {
  fs::path path;
  ~MutableTreeCleanup() {
    if (path.empty()) {
      return;
    }
    std::error_code ignored;
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ignored);
    for (fs::recursive_directory_iterator iterator(path, ignored), end;
         !ignored && iterator != end; ++iterator) {
      fs::permissions(iterator->path(), fs::perms::owner_all,
                      fs::perm_options::add, ignored);
    }
    ignored.clear();
    fs::remove_all(path, ignored);
  }
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

bool extractArchive(const fs::path &archivePath,
                    const ArchiveInventory &inventory,
                    const fs::path &destination, std::stop_token stop,
                    const SkinProgressCallback &callback,
                    std::vector<SkinDiagnostic> &diagnostics, bool &cancelled) {
  ArchiveHandle reader(archive_read_new());
  if (!reader ||
      !configureArchiveReader(reader.get(), archivePath, diagnostics)) {
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
    const ArchiveMember &member = inventory.members[index++];
    if (member.installedPath.empty()) {
      if (archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
        diagnostics.push_back(diagnostic(
            "skin_archive_extract_failed",
            archiveError(reader.get(), "unable to skip wrapper directory")));
        return false;
      }
      continue;
    }
    const fs::path output = destination / pathFromUtf8(member.installedPath);
    std::error_code error;
    if (member.kind == MemberKind::Directory) {
      fs::create_directories(output, error);
      if (error || archive_read_data_skip(reader.get()) != ARCHIVE_OK) {
        diagnostics.push_back(
            diagnostic("skin_archive_extract_failed",
                       "unable to create explicit package directory",
                       member.installedPath));
        return false;
      }
      continue;
    }

    fs::create_directories(output.parent_path(), error);
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (error || !stream) {
      diagnostics.push_back(diagnostic(
          "skin_archive_extract_failed",
          "unable to create extracted package file", member.installedPath));
      return false;
    }
    std::uint64_t fileBytes = 0;
    while (true) {
      if (stop.stop_requested()) {
        cancelled = true;
        return false;
      }
      const la_ssize_t count =
          archive_read_data(reader.get(), buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
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
        diagnostics.push_back(diagnostic(
            "skin_archive_stream_limit_exceeded",
            "archive data exceeds its inventoried size", member.installedPath));
        return false;
      }
      stream.write(buffer.data(), count);
      if (!stream) {
        diagnostics.push_back(diagnostic(
            "skin_archive_extract_failed",
            "unable to write extracted package data", member.installedPath));
        return false;
      }
      fileBytes = nextFile;
      completedBytes = nextTotal;
      if (!report(callback,
                  {.phase = SkinProgressPhase::Copying,
                   .completedBytes = completedBytes,
                   .totalBytes = inventory.totalBytes,
                   .completedFiles = completedFiles},
                  diagnostics)) {
        return false;
      }
    }
    stream.close();
    if (!stream || fileBytes != member.declaredSize) {
      diagnostics.push_back(
          diagnostic("skin_archive_size_mismatch",
                     "extracted package data does not match its declared size",
                     member.installedPath));
      return false;
    }
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

bool copyStableCandidate(const fs::path &source, const fs::path &destination,
                         std::stop_token stop,
                         std::vector<SkinDiagnostic> &diagnostics,
                         bool &cancelled) {
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(source, error), end;
       !error && iterator != end; ++iterator) {
    if (stop.stop_requested()) {
      cancelled = true;
      return false;
    }
    const fs::path relative = iterator->path().lexically_relative(source);
    const fs::path output = destination / relative;
    if (iterator->is_directory(error)) {
      fs::create_directories(output, error);
    } else if (iterator->is_regular_file(error)) {
      fs::create_directories(output.parent_path(), error);
      if (!error) {
        std::ifstream input(iterator->path(), std::ios::binary);
        std::ofstream copied(output, std::ios::binary | std::ios::trunc);
        file_checksum::Sha256 sourceHash;
        std::array<char, 64 * 1024> buffer{};
        while (input && copied) {
          if (stop.stop_requested()) {
            cancelled = true;
            return false;
          }
          input.read(buffer.data(),
                     static_cast<std::streamsize>(buffer.size()));
          const std::streamsize count = input.gcount();
          if (count > 0) {
            sourceHash.update(std::as_bytes(
                std::span(buffer.data(), static_cast<std::size_t>(count))));
            copied.write(buffer.data(), count);
          }
        }
        copied.close();
        if (!input.eof() || !copied) {
          error = std::make_error_code(std::errc::io_error);
        } else {
          std::ifstream verification(output, std::ios::binary);
          file_checksum::Sha256 copiedHash;
          while (verification) {
            if (stop.stop_requested()) {
              cancelled = true;
              return false;
            }
            verification.read(buffer.data(),
                              static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = verification.gcount();
            if (count > 0) {
              copiedHash.update(std::as_bytes(
                  std::span(buffer.data(), static_cast<std::size_t>(count))));
            }
          }
          if (!verification.eof() ||
              sourceHash.finalHex() != copiedHash.finalHex()) {
            error = std::make_error_code(std::errc::io_error);
          }
        }
      }
      if (!error) {
        fs::permissions(output, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::add, error);
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
        std::vector<SkinEntryId> entriesValue, fs::path visibleValue)
      : revision(std::move(revisionValue)), entries(std::move(entriesValue)),
        visibleStagingRoot(std::move(visibleValue)) {}

  PreparedSkinRevision revision;
  std::vector<SkinEntryId> entries;
  fs::path visibleStagingRoot;

  ~State() { MutableTreeCleanup cleanup{visibleStagingRoot}; }
};

PreparedPackage::PreparedPackage(PreparedSkinRevision revision,
                                 std::vector<SkinEntryId> entries,
                                 fs::path visibleStagingRoot)
    : state_(std::make_unique<State>(std::move(revision), std::move(entries),
                                     std::move(visibleStagingRoot))) {}

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

SkinArchiveImporter::SkinArchiveImporter(SkinStorageRoots roots,
                                         const SkinAliasDetector &aliases)
    : roots_(std::move(roots)), aliases_(aliases) {}

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
  std::error_code error;
  const fs::file_status status = fs::symlink_status(archivePath, error);
  if (error || !fs::is_regular_file(status)) {
    result.diagnostics.push_back(
        diagnostic("skin_archive_input_invalid",
                   "skin archive is missing or is not a regular file"));
    return result;
  }
  const std::uint64_t archiveBytes = fs::file_size(archivePath, error);
  if (error || archiveBytes > SkinPackagePolicy::maxArchiveBytes) {
    result.diagnostics.push_back(
        diagnostic("skin_archive_input_too_large",
                   "skin archive exceeds the package archive-byte limit"));
    return result;
  }
  if (!validateRawZipEnvelope(archivePath, archiveBytes,
                              *normalizedPackage.package, result.diagnostics)) {
    return result;
  }
  std::string digestError;
  const auto beforeDigest = file_checksum::sha256File(
      archivePath, digestError, SkinPackagePolicy::maxArchiveBytes);
  if (!beforeDigest) {
    result.diagnostics.push_back(
        diagnostic("skin_archive_input_read_failed", std::move(digestError)));
    return result;
  }
  auto inventory = inventoryArchive(archivePath, *normalizedPackage.package,
                                    stop, callback, result.diagnostics);
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

  fs::path visibleStaging;
  if (!makeVisibleStagingRoot(roots_, visibleStaging, result.diagnostics)) {
    return result;
  }
  MutableTreeCleanup cleanup{visibleStaging};
  if (!extractArchive(archivePath, *inventory, visibleStaging, stop, callback,
                      result.diagnostics, result.cancelled)) {
    return result;
  }
  const auto afterDigest = file_checksum::sha256File(
      archivePath, digestError, SkinPackagePolicy::maxArchiveBytes);
  if (!afterDigest || *afterDigest != *beforeDigest) {
    result.diagnostics.push_back(
        diagnostic("skin_archive_source_changed",
                   "skin archive changed between inventory and extraction"));
    return result;
  }

  SkinTreeSnapshotter snapshotter(roots_, aliases_);
  auto snapshot = snapshotter.snapshot(
      visibleStaging, *normalizedPackage.package, stop, std::move(callback));
  if (!snapshot.prepared) {
    appendSnapshotFailure(snapshot, result);
    return result;
  }
  auto entries = discoverEntries(snapshot.prepared->readView(), stop,
                                 result.diagnostics, result.cancelled);
  if (!entries) {
    return result;
  }
  result.prepared = PreparedPackage(std::move(*snapshot.prepared),
                                    std::move(*entries), visibleStaging);
  cleanup.path.clear();
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
  fs::path visibleStaging;
  if (!makeVisibleStagingRoot(roots_, visibleStaging, result.diagnostics)) {
    return result;
  }
  MutableTreeCleanup cleanup{visibleStaging};
  if (!copyStableCandidate(snapshot.prepared->stagingRoot(), visibleStaging,
                           stop, result.diagnostics, result.cancelled)) {
    return result;
  }
  result.prepared = PreparedPackage(std::move(*snapshot.prepared),
                                    std::move(*entries), visibleStaging);
  cleanup.path.clear();
  return result;
}

} // namespace skin
