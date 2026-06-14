#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class IpfsDriver {
public:
  static std::optional<DownloadCandidate> classify(const std::string &url);

private:
  static std::optional<std::string> pathFromUrl(const std::string &url);
};

} // namespace asobmshow::bms_search
