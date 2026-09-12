#include "video/VideoPlayer.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"

#include "skin_movie_clock_video_fixture.h"

#include <algorithm>

namespace movie_clock_fixture {
UploadObservation observation;
}

namespace bgfx {
void observeMovieClockTextureUpload(
    TextureHandle handle, std::uint16_t layer, std::uint8_t mip,
    std::uint16_t x, std::uint16_t y, std::uint16_t width,
    std::uint16_t height, const Memory *memory, std::uint16_t pitch) {
  if (width == 16 && height == 16) {
    auto &observed = movie_clock_fixture::observation;
    ++observed.uploads;
    const unsigned int marker = memory->data[0];
    observed.validMarker = marker >= 16 && marker <= 168 &&
                           (marker - 16) % 8 == 0;
    for (std::uint16_t row = 0; row < height; ++row) {
      for (std::uint16_t column = 0; column < width; ++column) {
        observed.validMarker = observed.validMarker &&
            memory->data[row * pitch + column] == marker;
      }
    }
    observed.displayedMicros = observed.validMarker
        ? static_cast<std::int64_t>((marker - 16) / 8) * 50'000
        : -1;
  }
  updateTexture2D(handle, layer, mip, x, y, width, height, memory, pitch);
}
}

#define updateTexture2D observeMovieClockTextureUpload
#include "video/VideoPlayer.cpp"
#undef updateTexture2D
