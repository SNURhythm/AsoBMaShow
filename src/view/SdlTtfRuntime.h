#pragma once

#include <mutex>

namespace text_runtime {
// Lock order is lifecycle -> operation. acquire/release take both locks when
// crossing the TTF_Init/TTF_Quit boundary; every other SDL_ttf operation takes
// OperationGuard. This keeps final TTF_Quit after the last open font closes.
class OperationGuard {
public:
  OperationGuard() noexcept;
  ~OperationGuard() = default;
  OperationGuard(const OperationGuard &) = delete;
  OperationGuard &operator=(const OperationGuard &) = delete;
private:
  std::unique_lock<std::recursive_mutex> lock_;
};

bool acquire() noexcept;
void release() noexcept;
// Deterministic lifecycle probe for overlap/final-release regression tests.
[[nodiscard]] unsigned activeReferencesForTesting() noexcept;
} // namespace text_runtime
