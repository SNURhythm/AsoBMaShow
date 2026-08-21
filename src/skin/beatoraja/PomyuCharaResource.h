#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "BeatorajaSkinModel.h"
#include "LuaSkinFileSystem.h"
#include "../SkinSafetyPolicy.h"
#include "../../view/DecodedImageCache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

namespace skin {

struct PreparedPomyuCharaFrame {
  SkinResourceId resource = 0;
  SkinSourceRect region;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::uint8_t alpha = 255;
  int angleDegrees = 0;
};

struct PreparedPomyuCharaAnimation {
  int motion = 0;
  std::optional<int> builtinTimerId;
  std::array<int, 3> options{};
  int frameMillis = 0;
  int cycleMillis = 0;
  std::size_t loopStartFrame = 0;
  std::vector<PreparedPomyuCharaFrame> frames;
};

struct PreparedPomyuCharaResource {
  SkinObjectId object = 0;
  bool relativePlacement = false;
  int coordinateWidth = 0;
  int coordinateHeight = 0;
  std::vector<PreparedPomyuCharaAnimation> animations;
};

struct PomyuCharaDecodedImage {
  SkinResourceId id = 0;
  image_decode::DecodedImageData pixels;
  std::vector<SkinSourceRect> regions;
};

struct PomyuCharaPreparationResult {
  std::vector<PomyuCharaDecodedImage> images;
  std::vector<PreparedPomyuCharaResource> resources;
  std::array<int, 8> motionCyclesMillis = {1, 1, 1, 1, 1, 1, 1, 1};
  std::size_t encodedBytes = 0;
  std::size_t decodedBytes = 0;
  bool budgetExceeded = false;
  bool cancelled = false;
};

[[nodiscard]] PomyuCharaPreparationResult preparePomyuCharaResources(
    const LuaSkinFileSystem &, const ValidatedBeatorajaSkinModel &,
    const BeatorajaSkinConfiguration &, SkinSafetyPolicy,
    std::stop_token = {});

} // namespace skin
