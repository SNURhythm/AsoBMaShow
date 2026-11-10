#include "DownloadStaging.h"
#include "DownloadStorageIdentity.h"
#include "../downloads/DownloadPublicationPaths.h"

#include "../ArchiveFile.h"
#include "../Uuid.h"
#include "../RAII.h"
#include "../sqlite3.h"
#include "../targets.h"
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
#include "../downloads/DownloadPlatform.h"
#endif

#include <array>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace asobmshow::bms_search {
namespace {

std::filesystem::path normalizedPath(const std::filesystem::path &path,
                                     std::error_code &error) {
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return normalized.lexically_normal();
  }
  error.clear();
  normalized = std::filesystem::absolute(path, error);
  if (error) {
    return {};
  }
  return normalized.lexically_normal();
}

bool isStrictDescendant(const std::filesystem::path &path,
                        const std::filesystem::path &parent) {
  auto pathIterator = path.begin();
  auto parentIterator = parent.begin();
  for (; parentIterator != parent.end(); ++parentIterator, ++pathIterator) {
    if (pathIterator == path.end() || *pathIterator != *parentIterator) {
      return false;
    }
  }
  return pathIterator != path.end();
}

bool isSafePathComponent(const std::string &value) {
  const std::filesystem::path path(value);
  return !value.empty() && path != "." && path != ".." &&
         path.filename() == path;
}

bool isRetainedJobArchive(const std::filesystem::path &source,
                          const std::filesystem::path &stagingRoot,
                          const std::filesystem::path &base) {
  // Durable jobs retain their immutable archive beside imports/<UUID>. Only
  // that same job's numbered archive may be borrowed by the staging workflow.
  if (source.parent_path().parent_path() != base.parent_path()) return false;
  auto id = source.parent_path().filename().string();
  if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
  id[12] = '4'; id[16] = '8';
  id.insert(20, "-"); id.insert(16, "-"); id.insert(12, "-"); id.insert(8, "-");
  if (stagingRoot.filename() != id) return false;
  auto name = source.filename().string();
  const auto extension = archive_file::archiveExtensionFromName(name);
  if (extension.empty()) return false;
  name.resize(name.size() - extension.size());
  if (name != "archive" && (!name.starts_with("archive.") || name.size() == 8 ||
      name.find_first_not_of("0123456789", 8) != std::string::npos)) return false;
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::symlink_status(source, error)) && !error;
}

bool validatePendingArtifact(const BmsSearchPendingArtifact &artifact,
                             std::string &errorMessage) {
  std::error_code error;
  const auto base = normalizedPath(findBmsStagingBasePath(), error);
  if (error || base.empty()) {
    errorMessage = "Could not resolve the Find BMS staging folder.";
    return false;
  }
  const auto stagingRoot = normalizedPath(artifact.stagingRoot, error);
  if (error || stagingRoot.empty() || stagingRoot.parent_path() != base ||
      !uuid::isCanonicalLowerV4(stagingRoot.filename().string())) {
    errorMessage = "Refusing an unsafe Find BMS staging path.";
    return false;
  }
  const auto sourcePath = normalizedPath(artifact.sourcePath, error);
  const bool retainedArchive = !error && artifact.kind == BmsSearchPendingArtifactKind::Archive &&
      !std::filesystem::is_symlink(std::filesystem::symlink_status(artifact.sourcePath, error)) && !error &&
      isRetainedJobArchive(sourcePath, stagingRoot, base);
  if (error || sourcePath.empty() ||
      (!isStrictDescendant(sourcePath, stagingRoot) && !retainedArchive)) {
    errorMessage = "Refusing an unsafe Find BMS source path.";
    return false;
  }
  const auto downloadRoot = normalizedPath(artifact.downloadRoot, error);
  if (error || downloadRoot.empty() ||
      downloadRoot.filename() != "BMSSEARCH" || downloadRoot == base ||
      isStrictDescendant(downloadRoot, base)) {
    errorMessage = "Refusing an unsafe Find BMS download folder.";
    return false;
  }
  const auto destinationPath =
      normalizedPath(artifact.destinationPath, error);
  if (error || destinationPath.empty() ||
      !isStrictDescendant(destinationPath, downloadRoot)) {
    errorMessage = "Refusing an unsafe Find BMS destination path.";
    return false;
  }

  const bool hasAlternateMetadata =
      !artifact.archiveName.empty() || !artifact.storageKey.empty() ||
      !artifact.alternateDestinationPath.empty();
  if (hasAlternateMetadata) {
    if (!isSafePathComponent(artifact.archiveName) ||
        !isSafePathComponent(artifact.storageKey) ||
        artifact.storageKey == "_archives" ||
        artifact.alternateDestinationPath.empty()) {
      errorMessage = "Refusing invalid Find BMS alternate metadata.";
      return false;
    }
    const auto alternateDestinationPath =
        normalizedPath(artifact.alternateDestinationPath, error);
    if (error || alternateDestinationPath.empty() ||
        !isStrictDescendant(alternateDestinationPath, downloadRoot)) {
      errorMessage = "Refusing an unsafe Find BMS alternate path.";
      return false;
    }
    const auto expectedArchivePath = normalizedPath(
        downloadRoot / "_archives" / artifact.archiveName, error);
    const auto expectedExtractedPath =
        normalizedPath(downloadRoot / artifact.storageKey, error);
    const auto expectedArchiveSource =
        normalizedPath(stagingRoot / artifact.archiveName, error);
    if (error || expectedArchivePath.empty() || expectedExtractedPath.empty() ||
        expectedArchiveSource.empty()) {
      errorMessage = "Could not resolve Find BMS alternate metadata.";
      return false;
    }
    const bool pathsMatch =
        artifact.kind == BmsSearchPendingArtifactKind::Archive
            ? (sourcePath == expectedArchiveSource || retainedArchive) &&
                  destinationPath == expectedArchivePath &&
                  alternateDestinationPath == expectedExtractedPath
            : destinationPath == expectedExtractedPath &&
                  alternateDestinationPath == expectedArchivePath;
    if (!pathsMatch) {
      errorMessage = "Refusing mismatched Find BMS alternate metadata.";
      return false;
    }
  }
  if (artifact.kind == BmsSearchPendingArtifactKind::Archive) {
    if ((!retainedArchive && sourcePath.parent_path() != stagingRoot) ||
        destinationPath.parent_path() != downloadRoot / "_archives") {
      errorMessage = "Refusing invalid Find BMS archive metadata.";
      return false;
    }
  } else if (sourcePath != stagingRoot / "extracted" ||
             destinationPath.parent_path() != downloadRoot) {
    errorMessage = "Refusing invalid Find BMS extracted-folder metadata.";
    return false;
  }
  if (!artifact.originalFilesIndexPath.empty()) {
    const auto indexPath = normalizedPath(artifact.originalFilesIndexPath, error);
    if (error || artifact.kind != BmsSearchPendingArtifactKind::ExtractedDirectory ||
        indexPath != stagingRoot / "original-files.sqlite" ||
        !std::filesystem::is_regular_file(std::filesystem::symlink_status(artifact.originalFilesIndexPath, error)) || error) {
      errorMessage = "Refusing an unsafe Find BMS original-file index.";
      return false;
    }
  }
  return true;
}

bool copyDirectoryContents(const std::filesystem::path &source,
                           const std::filesystem::path &destination,
                           std::error_code &error) {
  std::filesystem::create_directories(destination, error);
  if (error) {
    return false;
  }
  for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto target = destination / iterator->path().lexically_relative(source);
    const auto sourceStatus = iterator->symlink_status(error);
    if (error) break;
    const auto targetStatus = std::filesystem::symlink_status(target, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    if (error) break;
    // copy_symlinks only controls source links. An overwrite would still
    // follow a preserved destination link, including a dangling one. Visit
    // directories before their children so no parent link can redirect writes.
    if (std::filesystem::is_symlink(targetStatus)) {
      std::filesystem::remove(target, error);
      if (error) break;
    }
    if (std::filesystem::is_directory(sourceStatus))
      std::filesystem::create_directories(target, error);
    else if (std::filesystem::is_regular_file(sourceStatus))
      std::filesystem::copy_file(iterator->path(), target,
          std::filesystem::copy_options::overwrite_existing, error);
    else if (std::filesystem::is_symlink(sourceStatus))
      std::filesystem::copy_symlink(iterator->path(), target, error);
    else
      error = std::make_error_code(std::errc::operation_not_supported);
  }
  return !error;
}

struct DestinationObjectIdentity {
  std::array<std::uint64_t, 2> key;
  std::uint64_t links;
};

// Unlike optimizer input identity, promotion must also recognize hard links
// and symlinks so an ambiguous alias can fail without removing unrelated names.
std::optional<DestinationObjectIdentity> destinationObjectIdentity(
    const std::filesystem::path &path, std::error_code &error) {
#ifdef _WIN32
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const auto code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
      error = std::error_code(code, std::system_category());
    return {};
  }
  BY_HANDLE_FILE_INFORMATION info{};
  const bool valid = GetFileInformationByHandle(handle, &info);
  const auto code = valid ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!valid) { error = std::error_code(code, std::system_category()); return {}; }
  return DestinationObjectIdentity{{info.dwVolumeSerialNumber,
      (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow}, info.nNumberOfLinks};
#else
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) {
    if (errno != ENOENT && errno != ENOTDIR) error = std::error_code(errno, std::generic_category());
    return {};
  }
  return DestinationObjectIdentity{{static_cast<std::uint64_t>(info.st_dev),
      static_cast<std::uint64_t>(info.st_ino)}, static_cast<std::uint64_t>(info.st_nlink)};
#endif
}

bool indexRetiredDestinationObjects(const BmsSearchPendingArtifact &artifact,
                                    sqlite3 *database, std::error_code &error) {
  const auto failed = [&] { error = std::make_error_code(std::errc::io_error); return false; };
  if (sqlite3_exec(database,
      "PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-1024;"
      "CREATE TEMP TABLE retired_objects(id BLOB PRIMARY KEY) WITHOUT ROWID; BEGIN;",
      nullptr, nullptr, nullptr) != SQLITE_OK) return failed();
  sqlite3_stmt *rows = nullptr, *insert = nullptr;
  const auto selected = sqlite3_prepare_v2(database, "SELECT path FROM original_files", -1, &rows, nullptr);
  UniqueResource<sqlite3_stmt, sqlite3_finalize> rowOwner(rows);
  const auto prepared = sqlite3_prepare_v2(database, "INSERT OR IGNORE INTO retired_objects VALUES(?)", -1, &insert, nullptr);
  UniqueResource<sqlite3_stmt, sqlite3_finalize> insertOwner(insert);
  if (selected != SQLITE_OK || prepared != SQLITE_OK) return failed();
  int step;
  while ((step = sqlite3_step(rows)) == SQLITE_ROW) {
    const auto bytes = sqlite3_column_bytes(rows, 0);
    const auto *name = static_cast<const char8_t *>(sqlite3_column_blob(rows, 0));
    if (!name || bytes <= 0) return failed();
    const std::filesystem::path relative(std::u8string(name, bytes));
    if (relative.has_root_path()) return failed();
    for (const auto &part : relative) if (part == "..") return failed();
    if (std::filesystem::exists(artifact.sourcePath / relative, error)) continue;
    if (error) return false;
    const auto identity = destinationObjectIdentity(artifact.destinationPath / relative, error);
    if (error) return false;
    if (!identity) continue;
    if (sqlite3_bind_blob(insert, 1, identity->key.data(), sizeof(identity->key), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(insert) != SQLITE_DONE || sqlite3_reset(insert) != SQLITE_OK ||
        sqlite3_clear_bindings(insert) != SQLITE_OK) return failed();
  }
  return step == SQLITE_DONE && sqlite3_exec(database, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK
      ? true : failed();
}

bool indexContains(sqlite3_stmt *statement, const void *key, std::size_t bytes,
                   bool &found, std::error_code &error) {
  if (sqlite3_bind_blob64(statement, 1, key, bytes, SQLITE_TRANSIENT) != SQLITE_OK) {
    error = std::make_error_code(std::errc::io_error); return false;
  }
  const auto step = sqlite3_step(statement);
  found = step == SQLITE_ROW;
  if ((step != SQLITE_ROW && step != SQLITE_DONE) ||
      sqlite3_reset(statement) != SQLITE_OK || sqlite3_clear_bindings(statement) != SQLITE_OK) {
    error = std::make_error_code(std::errc::io_error); return false;
  }
  return true;
}

// Only destination files not owned by this download survive the merge. Query
// before copying so retired media payloads are never read into the transaction.
bool copyPreservedDirectoryContents(const std::filesystem::path &source,
                                    const std::filesystem::path &destination,
                                    sqlite3_stmt *originalFiles, sqlite3_stmt *retiredObjects,
                                    std::error_code &error) {
  std::filesystem::create_directories(destination, error);
  if (error) return false;
  for (std::filesystem::recursive_directory_iterator it(source, error), end;
       !error && it != end; it.increment(error)) {
    const auto relative = it->path().lexically_relative(source);
    const auto key = relative.generic_u8string();
    bool found = false;
    if (!indexContains(originalFiles, key.data(), key.size(), found, error)) return false;
    if (!found) {
      // Native lookup resolves case/Unicode aliases without folding distinct
      // names on case-sensitive volumes. Multiple links are ambiguous.
      const auto identity = destinationObjectIdentity(it->path(), error);
      if (error) return false;
      if (identity && !indexContains(retiredObjects, identity->key.data(), sizeof(identity->key), found, error)) return false;
      if (found && identity->links != 1) {
        error = std::make_error_code(std::errc::too_many_links); return false;
      }
    }
    const auto status = it->symlink_status(error);
    if (error) break;
    if (found) {
      if (!std::filesystem::is_regular_file(status)) {
        error = std::make_error_code(std::errc::operation_not_supported); return false;
      }
      continue;
    }
    if (std::filesystem::is_directory(status))
      std::filesystem::create_directories(destination / relative, error);
    else
      std::filesystem::copy(it->path(), destination / relative,
          std::filesystem::copy_options::overwrite_existing |
          std::filesystem::copy_options::copy_symlinks, error);
  }
  return !error;
}

void removePath(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

bool removeVariantPath(
    const std::filesystem::path &path,
    std::vector<std::filesystem::path> *removedPaths, std::string &message) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) { message = "Could not remove an older downloaded variant: " + error.message(); return false; }
  if (removedPaths) removedPaths->push_back(path.lexically_normal());
  return true;
}

std::optional<std::string_view> storageIdFromKey(std::string_view storageKey) {
  constexpr std::size_t kStorageIdLength = 16;
  constexpr std::size_t kDelimiterLength = 2;
  if (storageKey.size() < kStorageIdLength + kDelimiterLength) {
    return std::nullopt;
  }
  const std::size_t delimiterOffset =
      storageKey.size() - kStorageIdLength - kDelimiterLength;
  if (storageKey.substr(delimiterOffset, kDelimiterLength) != "--") {
    return std::nullopt;
  }
  const std::string_view storageId = storageKey.substr(
      storageKey.size() - kStorageIdLength, kStorageIdLength);
  for (const char character : storageId) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return std::nullopt;
    }
  }
  return storageId;
}

bool storageKeysShareIdentity(std::string_view candidateKey,
                              std::string_view storageKey) {
  const auto storageId = storageIdFromKey(storageKey);
  if (!storageId) {
    return candidateKey == storageKey;
  }
  const auto candidateId = storageIdFromKey(candidateKey);
  return candidateId && *candidateId == *storageId;
}

void collectStaleExtractedVariants(
    const BmsSearchPendingArtifact &artifact,
    std::vector<std::filesystem::path> *removedPaths) {
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(artifact.downloadRoot,
                                                     error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    std::error_code entryError;
    if (!iterator->is_directory(entryError) || entryError ||
        iterator->path().filename() == "_archives") {
      continue;
    }
    const auto path = iterator->path();
    if (path.lexically_normal() ==
        artifact.destinationPath.lexically_normal()) {
      continue;
    }
    if (storageKeysShareIdentity(fspath_to_utf8(path.filename()),
                                 artifact.storageKey)) {
      removedPaths->push_back(path.lexically_normal());
    }
  }
}

void collectStaleArchiveVariants(
    const BmsSearchPendingArtifact &artifact,
    std::vector<std::filesystem::path> *removedPaths) {
  if (artifact.storageKey.empty()) {
    return;
  }

  std::error_code error;
  const auto archivesPath = artifact.downloadRoot / "_archives";
  for (std::filesystem::directory_iterator iterator(archivesPath, error), end;
       !error && iterator != end; iterator.increment(error)) {
    std::error_code entryError;
    if (!iterator->is_regular_file(entryError) || entryError) {
      continue;
    }

    const auto path = iterator->path();
    const std::string extension = archive_file::archiveExtensionFromPath(path);
    if (extension.empty()) {
      continue;
    }
    std::string archiveKey = fspath_to_utf8(path.filename());
    archiveKey.resize(archiveKey.size() - extension.size());
    if (storageKeysShareIdentity(archiveKey, artifact.storageKey) &&
        path.lexically_normal() != artifact.destinationPath.lexically_normal()) {
      removedPaths->push_back(path.lexically_normal());
    }
  }
}

} // namespace

std::filesystem::path findBmsStagingBasePath() {
#if TARGET_OS_IPHONE || TARGET_OS_ANDROID
  return downloads::platform::privateRoot() / "imports";
#else
  return std::filesystem::temp_directory_path() / "AsoBMaShowFindBms";
#endif
}

std::optional<FindBmsDownloadAttempt>
createFindBmsDownloadAttempt(const std::string &archiveName,
                             std::string &errorMessage) {
  const std::filesystem::path archiveFileName(archiveName);
  if (archiveName.empty() || archiveFileName == "." ||
      archiveFileName == ".." || archiveFileName.filename() != archiveFileName) {
    errorMessage = "Downloaded archive has an unsafe file name.";
    return std::nullopt;
  }

  std::error_code error;
  const auto base = findBmsStagingBasePath();
  std::filesystem::create_directories(base, error);
  if (error) {
    errorMessage = "Could not create the Find BMS staging folder: " +
                   error.message();
    return std::nullopt;
  }
  for (int attemptIndex = 0; attemptIndex < 4; ++attemptIndex) {
    const auto root = base / uuid::generateV4();
    if (!std::filesystem::create_directory(root, error)) {
      if (error) {
        errorMessage = "Could not create a Find BMS download attempt: " +
                       error.message();
        return std::nullopt;
      }
      continue;
    }
    const auto extractedPath = root / "extracted";
    std::filesystem::create_directory(extractedPath, error);
    if (error) {
      removePath(root);
      errorMessage = "Could not prepare archive extraction: " + error.message();
      return std::nullopt;
    }
    return FindBmsDownloadAttempt{.root = root,
                                  .archivePath = root / archiveFileName,
                                  .extractedPath = extractedPath};
  }
  errorMessage = "Could not allocate a unique Find BMS download attempt.";
  return std::nullopt;
}

bool commitFindBmsPendingArtifact(
    const BmsSearchPendingArtifact &artifact, std::string &errorMessage,
    FindBmsRenameOperation renameOperation,
    std::vector<std::filesystem::path> *removedPaths,
    FindBmsPrepareVariantRemoval prepareRemoval,
    std::function<bool()> beforePublication) {
  if (removedPaths != nullptr) {
    removedPaths->clear();
  }
  if (!validatePendingArtifact(artifact, errorMessage)) {
    return false;
  }

  std::error_code error;
  const bool sourceIsValid =
      artifact.kind == BmsSearchPendingArtifactKind::Archive
          ? std::filesystem::is_regular_file(artifact.sourcePath, error)
          : std::filesystem::is_directory(artifact.sourcePath, error);
  if (error || !sourceIsValid) {
    errorMessage = "Downloaded files are no longer available to keep.";
    return false;
  }

  UniqueResource<sqlite3, sqlite3_close> originalFilesDatabase;
  UniqueResource<sqlite3_stmt, sqlite3_finalize> originalFiles, retiredObjects;
  if (!artifact.originalFilesIndexPath.empty()) {
    sqlite3 *database = nullptr;
    const auto opened = sqlite3_open_v2(fspath_to_utf8(artifact.originalFilesIndexPath).c_str(),
        &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    originalFilesDatabase.reset(database);
    sqlite3_stmt *statement = nullptr;
    if (opened != SQLITE_OK ||
        sqlite3_exec(database, "PRAGMA cache_size=-1024", nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(database, "SELECT 1 FROM original_files WHERE path=?", -1, &statement, nullptr) != SQLITE_OK) {
      sqlite3_finalize(statement);
      errorMessage = "Could not read the Find BMS original-file index.";
      return false;
    }
    originalFiles.reset(statement);
    if (!indexRetiredDestinationObjects(artifact, database, error)) {
      errorMessage = "Could not resolve retired downloaded files: " + error.message();
      return false;
    }
    statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(database, "SELECT 1 FROM retired_objects WHERE id=?", -1, &statement, nullptr);
    retiredObjects.reset(statement);
    if (prepared != SQLITE_OK) {
      errorMessage = "Could not read retired downloaded file identities.";
      return false;
    }
  }

  const auto destination = artifact.destinationPath;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    errorMessage = "Could not create the Find BMS destination folder: " +
                   error.message();
    return false;
  }
  const auto transactionRoot = downloads::publicationTransactionRoot(artifact.downloadRoot);
  if (transactionRoot.empty()) {
    errorMessage = "Private publication storage is unavailable on this library volume.";
    return false;
  }
  const auto transactionPath = transactionRoot / artifact.stagingRoot.filename();
  const auto commitPath = transactionPath / "commit";
  const auto backupPath = transactionPath / "backup";
  removePath(transactionPath);
  std::filesystem::create_directories(transactionPath, error);
  if (error) {
    errorMessage = "Could not create the private Find BMS transaction folder: " +
                   error.message();
    return false;
  }
  auto cleanupTransaction = [&]() {
    removePath(transactionPath);
    std::error_code ignored;
    std::filesystem::remove(transactionRoot, ignored);
  };

  if (artifact.kind == BmsSearchPendingArtifactKind::Archive) {
    std::filesystem::copy_file(artifact.sourcePath, commitPath,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
  } else {
    if (std::filesystem::exists(destination, error) && !error &&
        !std::filesystem::is_directory(destination, error)) {
      cleanupTransaction();
      errorMessage = "Existing Find BMS destination is not a folder.";
      return false;
    }
    if (!error && std::filesystem::exists(destination, error)) {
      if (originalFiles)
        copyPreservedDirectoryContents(destination, commitPath, originalFiles.get(), retiredObjects.get(), error);
      else
        copyDirectoryContents(destination, commitPath, error);
    } else if (!error) {
      std::filesystem::create_directories(commitPath, error);
    }
    if (!error) {
      copyDirectoryContents(artifact.sourcePath, commitPath, error);
    }
  }
  if (error) {
    cleanupTransaction();
    errorMessage = "Could not prepare downloaded files: " + error.message();
    return false;
  }

  originalFiles.reset();
  retiredObjects.reset();
  originalFilesDatabase.reset();

  auto renamePath = [&renameOperation](const std::filesystem::path &from,
                                       const std::filesystem::path &to,
                                       std::error_code &renameError) {
    renameError.clear();
    if (renameOperation) {
      renameOperation(from, to, renameError);
    } else {
      std::filesystem::rename(from, to, renameError);
    }
  };

  const bool hadDestination = std::filesystem::exists(destination, error);
  if (error) {
    cleanupTransaction();
    errorMessage = "Could not inspect the Find BMS destination: " +
                   error.message();
    return false;
  }
  // Preparation may copy a large package. Honor a cancellation that arrived
  // during that work before the first rename; once swapping starts, finish or
  // restore the original destination without another cancellation boundary.
  if (beforePublication && !beforePublication()) {
    cleanupTransaction();
    errorMessage = "Download cancelled.";
    return false;
  }
  if (hadDestination) {
    renamePath(destination, backupPath, error);
    if (error) {
      cleanupTransaction();
      errorMessage = "Could not back up existing downloaded files: " +
                     error.message();
      return false;
    }
  }

  renamePath(commitPath, destination, error);
  if (error) {
    const std::string swapError = error.message();
    if (hadDestination) {
      std::error_code restoreError;
      renamePath(backupPath, destination, restoreError);
      if (restoreError) {
        errorMessage = "Could not install downloaded files (" + swapError +
                       ") or restore the previous files (" +
                       restoreError.message() + ").";
        return false;
      }
    }
    cleanupTransaction();
    errorMessage = "Could not install downloaded files: " + swapError;
    return false;
  }

  if (hadDestination) {
    removePath(backupPath);
  }
  cleanupTransaction();
  return finishFindBmsPendingArtifactPublication(artifact, errorMessage, removedPaths, std::move(prepareRemoval));
}

bool deleteFindBmsPendingArtifact(const BmsSearchPendingArtifact &artifact,
                                  std::string &errorMessage) {
  if (!validatePendingArtifact(artifact, errorMessage)) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::exists(artifact.stagingRoot, error)) {
    if (error) {
      errorMessage = "Could not inspect downloaded files: " + error.message();
      return false;
    }
    errorMessage.clear();
    return true;
  }
  std::filesystem::remove_all(artifact.stagingRoot, error);
  if (error) {
    errorMessage = "Could not delete downloaded files: " + error.message();
    return false;
  }
  errorMessage.clear();
  return true;
}

bool finishFindBmsPendingArtifactPublication(const BmsSearchPendingArtifact &artifact,
    std::string &errorMessage, std::vector<std::filesystem::path> *removedPaths,
    FindBmsPrepareVariantRemoval prepareRemoval) {
  auto recovered = artifact;
  // The original-file index may already have been discarded after publication.
  recovered.originalFilesIndexPath.clear();
  if (!validatePendingArtifact(recovered, errorMessage)) return false;
  std::vector<std::filesystem::path> variants;
  if (!recovered.alternateDestinationPath.empty() && std::filesystem::exists(recovered.alternateDestinationPath))
    variants.push_back(recovered.alternateDestinationPath.lexically_normal());
  collectStaleExtractedVariants(recovered, &variants);
  collectStaleArchiveVariants(recovered, &variants);
  std::sort(variants.begin(), variants.end());
  variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
  // Persist the entire metadata-only removal set before deleting the first path.
  if (prepareRemoval && !prepareRemoval(variants, errorMessage)) return false;
  for (const auto &path : variants)
    if (!removeVariantPath(path, removedPaths, errorMessage) && prepareRemoval) return false;
  removePath(recovered.stagingRoot);
  errorMessage.clear();
  return true;
}

} // namespace asobmshow::bms_search
