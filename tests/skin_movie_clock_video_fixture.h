#pragma once

#include <cstdint>

namespace movie_clock_fixture {
struct UploadObservation {
  std::uint64_t uploads = 0;
  std::int64_t displayedMicros = -1;
  bool validMarker = false;
};

extern UploadObservation observation;
}
