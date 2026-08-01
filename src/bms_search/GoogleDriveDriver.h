#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class GoogleDriveDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::optional<DownloadCandidate> classify(const std::string &url);
  static bool resolveWarningDownload(
      const std::string &downloadUrl, const std::string &displayUrl,
      const std::filesystem::path &archivePath, std::atomic_bool &cancelled,
      std::string &errorMessage,
      BmsSearchDownloadProgressCallback progressCallback);

private:
  static std::optional<std::string> fileId(const std::string &url);
  static std::optional<std::string> fileIdFromUrls(
      const std::string &downloadUrl, const std::string &displayUrl);
  static std::optional<std::string> confirmationUrl(
      const std::string &fileId, const std::string &html,
      const std::string &baseUrl);
};

} // namespace asobmshow::bms_search
