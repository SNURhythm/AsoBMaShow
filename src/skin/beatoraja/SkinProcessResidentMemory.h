#pragma once

#include <cstdint>
#include <optional>

namespace skin {

struct ProcessResidentMemoryNativeQueryResult {
  bool succeeded = false;
  bool complete = false;
  std::uint64_t residentBytes = 0;
};

using ProcessResidentMemoryNativeQuery =
    ProcessResidentMemoryNativeQueryResult (*)() noexcept;

// The query overload is an allocation-free seam for platform-independent
// callers/tests. A result is available only when the native query completed.
[[nodiscard]] std::optional<std::uint64_t>
    currentProcessResidentBytes(ProcessResidentMemoryNativeQuery) noexcept;

// Returns current process resident bytes when the platform has an honest
// native probe. Unsupported platforms and all query failures are unavailable.
[[nodiscard]] std::optional<std::uint64_t>
currentProcessResidentBytes() noexcept;

} // namespace skin
