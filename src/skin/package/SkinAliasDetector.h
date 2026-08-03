#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace skin {

enum class SkinRejectedLinkKind : std::uint8_t {
  None,
  SymbolicLink,
  HardLink,
  AppleFinderAlias,
  WindowsReparsePoint,
  NonRegular,
};

class SkinAliasDetector {
public:
  virtual ~SkinAliasDetector() = default;
  virtual SkinRejectedLinkKind
  inspectNoFollow(const std::filesystem::path &) const = 0;
};

std::unique_ptr<SkinAliasDetector> createPlatformSkinAliasDetector();

} // namespace skin
