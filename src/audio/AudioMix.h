#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace audio {

enum class Bus : std::uint8_t { Bgm, Keysound };

struct Volumes {
  float master = 1.0f;
  float bgm = 1.0f;
  float keysound = 1.0f;
};

float EffectiveGain(Bus bus, const Volumes &volumes);

std::vector<short> ResamplePcm(std::span<const short> source, int channels,
                               int sourceRate, int targetRate);

} // namespace audio
