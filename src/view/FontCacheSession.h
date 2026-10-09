#pragma once

#include <cstddef>
#include <cstdint>

namespace text_runtime {
// Keep recently used TextView fonts alive across screen changes. Construct before
// the application's views so retained fonts close before the SDL_ttf runtime.
// Sessions and their views must be destroyed on their creating thread.
class FontCacheSession {
public:
  FontCacheSession() noexcept;
  ~FontCacheSession();
  FontCacheSession(const FontCacheSession &) = delete;
  FontCacheSession &operator=(const FontCacheSession &) = delete;
private:
  bool initialized_ = false;
};

struct FontCacheStats {
  std::size_t active = 0;
  std::size_t idle = 0;
  std::uint64_t opens = 0;
};
[[nodiscard]] FontCacheStats fontCacheStatsForTesting();
} // namespace text_runtime
