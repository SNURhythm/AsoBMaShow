#include "SdlTtfRuntime.h"

#include <SDL_ttf.h>

namespace text_runtime {
namespace {
std::mutex runtimeMutex;
int references = 0;
}

bool acquire() noexcept {
  std::lock_guard lock(runtimeMutex);
  if (references == 0 && TTF_Init() != 0) return false;
  ++references;
  return true;
}
void release() noexcept {
  std::lock_guard lock(runtimeMutex);
  if (references > 0 && --references == 0) TTF_Quit();
}
std::mutex &mutex() noexcept { return runtimeMutex; }
} // namespace text_runtime
