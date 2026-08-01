#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class DirectArchiveDriver {
public:
  static std::optional<DownloadCandidate> classify(const std::string &url);
};

} // namespace asobmshow::bms_search
