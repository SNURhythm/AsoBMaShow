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
                       .authoredProjection = GameplayBgaAuthoredProjection{
                           .x = rect.x,
                           .y = rect.y,
                           .width = rect.width,
                           .height = rect.height,
                           .centerX = geometry.centerX,
                           .centerY = geometry.centerY,
                           .angleDegrees = geometry.angleDegrees,
                           .authoredToUi = {
                               .m00 = command.viewport.authoredToUi.m00,
                               .m01 = command.viewport.authoredToUi.m01,
                               .tx = command.viewport.authoredToUi.tx,
                               .m10 = command.viewport.authoredToUi.m10,
                               .m11 = command.viewport.authoredToUi.m11,
                               .ty = command.viewport.authoredToUi.ty}},
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
  const auto &skinBounds = projectedSkinScissorBounds(command.viewport);
  GameplayBgaClipRect clip{.x = skinBounds.x,
                            .y = skinBounds.y,
                            .width = skinBounds.width,
                            .height = skinBounds.height};
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
    const double clipRight = std::min(clip.x + clip.width, maximumX);
    const double clipBottom = std::min(clip.y + clip.height, maximumY);
    clip.x = std::max(clip.x, minimumX);
    clip.y = std::max(clip.y, minimumY);
    clip.width = std::max(0.0, clipRight - clip.x);
    clip.height = std::max(0.0, clipBottom - clip.y);
  }
  target.clip = clip;
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

bool Skin2DRenderer::submitOverlay(
    const SkinCommandBuffer &buffer, const SkinPreparedResourceView &resources,
    RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer) const noexcept {
  try {
    renderer.begin(context, resources);
    if (!renderer.submit(buffer.commands)) {
      renderer.flush();
      return false;
    }
    renderer.flush();
    return true;
  } catch (...) {
    return false;
  }
}

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer,
    const SkinPreparedResourceView &resources, RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer,
    const PreparedGameplayBgaFrame &bgaFrame,
    IGameplayBgaSubmitter &bgaSubmitter) const noexcept {
  std::vector<std::span<const SkinDrawCommand>> quadSegments;
  std::vector<BgaDrawTarget> bgaTargets;
  std::vector<SubmissionStep> steps;
  bool hasBgaMarker = false;
  bool bgaTargetProjectionFailed = false;
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
          bgaTargetProjectionFailed = true;
        }
        break;
      case GameplayBgaComposition::MissOnly:
        if (bgaFrame.miss &&
            !appendBgaTarget(*bga, GameplayBgaRole::Miss)) {
          bgaTargetProjectionFailed = true;
        }
        break;
      case GameplayBgaComposition::BaseThenLayer:
        if (!appendBgaTarget(*bga, GameplayBgaRole::Base)) {
          bgaTargetProjectionFailed = true;
        }
        if (bgaFrame.layer &&
            !appendBgaTarget(*bga, GameplayBgaRole::Layer)) {
          bgaTargetProjectionFailed = true;
        }
        break;
      }
      quadStart = commandIndex + 1U;
    }
    appendQuadSegment(buffer.commands.size());
  } catch (...) {
    return false;
  }

  if (bgaTargetProjectionFailed) {
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const auto &step) {
                  return step.kind == SubmissionStepKind::BgaTarget;
                }),
                steps.end());
    bgaTargets.clear();
    hasBgaMarker = false;
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
  if (!bgaReady) {
    // A BGA source can be absent or unsupported independently of a skin.
    // JsonPlaySkinObjectLoader leaves the rest of the skin live in that case;
    // drop only its BGA submission steps and release the prepared frame below.
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const auto &step) {
                  return step.kind == SubmissionStepKind::BgaTarget;
                }),
                steps.end());
    bgaTargets.clear();
    hasBgaMarker = false;
  }
  rendering::SkinQuadSubmissionPlan quadPlan;
  const bool quadsReady =
      renderer.prepare(quadSegments, quadPlan, bgaRequirements);
  if (!quadsReady) {
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
  // Every fallible check ends above commitPrepared. Production submission
  // operations are contractually nonthrowing, and this overload's noexcept
  // boundary prevents any contract violation from unwinding into fullscreen
  // fallback after an authored draw has already been emitted.
  // A failed BGA preflight owns no committed target plan, but its prepared
  // frame can still hold a media lease and must be released.
  bgaSubmitter.finalizePrepared(bgaFrame);
  return true;
}

} // namespace skin

#endif
