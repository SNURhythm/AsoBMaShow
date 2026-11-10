#pragma once
#include "Internal.h"

namespace asobmshow::bms_search {
class GingerRushDriver {
public:
  static PackageSourceLookupResult lookupByMd5(
      const std::string &md5, const std::atomic_bool &cancelled);
};
} // namespace asobmshow::bms_search
