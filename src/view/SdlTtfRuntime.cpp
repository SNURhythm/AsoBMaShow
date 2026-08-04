#include "SdlTtfRuntime.h"

#include <SDL_ttf.h>

namespace text_runtime {
namespace {
std::mutex lifecycleMutex;
std::recursive_mutex operationMutex;
int references = 0;
}

OperationGuard::OperationGuard() noexcept : lock_(operationMutex) {}

bool acquire() noexcept {
  std::lock_guard lifecycleLock(lifecycleMutex);
  std::lock_guard operationLock(operationMutex);
  if (references == 0 && TTF_Init() != 0) return false;
  ++references;
  return true;
}
void release() noexcept {
  std::lock_guard lifecycleLock(lifecycleMutex);
  std::lock_guard operationLock(operationMutex);
  if (references > 0 && --references == 0) TTF_Quit();
}
unsigned activeReferencesForTesting() noexcept {
  std::lock_guard lifecycleLock(lifecycleMutex);
  return static_cast<unsigned>(references);
}
} // namespace text_runtime
