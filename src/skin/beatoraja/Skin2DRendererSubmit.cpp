#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"
#include "../../audio/GameplayBgaFrame.h"
#include "../../rendering/RenderPlan.h"
#include "../../rendering/SkinQuadBatchRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace skin {
namespace {

enum class SubmissionStepKind : std::uint8_t {
  QuadSegment,
  MovieTarget,
  BgaTarget,
};

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
    rendering::SkinQuadBatchRenderer &renderer, SkinMovieCatalog *movies,
    const PlaySkinViewport &viewport) const noexcept {
  std::vector<std::span<const SkinDrawCommand>> quadSegments;
  std::vector<const SkinMovieCommand *> movieCommands;
  std::vector<SubmissionStep> steps;
  try {
    quadSegments.reserve(buffer.commands.size() / 2U + 1U);
    movieCommands.reserve(buffer.commands.size());
    steps.reserve(buffer.commands.size());
    std::size_t quadStart = 0;
    const auto appendQuadSegment = [&](std::size_t end) {
      if (end == quadStart) return;
      quadSegments.emplace_back(buffer.commands.data() + quadStart,
                                end - quadStart);
      steps.push_back({.kind = SubmissionStepKind::QuadSegment,
                       .index = quadSegments.size() - 1U});
    };
    for (std::size_t index = 0; index < buffer.commands.size(); ++index) {
      const auto *movie =
          std::get_if<SkinMovieCommand>(&buffer.commands[index].payload);
      if (movie != nullptr) {
        appendQuadSegment(index);
        movieCommands.push_back(movie);
        steps.push_back({.kind = SubmissionStepKind::MovieTarget,
                         .index = movieCommands.size() - 1U});
        quadStart = index + 1U;
        continue;
      }
      // Result MainState has no BMSResource BGA frame to submit.  Ignore an
      // authored BGA marker rather than sending it to the quad renderer.
      if (std::holds_alternative<SkinBgaCommand>(buffer.commands[index].payload)) {
        appendQuadSegment(index);
        quadStart = index + 1U;
      }
    }
    appendQuadSegment(buffer.commands.size());
  } catch (...) {
    return false;
  }

  renderer.begin(context, resources);
  SkinMovieCatalogFrameResult moviePlan;
  try {
    moviePlan = movies != nullptr
                    ? movies->prepareFrame(movieCommands, viewport)
                    : SkinMovieCatalogFrameResult{
                          .ready = movieCommands.empty()};
  } catch (...) {
    if (movies != nullptr) movies->discardFrame();
    return false;
  }
  if (!moviePlan.ready) {
    if (movies != nullptr) movies->discardFrame();
    return false;
  }
  rendering::SkinQuadSubmissionPlan quadPlan;
  if (!renderer.prepare(quadSegments, quadPlan, moviePlan.requirements)) {
    renderer.discardPrepared(quadPlan);
    if (movies != nullptr) movies->discardFrame();
    return false;
  }
  if (movies != nullptr) movies->commitFrame();
  for (const auto &step : steps) {
    if (step.kind == SubmissionStepKind::QuadSegment) {
      renderer.submitPrepared(quadPlan, step.index);
      renderer.flush();
    } else {
      movies->submitPrepared(step.index);
    }
  }
  if (movies != nullptr) movies->discardFrame();
  return true;
}

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer,
    const SkinPreparedResourceView &resources, RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer,
    SkinMovieCatalog *movies, const PlaySkinViewport &viewport,
    const PreparedGameplayBgaFrame &bgaFrame,
    IGameplayBgaSubmitter &bgaSubmitter) const noexcept {
  std::vector<std::span<const SkinDrawCommand>> quadSegments;
  std::vector<const SkinMovieCommand *> movieCommands;
  std::vector<BgaDrawTarget> bgaTargets;
  std::vector<SubmissionStep> steps;
  bool hasBgaMarker = false;
  bool bgaTargetProjectionFailed = false;
  try {
    quadSegments.reserve(buffer.commands.size() / 2U + 1U);
    bgaTargets.reserve(buffer.commands.size() * 2U);
    movieCommands.reserve(buffer.commands.size());
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
      if (const auto *movie = std::get_if<SkinMovieCommand>(
              &buffer.commands[commandIndex].payload)) {
        appendQuadSegment(commandIndex);
        movieCommands.push_back(movie);
        steps.push_back({.kind = SubmissionStepKind::MovieTarget,
                         .index = movieCommands.size() - 1U});
        quadStart = commandIndex + 1U;
        continue;
      }
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
  SkinMovieCatalogFrameResult moviePlan;
  try {
    moviePlan = movies != nullptr
                    ? movies->prepareFrame(movieCommands, viewport)
                    : SkinMovieCatalogFrameResult{
                          .ready = movieCommands.empty()};
  } catch (...) {
    if (movies != nullptr) {
      movies->discardFrame();
    }
    return false;
  }
  if (!moviePlan.ready) {
    if (movies != nullptr) {
      movies->discardFrame();
    }
    return false;
  }
  bool bgaReady = true;
  GameplayBgaTransientRequirements bgaRequirements = moviePlan.requirements;
  if (hasBgaMarker) {
    try {
      const auto result = bgaSubmitter.preflight(bgaFrame, bgaTargets);
      bgaReady = result.ready;
      if (bgaReady) {
        const auto add = [](std::uint64_t &target, std::uint64_t amount) {
          if (amount > std::numeric_limits<std::uint64_t>::max() - target) {
            return false;
          }
          target += amount;
          return true;
        };
        bgaReady = add(bgaRequirements.vertexBytes,
                       result.requirements.vertexBytes) &&
                   add(bgaRequirements.vertexAlignmentPadding,
                       result.requirements.vertexAlignmentPadding) &&
                   add(bgaRequirements.indexCount,
                       result.requirements.indexCount);
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
    if (movies != nullptr) {
      movies->discardFrame();
    }
    return false;
  }
  if (movies != nullptr) {
    movies->commitFrame();
  }
  if (hasBgaMarker) {
    bgaSubmitter.commitPrepared(bgaFrame);
  }

  for (const auto &step : steps) {
    if (step.kind == SubmissionStepKind::QuadSegment) {
      renderer.submitPrepared(quadPlan, step.index);
      renderer.flush();
    } else if (step.kind == SubmissionStepKind::MovieTarget) {
      movies->submitPrepared(step.index);
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
  if (movies != nullptr) {
    movies->discardFrame();
  }
  return true;
}

} // namespace skin

#endif
