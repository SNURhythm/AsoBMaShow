#pragma once

#include "SkinProfileSettings.h"

#include <cstdint>

namespace skin {

// Every guard here names an already-existing host protection.  This policy
// deliberately does not classify Lua/model compatibility validation.
enum class SkinSafetyGuardSeverity : std::uint8_t {
  Protective,
  Catastrophic,
};

enum class SkinSafetyGuard : std::uint8_t {
  CatalogWriteAuthorization,
  VirtualFileContainment,
  LuaResourceBudget,
  PackageResourceLimit,
  ResourceAllocationLimit,
  LuaDecoderLimit,
  ProcessGlobalMutation,
};

[[nodiscard]] constexpr SkinSafetyGuardSeverity
skinSafetyGuardSeverity(SkinSafetyGuard guard) noexcept {
  switch (guard) {
  case SkinSafetyGuard::CatalogWriteAuthorization:
    return SkinSafetyGuardSeverity::Protective;
  case SkinSafetyGuard::VirtualFileContainment:
  case SkinSafetyGuard::LuaResourceBudget:
  case SkinSafetyGuard::PackageResourceLimit:
  case SkinSafetyGuard::ResourceAllocationLimit:
  case SkinSafetyGuard::LuaDecoderLimit:
  case SkinSafetyGuard::ProcessGlobalMutation:
    return SkinSafetyGuardSeverity::Catastrophic;
  }
  return SkinSafetyGuardSeverity::Catastrophic;
}

class SkinSafetyPolicy final {
public:
  constexpr explicit SkinSafetyPolicy(
      SkinSafetyLevel level = SkinSafetyLevel::Standard) noexcept
      : level_(level) {}

  [[nodiscard]] constexpr SkinSafetyLevel level() const noexcept {
    return level_;
  }

  [[nodiscard]] constexpr bool enforces(SkinSafetyGuard guard) const noexcept {
    switch (level_) {
    case SkinSafetyLevel::Standard:
      return true;
    case SkinSafetyLevel::BeatorajaCompatibility:
      return skinSafetyGuardSeverity(guard) ==
             SkinSafetyGuardSeverity::Catastrophic;
    case SkinSafetyLevel::Unrestricted:
      return false;
    }
    return true;
  }

private:
  SkinSafetyLevel level_ = SkinSafetyLevel::Standard;
};

} // namespace skin
