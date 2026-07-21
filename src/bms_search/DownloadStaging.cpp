#include "DownloadStaging.h"

#include "../ArchiveFile.h"
#include "../Uuid.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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
  if (error || sourcePath.empty() ||
      !isStrictDescendant(sourcePath, stagingRoot)) {
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
            ? sourcePath == expectedArchiveSource &&
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
    if (sourcePath.parent_path() != stagingRoot ||
        destinationPath.parent_path() != downloadRoot / "_archives") {
      errorMessage = "Refusing invalid Find BMS archive metadata.";
      return false;
    }
  } else if (sourcePath != stagingRoot / "extracted" ||
             destinationPath.parent_path() != downloadRoot) {
    errorMessage = "Refusing invalid Find BMS extracted-folder metadata.";
    return false;
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
  for (std::filesystem::directory_iterator iterator(source, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto target = destination / iterator->path().filename();
    std::filesystem::copy(
        iterator->path(), target,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::overwrite_existing |
            std::filesystem::copy_options::copy_symlinks,
        error);
  }
  return !error;
}

void removePath(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
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

void removeStaleExtractedVariants(const BmsSearchPendingArtifact &artifact) {
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
      removePath(path);
    }
  }
}

void removeStaleArchiveVariants(const BmsSearchPendingArtifact &artifact) {
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
      removePath(path);
    }
  }
}

} // namespace

std::filesystem::path findBmsStagingBasePath() {
  return std::filesystem::temp_directory_path() / "AsoBMaShowFindBms";
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
    FindBmsRenameOperation renameOperation) {
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

  const auto destination = artifact.destinationPath;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    errorMessage = "Could not create the Find BMS destination folder: " +
                   error.message();
    return false;
  }
  const std::string transactionId = uuid::generateV4();
  const auto commitPath = destination.parent_path() /
                          (destination.filename().string() + ".commit-" +
                           transactionId);
  const auto backupPath = destination.parent_path() /
                          (destination.filename().string() + ".backup-" +
                           transactionId);
  removePath(commitPath);
  removePath(backupPath);

  if (artifact.kind == BmsSearchPendingArtifactKind::Archive) {
    std::filesystem::copy_file(artifact.sourcePath, commitPath,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
  } else {
    if (std::filesystem::exists(destination, error) && !error &&
        !std::filesystem::is_directory(destination, error)) {
      errorMessage = "Existing Find BMS destination is not a folder.";
      return false;
    }
    if (!error && std::filesystem::exists(destination, error)) {
      copyDirectoryContents(destination, commitPath, error);
    } else if (!error) {
      std::filesystem::create_directories(commitPath, error);
    }
    if (!error) {
      copyDirectoryContents(artifact.sourcePath, commitPath, error);
    }
  }
  if (error) {
    removePath(commitPath);
    errorMessage = "Could not prepare downloaded files: " + error.message();
    return false;
  }

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
    removePath(commitPath);
    errorMessage = "Could not inspect the Find BMS destination: " +
                   error.message();
    return false;
  }
  if (hadDestination) {
    renamePath(destination, backupPath, error);
    if (error) {
      removePath(commitPath);
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
    removePath(commitPath);
    errorMessage = "Could not install downloaded files: " + swapError;
    return false;
  }

  if (hadDestination) {
    removePath(backupPath);
  }
  if (!artifact.alternateDestinationPath.empty()) {
    std::error_code ignoredCleanupError;
    std::filesystem::remove_all(artifact.alternateDestinationPath,
                                ignoredCleanupError);
  }
  removeStaleExtractedVariants(artifact);
  removeStaleArchiveVariants(artifact);
  removePath(artifact.stagingRoot);
  errorMessage.clear();
  return true;
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

} // namespace asobmshow::bms_search
