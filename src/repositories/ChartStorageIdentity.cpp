#include "ChartStorageIdentity.h"

#include "../ArchiveFile.h"
#include "../Utils.h"
#include "../targets.h"

#include <optional>

namespace chart_storage_identity {
namespace {

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
std::optional<std::filesystem::path>
relativeToCurrentDocumentsPath(const std::filesystem::path &path) {
  if (path.empty() || !path.is_absolute()) {
    return std::nullopt;
  }

  const std::filesystem::path documentsRoot =
      Utils::GetDocumentsPath().lexically_normal();
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::string rootText = documentsRoot.generic_string();
  const std::string pathText = normalizedPath.generic_string();
  const std::string rootPrefix = rootText + "/";

  if (pathText == rootText) {
    return std::filesystem::path();
  }
  if (pathText.starts_with(rootPrefix)) {
    return std::filesystem::path(pathText.substr(rootPrefix.size()));
  }
  return std::nullopt;
}

std::filesystem::path
storedDocumentsPath(const std::filesystem::path &relativeToDocuments) {
  std::filesystem::path stored("Documents");
  if (!relativeToDocuments.empty()) {
    stored /= relativeToDocuments;
  }
  return stored;
}

std::optional<std::filesystem::path>
relativeFromStoredDocumentsPath(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return std::nullopt;
  }

  auto it = path.begin();
  if (it == path.end() || *it != std::filesystem::path("Documents")) {
    return std::nullopt;
  }
  ++it;

  std::filesystem::path relative;
  for (; it != path.end(); ++it) {
    relative /= *it;
  }
  return relative;
}
#endif

} // namespace

std::string StoredPathText(std::filesystem::path path) {
  if (path.empty()) {
    return "";
  }
  ToRelativePath(path);
  path = path.lexically_normal();
  return fspath_to_utf8(path);
}

void ToRelativePath([[maybe_unused]] std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (path.empty() || path.is_relative()) {
    return;
  }

  if (auto relative = relativeToCurrentDocumentsPath(path)) {
    path = storedDocumentsPath(*relative);
  }
#endif
}

void ToAbsolutePath([[maybe_unused]] std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (path.empty()) {
    return;
  }

  if (path.is_absolute()) {
    if (auto relative = relativeToCurrentDocumentsPath(path)) {
      path = Utils::GetDocumentsPath() / *relative;
    }
    return;
  }

  if (auto relative = relativeFromStoredDocumentsPath(path)) {
    path = Utils::GetDocumentsPath() / *relative;
  } else {
    path = Utils::GetDocumentsPath("BMS") / path;
  }
#endif
}

void ConfigureArchiveCachePathNormalization() {
  archive_file::setCachePathNormalizer([](std::filesystem::path &path) {
    ToRelativePath(path);
  });
}

} // namespace chart_storage_identity
