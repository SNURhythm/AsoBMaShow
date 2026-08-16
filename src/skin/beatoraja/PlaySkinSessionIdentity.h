#pragma once

#include "../SkinProfileSettings.h"

#include <cstdint>
#include <string>

namespace skin {

struct PlaySkinSessionIdentity {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  SkinEntryId entry;
  std::string revisionDigest;
  std::string configurationDigest;
};

} // namespace skin
