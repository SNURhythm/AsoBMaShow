#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class BmsSearchDriver {
public:
  static bool isHost(const ParsedUrl &url);
  static std::vector<std::string> bmsLinks(const std::string &patternUrl,
                                           const std::string &html);
  static std::vector<DownloadCandidate>
  downloadCandidates(const std::string &bmsUrl, const std::string &html);
};

} // namespace asobmshow::bms_search
