#pragma once

#include "../package/SkinPackageTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct SkinCompatibilityDiagnostic {
  SkinDiagnostic diagnostic;
  std::string objectId;
  bool critical = false;
};

class SkinCompatibilityDiagnostics final {
public:
  void report(SkinDiagnostic, bool critical, std::string_view objectId = {});
  std::span<const SkinCompatibilityDiagnostic> entries() const noexcept;
  bool hasCritical() const noexcept;
  void clear() noexcept;

private:
  std::vector<SkinCompatibilityDiagnostic> entries_;
  bool hasCritical_ = false;
};

} // namespace skin
