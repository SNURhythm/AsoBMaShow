#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class GingerRushDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class KonmaiDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class WriggleDriver {
public:
  static PackageSourceLookupResult lookupByMd5(const std::string &md5);
};

class EndlessDreamSourcesDriver {
public:
  static bool tryDownloadByMd5(
      const std::string &md5, const std::string &archiveKey,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      BmsSearchResult &result);
};

} // namespace asobmshow::bms_search
