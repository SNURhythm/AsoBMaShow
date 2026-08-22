#pragma once

#include "SkinProfileSettings.h"

#include <cstdint>
#include <limits>

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
      SkinSafetyLevel level = SkinSafetyLevel::Standard,
      std::uint64_t maximumDocumentBytes =
          std::numeric_limits<std::uint64_t>::max()) noexcept
      : level_(level), maximumDocumentBytes_(maximumDocumentBytes) {}

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

  [[nodiscard]] constexpr std::uint64_t
  limit(SkinSafetyGuard guard, std::uint64_t standardLimit) const noexcept {
    return enforces(guard) ? standardLimit
                           : std::numeric_limits<std::uint64_t>::max();
  }

  [[nodiscard]] constexpr std::uint64_t
  documentByteLimit(std::uint64_t standardLimit) const noexcept {
    const std::uint64_t policyLimit =
        limit(SkinSafetyGuard::LuaDecoderLimit, standardLimit);
    return maximumDocumentBytes_ < policyLimit ? maximumDocumentBytes_
                                                : policyLimit;
  }

private:
  SkinSafetyLevel level_ = SkinSafetyLevel::Standard;
  std::uint64_t maximumDocumentBytes_ =
      std::numeric_limits<std::uint64_t>::max();
};

} // namespace skin
