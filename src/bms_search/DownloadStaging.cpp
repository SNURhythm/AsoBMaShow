#include "DownloadStaging.h"

#include "../Uuid.h"

#include <filesystem>
#include <string>

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
