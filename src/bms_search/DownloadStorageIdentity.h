#pragma once

#include <string>
#include <string_view>

namespace asobmshow::bms_search {

struct FindBmsStorageNames {
  std::string storageKey;
  std::string archiveName;
};

FindBmsStorageNames findBmsStorageNames(
    std::string_view archiveName, std::string_view fallbackExtension,
    std::string_view identitySeed);

} // namespace asobmshow::bms_search
