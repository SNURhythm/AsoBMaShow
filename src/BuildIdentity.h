#pragma once

#include <string>

namespace skin {

struct SkinBuildIdentity {
  std::string commit;
  std::string configuration;
  bool cleanSource = false;

  [[nodiscard]] bool validForAcceptance() const noexcept;
};

[[nodiscard]] SkinBuildIdentity compiledSkinBuildIdentity();

} // namespace skin
