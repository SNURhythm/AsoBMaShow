#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"
#include "../../audio/GameplayBgaFrame.h"
#include "../../rendering/RenderPlan.h"
#include "../../rendering/SkinQuadBatchRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace skin {
namespace {

enum class SubmissionStepKind : std::uint8_t { QuadSegment, BgaTarget };

struct SubmissionStep {
  SubmissionStepKind kind = SubmissionStepKind::QuadSegment;
  std::size_t index = 0;
};

std::array<double, 2> apply(const Affine2D &matrix, double x,
                            double y) noexcept {
  return {matrix.m00 * x + matrix.m01 * y + matrix.tx,
          matrix.m10 * x + matrix.m11 * y + matrix.ty};
}

std::optional<BgaDrawTarget>
projectBgaTarget(const SkinBgaCommand &command, GameplayBgaRole role) {
  if (!command.viewport.valid) {
    return std::nullopt;
  }
  const auto &geometry = command.authoredGeometry;
  const auto &rect = geometry.rect;
  const double pivotX = rect.x + geometry.centerX * rect.width;
  const double pivotY = rect.y + geometry.centerY * rect.height;
  const double radians = geometry.angleDegrees * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  const std::array<std::array<double, 2>, 4> authoredCorners = {
      {{rect.x, rect.y},
       {rect.x + rect.width, rect.y},
       {rect.x + rect.width, rect.y + rect.height},
       {rect.x, rect.y + rect.height}}};

  BgaDrawTarget target{.role = role,
                       .viewId = rendering::ui_view,
                       .stretch = geometry.stretch,
                       .tint = geometry.rgba,
                       .blend = geometry.blend,
                       .authoredOrdinal = command.authoredOrdinal};
  for (std::size_t index = 0; index < authoredCorners.size(); ++index) {
    const double x = authoredCorners[index][0] - pivotX;
    const double y = authoredCorners[index][1] - pivotY;
    const auto projected = apply(
        command.viewport.authoredToUi,
        pivotX + x * cosine - y * sine,
        pivotY + x * sine + y * cosine);
    target.destination[index] = {
        .x = static_cast<float>(projected[0]),
        .y = static_cast<float>(projected[1])};
  }
  if (geometry.clip && geometry.clip->width > 0.0 &&
      geometry.clip->height > 0.0) {
    const std::array clipCorners{
        apply(command.viewport.authoredToUi, geometry.clip->x,
              geometry.clip->y),
        apply(command.viewport.authoredToUi,
              geometry.clip->x + geometry.clip->width, geometry.clip->y),
        apply(command.viewport.authoredToUi,
              geometry.clip->x + geometry.clip->width,
              geometry.clip->y + geometry.clip->height),
        apply(command.viewport.authoredToUi, geometry.clip->x,
              geometry.clip->y + geometry.clip->height)};
    const auto [minimumX, maximumX] = std::ranges::minmax(
        clipCorners | std::views::transform(
                          [](const auto &point) { return point[0]; }));
    const auto [minimumY, maximumY] = std::ranges::minmax(
        clipCorners | std::views::transform(
                          [](const auto &point) { return point[1]; }));
    target.clip = GameplayBgaClipRect{
        .x = minimumX,
        .y = minimumY,
        .width = maximumX - minimumX,
        .height = maximumY - minimumY};
  }
  return target;
}

} // namespace

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer, const SkinResourceCatalog &resources,
    RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer) const {
  renderer.begin(context, resources);
  if (!renderer.submit(buffer.commands)) {
    renderer.flush();
    return false;
  }
  renderer.flush();
  return true;
}

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer,
    const SkinPreparedResourceView &resources, RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer,
    const PreparedGameplayBgaFrame &bgaFrame,
    IGameplayBgaSubmitter &bgaSubmitter) const {
  std::vector<std::span<const SkinDrawCommand>> quadSegments;
  std::vector<BgaDrawTarget> bgaTargets;
  std::vector<SubmissionStep> steps;
  bool hasBgaMarker = false;
  try {
    quadSegments.reserve(buffer.commands.size() / 2U + 1U);
    bgaTargets.reserve(buffer.commands.size() * 2U);
    steps.reserve(buffer.commands.size() * 2U);
    std::size_t quadStart = 0;
    const auto appendQuadSegment = [&](std::size_t end) {
      if (end == quadStart) {
        return;
      }
      quadSegments.emplace_back(buffer.commands.data() + quadStart,
                                end - quadStart);
      steps.push_back({.kind = SubmissionStepKind::QuadSegment,
                       .index = quadSegments.size() - 1U});
    };
    const auto appendBgaTarget = [&](const SkinBgaCommand &command,
                                     GameplayBgaRole role) {
      auto target = projectBgaTarget(command, role);
      if (!target) {
        return false;
      }
      bgaTargets.push_back(std::move(*target));
      steps.push_back({.kind = SubmissionStepKind::BgaTarget,
                       .index = bgaTargets.size() - 1U});
      return true;
    };

    for (std::size_t commandIndex = 0; commandIndex < buffer.commands.size();
         ++commandIndex) {
      const auto *bga =
          std::get_if<SkinBgaCommand>(&buffer.commands[commandIndex].payload);
      if (bga == nullptr) {
        continue;
      }
      hasBgaMarker = true;
      appendQuadSegment(commandIndex);
      switch (bgaFrame.composition) {
      case GameplayBgaComposition::Blank:
        if (!appendBgaTarget(*bga, GameplayBgaRole::Base)) {
          return false;
        }
        break;
      case GameplayBgaComposition::MissOnly:
        if (bgaFrame.miss &&
            !appendBgaTarget(*bga, GameplayBgaRole::Miss)) {
          return false;
        }
        break;
      case GameplayBgaComposition::BaseThenLayer:
        if (!appendBgaTarget(*bga, GameplayBgaRole::Base)) {
          return false;
        }
        if (bgaFrame.layer &&
            !appendBgaTarget(*bga, GameplayBgaRole::Layer)) {
          return false;
        }
        break;
      }
      quadStart = commandIndex + 1U;
    }
    appendQuadSegment(buffer.commands.size());
  } catch (...) {
    return false;
  }

  renderer.begin(context, resources);
  bool bgaReady = true;
  GameplayBgaTransientRequirements bgaRequirements;
  if (hasBgaMarker) {
    try {
      const auto result = bgaSubmitter.preflight(bgaFrame, bgaTargets);
      bgaReady = result.ready;
      if (bgaReady) {
        bgaRequirements = result.requirements;
      }
    } catch (...) {
      bgaReady = false;
    }
  }
  rendering::SkinQuadSubmissionPlan quadPlan;
  const bool quadsReady =
      renderer.prepare(quadSegments, quadPlan, bgaRequirements);
  if (!quadsReady || !bgaReady) {
    renderer.discardPrepared(quadPlan);
    return false;
  }
  if (hasBgaMarker) {
    bgaSubmitter.commitPrepared(bgaFrame);
  }

  for (const auto &step : steps) {
    if (step.kind == SubmissionStepKind::QuadSegment) {
      renderer.submitPrepared(quadPlan, step.index);
      renderer.flush();
    } else {
      bgaSubmitter.submitPrepared(bgaFrame, bgaTargets[step.index]);
    }
  }
  // Every fallible check ends above commitPrepared. Submission methods are
  // deliberately void/noexcept, so this phase can never select fullscreen
  // fallback after any authored draw has already been emitted.
  bgaSubmitter.finalizePrepared(bgaFrame);
  return true;
}

} // namespace skin

#endif
