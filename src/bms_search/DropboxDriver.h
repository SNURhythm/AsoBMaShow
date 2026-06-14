#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class DropboxDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::optional<DownloadCandidate> classify(const std::string &url);

private:
  static std::string forceDownloadUrl(const std::string &url);
};

} // namespace asobmshow::bms_search
