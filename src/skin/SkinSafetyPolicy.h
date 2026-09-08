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
          std::numeric_limits<std::uint64_t>::max(),
      bool preservePinnedLuaSandbox = false) noexcept
      : level_(level), maximumDocumentBytes_(maximumDocumentBytes),
        preservePinnedLuaSandbox_(preservePinnedLuaSandbox) {}

  [[nodiscard]] constexpr SkinSafetyLevel level() const noexcept {
    return level_;
  }

  [[nodiscard]] constexpr bool preservesPinnedLuaSandbox() const noexcept {
    return preservePinnedLuaSandbox_;
  }

  [[nodiscard]] constexpr bool
  usesPinnedLuaPathSemantics() const noexcept {
    return level_ == SkinSafetyLevel::BeatorajaCompatibility ||
           preservePinnedLuaSandbox_;
  }

  [[nodiscard]] constexpr bool enforces(SkinSafetyGuard guard) const noexcept {
    // Beatoraja's Lua loader always keeps its path resolver and process
    // globals sandboxed.  Selector compatibility lifts only Aso-added quotas,
    // never those upstream execution boundaries.
    if (preservePinnedLuaSandbox_ &&
        (guard == SkinSafetyGuard::VirtualFileContainment ||
         guard == SkinSafetyGuard::ProcessGlobalMutation)) {
      return true;
    }
    switch (level_) {
    case SkinSafetyLevel::Standard:
      return true;
    case SkinSafetyLevel::BeatorajaCompatibility:
      // SkinLuaAccessor(false) keeps only the upstream lexical skin-root
      // boundary and SafeOsLib process restrictions.  Its loader has none of
      // AsoBMaShow's document, resource, allocation, or decoder quotas.
      return guard == SkinSafetyGuard::VirtualFileContainment ||
             guard == SkinSafetyGuard::ProcessGlobalMutation;
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
  bool preservePinnedLuaSandbox_ = false;
};

} // namespace skin
