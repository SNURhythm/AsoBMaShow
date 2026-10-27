#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "audio/GameplayBgaFrame.h"
#include "audio/GameplayBgaMissStateTracker.h"
#include "audio/Jukebox.h"
#include "scene/SettingsPreviewBga.h"
#include "RAII.h"
#include "rendering/BlurPass.h"
#include "rendering/FullscreenBgaCache.h"
#include "rendering/FrameSubmission.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "skin/beatoraja/SkinDestinationEvaluator.cpp"
#include "utils/Stopwatch.h"
#include "video/VideoPlayer.h"
#include "StorageVideoTestSupport.h"

#include <bx/math.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
thread_local int allocationFailureCountdown = -1;
}

void *operator new(std::size_t size) {
  if (allocationFailureCountdown == 0) {
    allocationFailureCountdown = -1;
    throw std::bad_alloc();
  }
  if (allocationFailureCountdown > 0) {
    --allocationFailureCountdown;
  }
  if (void *allocation = std::malloc(size)) {
    return allocation;
  }
  throw std::bad_alloc();
}

void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept {
  std::free(allocation);
}

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = 1280;
int window_height = 720;
int render_width = 1280;
int render_height = 720;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = 1280;
int ui_view_height = 720;
} // namespace rendering

namespace {

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

PlayfieldJudgeEventClock clockAt(long long bgaTimeMicros,
                                 long long visualTimeMicros = 0) {
  return {.songTimeMicros = 0,
          .visualTimeMicros = visualTimeMicros,
          .bgaTimeMicros = bgaTimeMicros};
}

bool samePoint(const GameplayBgaPoint &left,
               const GameplayBgaPoint &right) {
  return left.x == right.x && left.y == right.y;
}

bool sameClip(const std::optional<GameplayBgaClipRect> &left,
              const std::optional<GameplayBgaClipRect> &right) {
  return left.has_value() == right.has_value() &&
         (!left.has_value() ||
          (left->x == right->x && left->y == right->y &&
           left->width == right->width && left->height == right->height));
}

bool sameTarget(const BgaDrawTarget &left, const BgaDrawTarget &right) {
  const auto sameAuthoredProjection = [&] {
    if (left.authoredProjection.has_value() !=
        right.authoredProjection.has_value()) {
      return false;
    }
    if (!left.authoredProjection) {
      return true;
    }
    const auto &a = *left.authoredProjection;
    const auto &b = *right.authoredProjection;
    return a.x == b.x && a.y == b.y && a.width == b.width &&
           a.height == b.height && a.centerX == b.centerX &&
           a.centerY == b.centerY && a.angleDegrees == b.angleDegrees &&
           a.authoredToUi.m00 == b.authoredToUi.m00 &&
           a.authoredToUi.m01 == b.authoredToUi.m01 &&
           a.authoredToUi.tx == b.authoredToUi.tx &&
           a.authoredToUi.m10 == b.authoredToUi.m10 &&
           a.authoredToUi.m11 == b.authoredToUi.m11 &&
           a.authoredToUi.ty == b.authoredToUi.ty;
  };
  return sameAuthoredProjection() && left.role == right.role &&
         left.viewId == right.viewId &&
         samePoint(left.destination[0], right.destination[0]) &&
         samePoint(left.destination[1], right.destination[1]) &&
         samePoint(left.destination[2], right.destination[2]) &&
         samePoint(left.destination[3], right.destination[3]) &&
         left.stretch == right.stretch && left.tint == right.tint &&
         left.blend == right.blend && sameClip(left.clip, right.clip) &&
         left.authoredOrdinal == right.authoredOrdinal;
}

bool sameSurface(const std::optional<PreparedGameplayBgaSurface> &left,
                 const std::optional<PreparedGameplayBgaSurface> &right) {
  return left.has_value() == right.has_value() &&
         (!left.has_value() ||
          (left->role == right->role && left->mediaKind == right->mediaKind &&
           left->surfaceToken == right->surfaceToken &&
           left->sourceWidth == right->sourceWidth &&
           left->sourceHeight == right->sourceHeight &&
           left->contentIdentity == right->contentIdentity));
}

bool sameFrame(const PreparedGameplayBgaFrame &left,
               const PreparedGameplayBgaFrame &right) {
  return left.sequence == right.sequence && left.composition == right.composition &&
         left.contentEpoch == right.contentEpoch &&
         sameSurface(left.base, right.base) && sameSurface(left.layer, right.layer) &&
         sameSurface(left.miss, right.miss);
}

struct JukeboxBackendControl {
  bool running = false;
};

class JukeboxTestBackend final : public audio::IBackend {
public:
  explicit JukeboxTestBackend(std::shared_ptr<JukeboxBackendControl> control)
      : control_(std::move(control)) {}

  bool start(std::string &) override {
    control_->running = true;
    return true;
  }
  bool stop(std::string &) override {
    control_->running = false;
    return true;
  }
  [[nodiscard]] bool isStarted() const override { return control_->running; }
  [[nodiscard]] audio::RuntimeState runtimeState() const override {
    return {.effectiveSampleRate = 44'100};
  }

private:
  std::shared_ptr<JukeboxBackendControl> control_;
};

class JukeboxTestBackendFactory final : public audio::IBackendFactory {
public:
  explicit JukeboxTestBackendFactory(
      std::shared_ptr<JukeboxBackendControl> control)
      : control_(std::move(control)) {}

  [[nodiscard]] audio::Capabilities capabilities() const override { return {}; }
  std::unique_ptr<audio::IBackend>
  open(const audio::StreamRequest &, audio::RenderCallback, void *,
       std::string &) override {
    return std::make_unique<JukeboxTestBackend>(control_);
  }

private:
  std::shared_ptr<JukeboxBackendControl> control_;
};

class FakeGameplayBgaSubmitter final : public IGameplayBgaSubmitter {
public:
  explicit FakeGameplayBgaSubmitter(bool *destroyed = nullptr)
      : destroyed_(destroyed) {}

  ~FakeGameplayBgaSubmitter() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  PreparedGameplayBgaFrame
  prepareVisualFrameAt(std::uint64_t frameSerial, std::int64_t bgaTimeMicros,
                       const GameplayBgaMissState &missState) override {
    preparedFrameSerial = frameSerial;
    preparedBgaTimeMicros = bgaTimeMicros;
    preparedMissState = missState;
    return prepared;
  }

  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override {
    preflightFrame = frame;
    preflightTargets.assign(targets.begin(), targets.end());
    return {.ready = true};
  }

  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {}

  void submitPrepared(const PreparedGameplayBgaFrame &frame,
                      const BgaDrawTarget &target) noexcept override {
    submittedPreparedFrame = frame;
    submittedTarget = target;
  }

  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {}

  void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept override {
    submittedFullscreenFrame = frame;
  }

  PreparedGameplayBgaFrame prepared;
  std::uint64_t preparedFrameSerial = 0;
  std::int64_t preparedBgaTimeMicros = 0;
  GameplayBgaMissState preparedMissState;
  PreparedGameplayBgaFrame preflightFrame;
  std::vector<BgaDrawTarget> preflightTargets;
  PreparedGameplayBgaFrame submittedPreparedFrame;
  std::optional<BgaDrawTarget> submittedTarget;
  PreparedGameplayBgaFrame submittedFullscreenFrame;

private:
  bool *destroyed_ = nullptr;
};

void testBgaDrawTargetRoleIsIndependentOfViewId() {
  const BgaDrawTarget target{
      .role = GameplayBgaRole::Miss,
      .viewId = 17,
      .destination = {{{.x = 1.0F, .y = 2.0F},
                       {.x = 3.0F, .y = 4.0F},
                       {.x = 5.0F, .y = 6.0F},
                       {.x = 7.0F, .y = 8.0F}}},
      .stretch = skin::SkinStretchMode::KeepAspectRatioNoExpanding,
      .tint = {0.1F, 0.2F, 0.3F, 0.4F},
      .blend = skin::SkinBlendMode::Additive,
      .clip = GameplayBgaClipRect{.x = 10.0, .y = 20.0, .width = 30.0,
                                   .height = 40.0},
      .authoredOrdinal = 9,
  };

  require(target.role == GameplayBgaRole::Miss && target.viewId == 17 &&
              target.destination[3].x == 7.0F &&
              target.stretch == skin::SkinStretchMode::KeepAspectRatioNoExpanding &&
              target.tint[3] == 0.4F &&
              target.blend == skin::SkinBlendMode::Additive &&
              target.clip.has_value() && target.authoredOrdinal == 9,
          "BGA draw targets carry an explicit role independently of view ID");
}

void testGameplayBgaCompositeStateDefaultsToBuiltInFullscreen() {
  const GameplayBgaCompositeState state;
  require(state.frameSerial == 0 &&
              state.mode == GameplayBgaCompositeMode::FullscreenBuiltIn &&
              !state.prepared.has_value(),
          "gameplay BGA composite state defaults to fullscreen built-in without a stale prepared frame");

  GameplayBgaCompositeState embedded{
      .frameSerial = 73,
      .mode = GameplayBgaCompositeMode::EmbeddedSkin,
      .prepared = PreparedGameplayBgaFrame{.sequence = 73}};
  require(embedded.frameSerial == 73 &&
              embedded.mode == GameplayBgaCompositeMode::EmbeddedSkin &&
              embedded.prepared.has_value() &&
              embedded.prepared->sequence == embedded.frameSerial,
          "gameplay BGA composite state carries one prepared value for the exact frame");
}

void testPinnedRoleAndMediaSelectExactBgaMaterials() {
  const auto baseImage = SelectGameplayBgaMaterial(
      GameplayBgaRole::Base, GameplayBgaMediaKind::Image);
  const auto layerImage = SelectGameplayBgaMaterial(
      GameplayBgaRole::Layer, GameplayBgaMediaKind::Image);
  const auto bmsonLayer = SelectGameplayBgaMaterial(
      GameplayBgaRole::Layer, GameplayBgaMediaKind::Image, false);
  require(!bmsonLayer.blackTransparent, "bmson image layers retain authored black pixels");
  const auto baseVideo = SelectGameplayBgaMaterial(
      GameplayBgaRole::Base, GameplayBgaMediaKind::Video);
  const auto layerVideo = SelectGameplayBgaMaterial(
      GameplayBgaRole::Layer, GameplayBgaMediaKind::Video);
  const auto missImage = SelectGameplayBgaMaterial(
      GameplayBgaRole::Miss, GameplayBgaMediaKind::Image);
  const auto missVideo = SelectGameplayBgaMaterial(
      GameplayBgaRole::Miss, GameplayBgaMediaKind::Video);

  require(baseImage.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Linear &&
              baseImage.linearSampling && !baseImage.blackTransparent,
          "base images use pinned LINEAR material semantics");
  require(layerImage.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Layer &&
              !layerImage.linearSampling && layerImage.blackTransparent,
          "layer images use pinned LAYER black-transparent material semantics");
  require(baseVideo.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Ffmpeg &&
              baseVideo.linearSampling && !baseVideo.blackTransparent &&
              layerVideo.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Ffmpeg &&
              layerVideo.linearSampling && !layerVideo.blackTransparent,
          "base and layer videos use pinned FFMPEG material semantics");
  require(missImage.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Linear &&
              missImage.linearSampling && !missImage.blackTransparent &&
              missVideo.referenceRendererType ==
                  GameplayBgaReferenceRendererType::Linear &&
              missVideo.linearSampling && !missVideo.blackTransparent,
          "miss uses pinned LINEAR semantics even when its resource is a movie");

  constexpr std::uint32_t linearFlags = BGFX_SAMPLER_UVW_CLAMP;
  constexpr std::uint32_t pointFlags =
      BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT |
      BGFX_SAMPLER_MAG_POINT;
  require((linearFlags &
           (BGFX_SAMPLER_MIN_MASK | BGFX_SAMPLER_MAG_MASK)) == 0U &&
              (pointFlags & BGFX_SAMPLER_MIN_MASK) ==
                  BGFX_SAMPLER_MIN_POINT &&
              (pointFlags & BGFX_SAMPLER_MAG_MASK) ==
                  BGFX_SAMPLER_MAG_POINT,
          "bgfx encodes true Linear with no point bits and LAYER with explicit Point bits");
}

void testLayerMaterialMakesOnlyExactBlackTransparent() {
  const std::array<std::uint8_t, 16> rgba{
      0, 0, 0, 255, 0, 0, 1, 127, 12, 34, 56, 78, 0, 0, 0, 0};
  const auto layer = MakeGameplayBgaLayerRgba(rgba);
  require(layer == std::vector<std::uint8_t>{
                       0, 0, 0, 0, 0, 0, 1, 127,
                       12, 34, 56, 78, 0, 0, 0, 0},
          "LAYER preprocessing clears alpha only for exact black pixels");
  require(MakeGameplayBgaLayerRgba(std::span(rgba).first(15)).empty(),
          "LAYER preprocessing rejects incomplete RGBA pixels");
}

void testEmbeddedBrightnessMultipliesAuthoredRgbAndPreservesAlpha() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const std::array<float, 4> authored{0.8F, 0.6F, 0.4F, 0.25F};

  require(jukebox.embeddedBgaBrightnessMultiplier() == 1.0F &&
              jukebox.embeddedBgaTint(authored) == authored,
          "embedded BGA brightness defaults to 100 percent");
  jukebox.setEmbeddedBgaBrightnessPercent(50);
  require(jukebox.embeddedBgaBrightnessMultiplier() == 0.5F &&
              jukebox.embeddedBgaTint(authored) ==
                  std::array<float, 4>{0.4F, 0.3F, 0.2F, 0.25F},
          "embedded brightness multiplies authored RGB while preserving alpha");
  jukebox.setEmbeddedBgaBrightnessPercent(-10);
  require(jukebox.embeddedBgaBrightnessMultiplier() == 0.0F,
          "embedded BGA brightness clamps below the setting range");
  jukebox.setEmbeddedBgaBrightnessPercent(130);
  require(jukebox.embeddedBgaBrightnessMultiplier() == 1.0F,
          "embedded BGA brightness clamps above the setting range");
}

void testEmbeddedYuvUsesOneBrightnessAdjustedTintOnEveryVertex() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  jukebox.setEmbeddedBgaBrightnessPercent(25);
  const auto tint = jukebox.embeddedBgaTint({0.8F, 0.4F, 1.0F, 0.6F});
  const std::array<video::VideoQuadPoint, 4> destination{{
      {3.0F, 20.0F}, {30.0F, 17.0F}, {26.0F, -1.0F}, {-2.0F, 4.0F}}};
  const std::array<video::VideoQuadPoint, 4> uvs{{
      {0.1F, 0.9F}, {0.8F, 0.9F}, {0.8F, 0.2F}, {0.1F, 0.2F}}};
  const auto quad = video::makeEmbeddedYuvQuadLayout(
      destination, uvs,
      {.r = tint[0], .g = tint[1], .b = tint[2], .a = tint[3]});
  require(quad.has_value(), "brightness-adjusted embedded YUV quad is valid");
  for (const auto &vertex : quad->vertices) {
    require(vertex.r == 0.2F && vertex.g == 0.1F && vertex.b == 0.25F &&
                vertex.a == 0.6F,
            "one brightness-adjusted authored RGBA tint is replicated to every YUV vertex");
  }

  const auto converted = EvaluateGameplayBgaYuvTint(
      0.5F, 0.6F, 0.4F, {0.5F, 0.25F, 0.75F, 0.4F});
  require(std::abs(converted[0] - 0.1799F) < 0.00001F &&
              std::abs(converted[1] - 0.13425F) < 0.00001F &&
              std::abs(converted[2] - 0.5079F) < 0.00001F &&
              converted[3] == 0.4F,
          "known YUV conversion is multiplied by the uniform authored tint");
}

void testBgaSubmitterPreflightsExactMultipleTargets() {
  FakeGameplayBgaSubmitter submitter;
  const PreparedGameplayBgaFrame frame{
      .sequence = 77,
      .composition = GameplayBgaComposition::BaseThenLayer,
  };
  const std::array targets{
      BgaDrawTarget{.role = GameplayBgaRole::Base,
                    .viewId = 3,
                    .destination = {{{.x = 1.0F, .y = 1.0F},
                                     {.x = 2.0F, .y = 1.0F},
                                     {.x = 2.0F, .y = 2.0F},
                                     {.x = 1.0F, .y = 2.0F}}},
                    .authoredOrdinal = 4},
      BgaDrawTarget{.role = GameplayBgaRole::Layer,
                    .viewId = 3,
                    .destination = {{{.x = 10.0F, .y = 20.0F},
                                     {.x = 30.0F, .y = 20.0F},
                                     {.x = 30.0F, .y = 40.0F},
                                     {.x = 10.0F, .y = 40.0F}}},
                    .stretch = skin::SkinStretchMode::NoResize,
                    .tint = {0.5F, 0.6F, 0.7F, 0.8F},
                    .blend = skin::SkinBlendMode::Multiply,
                    .clip = GameplayBgaClipRect{.x = 11.0, .y = 12.0,
                                                 .width = 13.0, .height = 14.0},
                    .authoredOrdinal = 5},
  };

  const auto result = submitter.preflight(frame, targets);
  require(result.ready && !result.failure.has_value() &&
              sameFrame(submitter.preflightFrame, frame) &&
              submitter.preflightTargets.size() == targets.size() &&
              sameTarget(submitter.preflightTargets[0], targets[0]) &&
              sameTarget(submitter.preflightTargets[1], targets[1]),
          "BGA preflight receives each exact authored draw target");
}

void testBgaSubmitterPreparedFrameIsAnImmutableValue() {
  static_assert(std::is_same_v<
                decltype(std::declval<IGameplayBgaSubmitter &>()
                             .prepareVisualFrameAt(
                                 0, 0,
                                 std::declval<const GameplayBgaMissState &>())),
                PreparedGameplayBgaFrame>);
  static_assert(std::is_same_v<
                decltype(std::declval<IGameplayBgaSubmitter &>()
                             .submitPrepared(
                                 std::declval<const PreparedGameplayBgaFrame &>(),
                                 std::declval<const BgaDrawTarget &>())),
                void>);

  FakeGameplayBgaSubmitter submitter;
  submitter.prepared = {
      .sequence = 91,
      .composition = GameplayBgaComposition::MissOnly,
      .miss = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Miss,
                                         .surfaceToken = 123},
  };
  const GameplayBgaMissState state{.active = true, .startedBgaMicros = 100};
  const auto prepared = submitter.prepareVisualFrameAt(55, 200, state);
  const auto original = prepared;
  const BgaDrawTarget target{.role = GameplayBgaRole::Miss, .viewId = 8};

  submitter.submitPrepared(prepared, target);
  submitter.submitFullscreen(prepared);
  require(submitter.preparedFrameSerial == 55 &&
              submitter.preparedBgaTimeMicros == 200 &&
              submitter.preparedMissState.active &&
              sameFrame(prepared, original) &&
              sameFrame(submitter.submittedPreparedFrame, original) &&
              sameFrame(submitter.submittedFullscreenFrame, original),
          "prepared BGA frames are returned by value and remain unchanged by submission");
}

void testBgaSubmitterHasVirtualDestruction() {
  bool destroyed = false;
  std::unique_ptr<IGameplayBgaSubmitter> submitter =
      std::make_unique<FakeGameplayBgaSubmitter>(&destroyed);
  submitter.reset();
  require(destroyed, "BGA submitters destroy derived implementations virtually");
}

void testJukeboxPreparesOneValueFrameAndNeverUpdatesOnSubmission() {
  static_assert(std::derived_from<Jukebox, IGameplayBgaSubmitter>);

  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const auto before = jukebox.gameplayBgaSubmissionStats();
  const GameplayBgaMissState miss{.active = true,
                                  .startedBgaMicros = 5,
                                  .durationMicros = 500'000,
                                  .triggerSerial = 9};
  const auto first = jukebox.prepareVisualFrameAt(77, 123'456, miss);
  const auto repeat = jukebox.prepareVisualFrameAt(77, 123'456, miss);
  const BgaDrawTarget target{.role = GameplayBgaRole::Base,
                             .viewId = rendering::bga_view,
                             .destination = {{{.x = 0.0F, .y = 1.0F},
                                              {.x = 1.0F, .y = 1.0F},
                                              {.x = 1.0F, .y = 0.0F},
                                              {.x = 0.0F, .y = 0.0F}}}};
  const auto preflight = jukebox.preflight(first, std::span(&target, 1));
  if (preflight.ready) {
    jukebox.commitPrepared(first);
    jukebox.submitPrepared(first, target);
    jukebox.finalizePrepared(first);
  }
  jukebox.submitFullscreen(first);
  const auto afterSubmission =
      jukebox.prepareVisualFrameAt(77, 123'456, miss);
  const auto next = jukebox.prepareVisualFrameAt(78, 123'456, miss);
  const auto after = jukebox.gameplayBgaSubmissionStats();

  require(first.sequence != 0 && sameFrame(first, repeat) &&
              first.sequence != afterSubmission.sequence &&
              afterSubmission.sequence != next.sequence &&
              first.composition == GameplayBgaComposition::Blank &&
              (preflight.ready ? !preflight.failure.has_value()
                               : preflight.failure.has_value()) &&
              afterSubmission.composition == GameplayBgaComposition::Blank &&
              after.preparedFrames == before.preparedFrames + 3 &&
              after.videoUpdates == before.videoUpdates &&
              after.embeddedSubmissions ==
                  before.embeddedSubmissions + (preflight.ready ? 1U : 0U) &&
              after.fullscreenSubmissions == before.fullscreenSubmissions + 1,
          "finalizing a prepared frame invalidates its same-key cache entry "
          "before a fresh sequence is prepared");
}

void testJukeboxPreflightPerformsNoShaderLookupOrFilesystemIo() {
  const auto originalDirectory = std::filesystem::current_path();
  const auto restoreDirectory = makeScopeExit([&originalDirectory] {
    std::error_code ignored;
    std::filesystem::current_path(originalDirectory, ignored);
  });
  const auto sourceDirectory =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  std::filesystem::current_path(sourceDirectory);
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const auto sessionLookupCount = jukebox.gameplayBgaProgramLookupCount();
  const PreparedGameplayBgaFrame frame{
      .sequence = 79, .composition = GameplayBgaComposition::Blank};
  const BgaDrawTarget target{
      .role = GameplayBgaRole::Base,
      .viewId = rendering::ui_view,
      .destination = {{{.x = 0.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 0.0F},
                       {.x = 0.0F, .y = 0.0F}}}};

  const auto shaderlessDirectory =
      std::filesystem::path(__FILE__).parent_path() /
      "fixtures/beatoraja_skin/resources";
  std::filesystem::current_path(shaderlessDirectory);
  const auto result = jukebox.preflight(frame, std::span(&target, 1));

  require(result.ready && !result.failure.has_value() &&
              jukebox.gameplayBgaProgramLookupCount() == sessionLookupCount,
          "BGA preflight uses session-prepared shader handles without lookup or filesystem I/O");
  const auto beforeSubmit = jukebox.gameplayBgaSubmissionStats();
  jukebox.commitPrepared(frame);
  jukebox.submitPrepared(frame, target);
  jukebox.finalizePrepared(frame);
  bgfx::frame();
  require(jukebox.gameplayBgaSubmissionStats().embeddedSubmissions ==
                  beforeSubmit.embeddedSubmissions + 1 &&
              jukebox.gameplayBgaProgramLookupCount() == sessionLookupCount,
          "the live placeholder program remains cached through preflight and submission");
}

void testJukeboxBgaTargetStretchAndTrimmedUvs() {
  BgaDrawTarget target{
      .destination = {{{.x = 0.0F, .y = 100.0F},
                       {.x = 200.0F, .y = 100.0F},
                       {.x = 200.0F, .y = 0.0F},
                       {.x = 0.0F, .y = 0.0F}}},
      .stretch = skin::SkinStretchMode::KeepAspectRatioFitInner};
  const auto inner = ResolveGameplayBgaTargetQuad(target, 400, 100);
  require(inner && inner->destination[0].y == 75.0F &&
              inner->destination[2].y == 25.0F &&
              inner->uvs[0].x == 0.0F && inner->uvs[2].x == 1.0F,
          "fit-inner uses source dimensions and centers the final BGA quad");

  target.stretch = skin::SkinStretchMode::KeepAspectRatioFitOuterTrimmed;
  const auto trimmed = ResolveGameplayBgaTargetQuad(target, 400, 100);
  require(trimmed && trimmed->destination[0].x == 0.0F &&
              trimmed->destination[0].y == 100.0F &&
              trimmed->destination[2].x == 200.0F &&
              trimmed->destination[2].y == 0.0F &&
              trimmed->uvs[0].x == 0.25F && trimmed->uvs[1].x == 0.75F &&
              trimmed->uvs[0].y == 1.0F && trimmed->uvs[2].y == 0.0F,
          "outer-trimmed keeps the authored target while center-cropping UVs");

  target.stretch = skin::SkinStretchMode::NoResizeTrimmed;
  const auto noResizeTrimmed = ResolveGameplayBgaTargetQuad(target, 400, 100);
  require(noResizeTrimmed && noResizeTrimmed->uvs[0].x == 0.25F &&
              noResizeTrimmed->uvs[1].x == 0.75F,
          "no-resize-trimmed uses the same centered source crop");
  require(!ResolveGameplayBgaTargetQuad(target, 0, 100) &&
              !ResolveGameplayBgaTargetQuad(target, 400, 0),
          "nonpositive BGA source dimensions fail preflight geometry resolution");

  target.destination = {{{.x = 0.0F, .y = 100.0F},
                         {.x = 200.0F, .y = 100.0F},
                         {.x = 175.0F, .y = -20.0F},
                         {.x = -10.0F, .y = 0.0F}}};
  target.stretch = skin::SkinStretchMode::Stretch;
  const auto arbitrary = ResolveGameplayBgaTargetQuad(target, 320, 240);
  require(arbitrary && samePoint(arbitrary->destination[0], target.destination[0]) &&
              samePoint(arbitrary->destination[1], target.destination[1]) &&
              samePoint(arbitrary->destination[2], target.destination[2]) &&
              samePoint(arbitrary->destination[3], target.destination[3]),
          "Stretch preserves every authored point of an arbitrary BGA quad");
}

void testBgaStretchPrecedesNonuniformViewportProjection() {
  BgaDrawTarget target{
      .stretch = skin::SkinStretchMode::KeepAspectRatioFitInner,
      .authoredProjection = GameplayBgaAuthoredProjection{
          .x = 0.0,
          .y = 0.0,
          .width = 100.0,
          .height = 100.0,
          .centerX = 0.5,
          .centerY = 0.5,
          .authoredToUi = {.m00 = 2.0, .m11 = 1.0},
      }};
  const auto projected = ResolveGameplayBgaTargetQuad(target, 200, 100);
  require(projected && projected->destination[0].x == 0.0F &&
              projected->destination[0].y == 25.0F &&
              projected->destination[1].x == 200.0F &&
              projected->destination[1].y == 25.0F &&
              projected->destination[2].x == 200.0F &&
              projected->destination[2].y == 75.0F &&
              projected->destination[3].x == 0.0F &&
              projected->destination[3].y == 75.0F,
          "a 2:1 BGA fits to 100x50 in authored space before a 2x/1x "
          "viewport produces a 200x50 UI quad");

  target.authoredProjection = GameplayBgaAuthoredProjection{
      .x = 10.0,
      .y = 20.0,
      .width = 100.0,
      .height = 100.0,
      .centerX = 0.25,
      .centerY = 0.75,
      .angleDegrees = 90.0,
      .authoredToUi = {.m00 = 2.0,
                       .m01 = 0.5,
                       .tx = 3.0,
                       .m10 = 0.25,
                       .m11 = 1.0,
                       .ty = -4.0},
  };
  const auto rotated = ResolveGameplayBgaTargetQuad(target, 200, 100);
  require(rotated && rotated->destination[0].x == 176.75F &&
              rotated->destination[0].y == 71.625F &&
              rotated->destination[1].x == 226.75F &&
              rotated->destination[1].y == 171.625F &&
              rotated->destination[2].x == 126.75F &&
              rotated->destination[2].y == 159.125F &&
              rotated->destination[3].x == 76.75F &&
              rotated->destination[3].y == 59.125F,
          "stretch-adjusted authored geometry rotates around its off-center "
          "pivot before the viewport shear is applied");

  target.stretch = skin::SkinStretchMode::KeepAspectRatioFitOuterTrimmed;
  target.authoredProjection = GameplayBgaAuthoredProjection{
      .x = 0.0,
      .y = 0.0,
      .width = 199.0,
      .height = 100.0,
  };
  const auto trimmed = ResolveGameplayBgaTargetQuad(target, 401, 101);
  require(trimmed &&
              std::abs(trimmed->uvs[0].x - 100.0F / 401.0F) < 0.000001F &&
              std::abs(trimmed->uvs[1].x - 300.0F / 401.0F) < 0.000001F,
          "authored outer trimming keeps the shared projector's Java-style "
          "integer source-region truncation");
}

void testAuthoredProjectionPreservesBgaUvOrientation() {
  BgaDrawTarget raw{
      .destination = {{{.x = 0.0F, .y = 100.0F},
                       {.x = 100.0F, .y = 100.0F},
                       {.x = 100.0F, .y = 0.0F},
                       {.x = 0.0F, .y = 0.0F}}},
      .stretch = skin::SkinStretchMode::Stretch};
  auto authored = raw;
  authored.authoredProjection = GameplayBgaAuthoredProjection{
      .x = 0.0, .y = 0.0, .width = 100.0, .height = 100.0};

  const auto rawFull = ResolveGameplayBgaTargetQuad(raw, 100, 100);
  const auto authoredFull = ResolveGameplayBgaTargetQuad(authored, 100, 100);
  const auto sameUvs = [](const GameplayBgaResolvedQuad &left,
                          const GameplayBgaResolvedQuad &right) {
    for (std::size_t index = 0; index < left.uvs.size(); ++index) {
      if (!samePoint(left.uvs[index], right.uvs[index])) {
        return false;
      }
    }
    return true;
  };
  require(rawFull && authoredFull && sameUvs(*authoredFull, *rawFull) &&
              authoredFull->uvs[0].y == 1.0F &&
              authoredFull->uvs[2].y == 0.0F,
          "identity authored projection keeps the BGA contract's bottom-v=1 "
          "and top-v=0 full-source orientation");

  raw.stretch = skin::SkinStretchMode::KeepAspectRatioFitOuterTrimmed;
  authored.stretch = raw.stretch;
  const auto rawVerticalTrim = ResolveGameplayBgaTargetQuad(raw, 100, 200);
  const auto authoredVerticalTrim =
      ResolveGameplayBgaTargetQuad(authored, 100, 200);
  require(rawVerticalTrim && authoredVerticalTrim &&
              sameUvs(*authoredVerticalTrim, *rawVerticalTrim) &&
              authoredVerticalTrim->uvs[0].y == 0.75F &&
              authoredVerticalTrim->uvs[2].y == 0.25F,
          "vertically trimmed authored projection preserves the raw BGA UV "
          "orientation at the adapter boundary");
}

const bgfx::VertexLayout &capacityProbeLayout() {
  static const bgfx::VertexLayout layout = [] {
    bgfx::VertexLayout value;
    value.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    return value;
  }();
  return layout;
}

void testLaterBgaValidationFailureLeavesTransientCapacityUntouched() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const PreparedGameplayBgaFrame frame{
      .sequence = 700, .composition = GameplayBgaComposition::Blank};
  std::array targets{
      BgaDrawTarget{.role = GameplayBgaRole::Base,
                    .destination = {{{.x = 0.0F, .y = 64.0F},
                                     {.x = 64.0F, .y = 64.0F},
                                     {.x = 64.0F, .y = 0.0F},
                                     {.x = 0.0F, .y = 0.0F}}},
                    .authoredOrdinal = 1},
      BgaDrawTarget{.role = GameplayBgaRole::Base,
                    .destination = {{{.x = 0.0F, .y = 64.0F},
                                     {.x = 64.0F, .y = 64.0F},
                                     {.x = 64.0F, .y = 0.0F},
                                     {.x = 0.0F, .y = 0.0F}}},
                    .authoredOrdinal = 2}};
  targets[1].destination[2].x = std::numeric_limits<float>::quiet_NaN();
  const auto *caps = bgfx::getCaps();
  require(caps != nullptr, "bgfx exposes transient capacity limits");
  const auto probeVertices =
      caps->limits.maxTransientVbSize / capacityProbeLayout().getStride();
  const auto probeIndices = caps->limits.maxTransientIbSize /
                            static_cast<std::uint32_t>(sizeof(std::uint16_t));
  const auto verticesBefore = bgfx::getAvailTransientVertexBuffer(
      probeVertices, capacityProbeLayout());
  const auto indicesBefore =
      bgfx::getAvailTransientIndexBuffer(probeIndices);

  const auto result = jukebox.preflight(frame, targets);

  require(!result.ready &&
              bgfx::getAvailTransientVertexBuffer(probeVertices,
                                                  capacityProbeLayout()) ==
                  verticesBefore &&
              bgfx::getAvailTransientIndexBuffer(probeIndices) == indicesBefore,
          "a later BGA validation failure consumes no transient capacity");
}

void testDuplicateTargetsConsumeDistinctPreparedEntriesExactlyOnce() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const PreparedGameplayBgaFrame frame{
      .sequence = 701, .composition = GameplayBgaComposition::MissOnly};
  const BgaDrawTarget target{
      .role = GameplayBgaRole::Miss,
      .destination = {{{.x = 0.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 0.0F},
                       {.x = 0.0F, .y = 0.0F}}},
      .authoredOrdinal = 9};
  const std::array targets{target, target};
  const auto before = jukebox.gameplayBgaSubmissionStats();
  const auto result = jukebox.preflight(frame, targets);
  require(result.ready, "duplicate no-draw targets can be planned");

  jukebox.commitPrepared(frame);
  jukebox.submitPrepared(frame, target);
  jukebox.submitPrepared(frame, target);
  const auto consumed = jukebox.gameplayBgaSubmissionStats();
  jukebox.submitPrepared(frame, target);
  const auto afterExtra = jukebox.gameplayBgaSubmissionStats();

  require(consumed.embeddedSubmissions == before.embeddedSubmissions + 2 &&
              afterExtra.embeddedSubmissions == consumed.embeddedSubmissions,
          "duplicate byte-identical targets consume two ordered plan entries and cannot be reused");
}

void testAuthoredProjectionParticipatesInPreparedTargetIdentity() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const PreparedGameplayBgaFrame frame{
      .sequence = 703, .composition = GameplayBgaComposition::MissOnly};
  BgaDrawTarget first{
      .role = GameplayBgaRole::Miss,
      .destination = {{{.x = 0.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 64.0F},
                       {.x = 64.0F, .y = 0.0F},
                       {.x = 0.0F, .y = 0.0F}}},
      .authoredProjection = GameplayBgaAuthoredProjection{
          .x = 10.0, .y = 20.0, .width = 30.0, .height = 40.0},
      .authoredOrdinal = 5};
  auto second = first;
  second.authoredProjection->authoredToUi.tx = 1.0;
  const std::array targets{first, second};
  const auto before = jukebox.gameplayBgaSubmissionStats();
  const auto result = jukebox.preflight(frame, targets);
  require(result.ready,
          "distinct authored projections can be prepared in stable order");
  jukebox.commitPrepared(frame);

  jukebox.submitPrepared(frame, second);
  require(jukebox.gameplayBgaSubmissionStats().embeddedSubmissions ==
              before.embeddedSubmissions,
          "a later target with matching raw UI points cannot consume the "
          "earlier authored projection");
  jukebox.submitPrepared(frame, first);
  jukebox.submitPrepared(frame, second);
  require(jukebox.gameplayBgaSubmissionStats().embeddedSubmissions ==
              before.embeddedSubmissions + 2,
          "each distinct authored projection consumes its own prepared entry");
  jukebox.finalizePrepared(frame);
}

bms_parser::TimeLine *appendBgaTimeline(bms_parser::Chart &chart,
                                        long long timingMicros, int base,
                                        int layer) {
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->Timing = timingMicros;
  timeline->BgaBase = base;
  timeline->BgaLayer = layer;
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  return timeline;
}

void appendBgaPoorTimeline(bms_parser::Chart &chart, long long timingMicros,
                           std::vector<int> frames) {
  auto *timeline = appendBgaTimeline(chart, timingMicros, -1, -1);
  timeline->BgaPoor =
      bms_parser::BgaPoorSequence{.Frames = std::move(frames)};
}

void testBmsonConditionalMissSequencesReachRenderedFrames() {
  const std::string source = R"({"version":"1.0.0","info":{"init_bpm":120},
    "bga":{"bga_header":[{"id":5,"name":"fixture.png"},
      {"id":42,"name":"fixture.png"},{"id":77,"name":"fixture.png"},
      {"id":88,"name":"fixture.png"}],
      "bga_sequence":[{"id":100,"sequence":[{"time":0,"id":42},
        {"time":90,"id":88},{"time":500}]},{"id":200,"sequence":[]}],
      "bga_events":[{"y":0,"id":5},{"y":960,"id":999}],
      "layer_events":[{"y":240,"id":42,"condition":"miss"},
        {"y":960,"id":77,"condition":"play"},{"y":1200,"id":77}],
      "poor_events":[{"y":480,"id":100},{"y":720,"id":200}]}})";
  bms_parser::Parser parser;
  std::atomic_bool cancelled{false};
  bms_parser::Chart *raw = nullptr;
  parser.Parse(std::vector<unsigned char>(source.begin(), source.end()), &raw,
               false, false, cancelled, "conditional.bmson");
  const std::unique_ptr<bms_parser::Chart> chart(raw);
  require(chart != nullptr, "bmson conditional BGA fixture parses");
  const auto schedule = BuildBgaPoorSequenceSchedule(*chart);
  require(schedule.size() == 3 && schedule[0].startMicros == 500'000 &&
              schedule[1].startMicros == 1'000'000 &&
              schedule[2].startMicros == 1'500'000,
          "conditional miss layers and poor sequences share the chart-time schedule");
  require(schedule[1].frames == std::vector<int>({42, 88, -1}) &&
              schedule[2].frames.empty(),
          "sequence resources, terminal blanks and empty sequences survive scheduling");
  const GameplayBgaMissState miss{.active = true, .startedBgaMicros = 1'000'000};
  const auto first = SelectGameplayBgaMissComposition(
      std::span<const int>(schedule[1].frames), miss, 1'000'000);
  const auto second = SelectGameplayBgaMissComposition(
      std::span<const int>(schedule[1].frames), miss, 1'250'000);
  require(first.resourceId == 42 && second.resourceId == 88,
          "bmson miss sequences follow the reference's uniform miss-window frame selection");

  const auto originalDirectory = std::filesystem::current_path();
  const auto restoreDirectory = makeScopeExit([&originalDirectory] {
    std::error_code ignored;
    std::filesystem::current_path(originalDirectory, ignored);
  });
  const auto sourceDirectory = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::filesystem::current_path(sourceDirectory);
  chart->Meta.Folder = sourceDirectory / "tests/fixtures/beatoraja_skin/resources";
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  jukebox.loadVisuals(*chart, cancelled);
  const auto idle = jukebox.prepareVisualFrameAt(901, 500'000, {});
  require(idle.base && !idle.layer && !idle.miss,
          "a miss-conditional layer is not visible during ordinary play");
  jukebox.finalizePrepared(idle);
  const auto triggered = jukebox.prepareVisualFrameAt(902, 1'250'000, miss);
  require(triggered.composition == GameplayBgaComposition::MissOnly &&
              triggered.miss && !triggered.base && !triggered.layer,
          "sequence-only image resources are loaded and render as a miss surface");
  jukebox.finalizePrepared(triggered);
  const auto empty = jukebox.prepareVisualFrameAt(903, 1'500'000,
      {.active = true, .startedBgaMicros = 1'500'000});
  require(empty.composition == GameplayBgaComposition::MissOnly &&
              !empty.miss && !empty.base && !empty.layer,
          "an authored empty miss sequence suppresses base and layer without fallback");
  jukebox.finalizePrepared(empty);
  const auto playEvent = jukebox.prepareVisualFrameAt(904, 2'000'000, {});
  require(playEvent.base && !playEvent.layer,
          "unimplemented play triggers and undefined base IDs do not replace the current image");
  jukebox.finalizePrepared(playEvent);
  const auto ordinary = jukebox.prepareVisualFrameAt(905, 2'500'000, {});
  require(ordinary.base && ordinary.layer,
          "standard unconditional bmson layers retain their existing rendering");
  jukebox.finalizePrepared(ordinary);
  const auto seekBack = jukebox.prepareVisualFrameAt(906, 1'250'000, miss);
  require(seekBack.miss && !seekBack.base && !seekBack.layer,
          "seeking backward restores the earlier bmson miss sequence");
  jukebox.finalizePrepared(seekBack);
}

std::uint16_t settledBgfxTextureCount() {
  bgfx::frame();
  bgfx::frame();
  return bgfx::getStats()->numTextures;
}

BgaDrawTarget makeLoadedImageTarget(GameplayBgaRole role,
                                    std::uint32_t authoredOrdinal) {
  return {.role = role,
          .viewId = rendering::ui_view,
          .destination = {{{.x = 0.0F, .y = 64.0F},
                           {.x = 64.0F, .y = 64.0F},
                           {.x = 64.0F, .y = 0.0F},
                           {.x = 0.0F, .y = 0.0F}}},
          .authoredOrdinal = authoredOrdinal};
}

void testOnlyScheduledLayerImageIdsReceiveCompanionTextures() {
  const auto originalDirectory = std::filesystem::current_path();
  const auto restoreDirectory = makeScopeExit([&originalDirectory] {
    std::error_code ignored;
    std::filesystem::current_path(originalDirectory, ignored);
  });
  const auto sourceDirectory =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  std::filesystem::current_path(sourceDirectory);

  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const auto baselineTextures = settledBgfxTextureCount();
  const auto imagePath = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/beatoraja_skin/resources/fixture.png";
  bms_parser::Chart chart;
  chart.Meta.Folder = imagePath.parent_path();
  for (int id = 1; id <= 4; ++id) {
    chart.ReferencedBmpTable.emplace(id, imagePath.filename().string());
  }
  appendBgaTimeline(chart, 0, 2, 2);
  appendBgaTimeline(chart, 100, 1, -1);
  appendBgaPoorTimeline(chart, 1, {3});

  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);
  require(settledBgfxTextureCount() == baselineTextures + 5,
          "only the one scheduled layer image ID receives a companion texture");

  const auto sessionLookupCount = jukebox.gameplayBgaProgramLookupCount();
  const auto shaderlessDirectory = imagePath.parent_path();
  std::filesystem::current_path(shaderlessDirectory);
  const auto baseTarget = makeLoadedImageTarget(GameplayBgaRole::Base, 1);
  const auto layerTarget = makeLoadedImageTarget(GameplayBgaRole::Layer, 2);
  const auto baseAndLayerFrame = jukebox.prepareVisualFrameAt(801, 0, {});
  const auto basePreflight =
      jukebox.preflight(baseAndLayerFrame, std::span(&baseTarget, 1));
  const auto layerPreflight =
      jukebox.preflight(baseAndLayerFrame, std::span(&layerTarget, 1));
  require(baseAndLayerFrame.base.has_value() &&
              baseAndLayerFrame.layer.has_value() && basePreflight.ready &&
              layerPreflight.ready,
          "one image ID reused as base and layer retains both textures and preflights both roles");
  jukebox.finalizePrepared(baseAndLayerFrame);

  const GameplayBgaMissState missState{
      .active = true, .startedBgaMicros = 1};
  const auto missFrame = jukebox.prepareVisualFrameAt(802, 1, missState);
  const auto missTarget = makeLoadedImageTarget(GameplayBgaRole::Miss, 3);
  const auto missPreflight =
      jukebox.preflight(missFrame, std::span(&missTarget, 1));
  require(missFrame.miss.has_value() && missPreflight.ready,
          "a poor-only image uses its single linear texture and preflights");
  jukebox.finalizePrepared(missFrame);

  const auto fullscreenFrame = jukebox.prepareVisualFrameAt(803, 0, {});
  const auto beforeFullscreen = jukebox.gameplayBgaSubmissionStats();
  jukebox.submitFullscreen(fullscreenFrame);
  bgfx::frame();
  const auto afterFullscreen = jukebox.gameplayBgaSubmissionStats();
  require(afterFullscreen.fullscreenSubmissions ==
                  beforeFullscreen.fullscreenSubmissions + 1 &&
              jukebox.gameplayBgaProgramLookupCount() == sessionLookupCount,
          "fullscreen base/layer paths use live session-prepared programs without lookup");

  const auto requireMissingCompanion = [&](int visualId,
                                           std::uint64_t frameSerial) {
    bms_parser::Chart layerProbe;
    appendBgaTimeline(layerProbe, 0, -1, visualId);
    jukebox.schedule(layerProbe, false, cancelled);
    const auto frame = jukebox.prepareVisualFrameAt(frameSerial, 0, {});
    const auto target =
        makeLoadedImageTarget(GameplayBgaRole::Layer, visualId + 10U);
    const auto result = jukebox.preflight(frame, std::span(&target, 1));
    require(frame.layer.has_value() && !result.ready && result.failure &&
                result.failure->code ==
                    "gameplay_bga.image.layer_texture",
            "a non-layer-preloaded image fails closed when later forced into a layer role");
    jukebox.finalizePrepared(frame);
  };
  requireMissingCompanion(1, 804);
  requireMissingCompanion(3, 805);
  requireMissingCompanion(4, 806);

  jukebox.unloadVisuals();
  require(settledBgfxTextureCount() == baselineTextures,
          "teardown releases the primary and selective companion image textures");
}

void testCancelledLayerScanDoesNotLeakRoleIntoReusedBaseId() {
  const auto originalDirectory = std::filesystem::current_path();
  const auto restoreDirectory = makeScopeExit([&originalDirectory] {
    std::error_code ignored;
    std::filesystem::current_path(originalDirectory, ignored);
  });
  std::filesystem::current_path(
      std::filesystem::path(__FILE__).parent_path().parent_path());

  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  const auto baselineTextures = settledBgfxTextureCount();
  const auto imagePath = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/beatoraja_skin/resources/fixture.png";

  bms_parser::Chart cancelledLayerChart;
  cancelledLayerChart.Meta.Folder = imagePath.parent_path();
  cancelledLayerChart.ReferencedBmpTable.emplace(
      7, imagePath.filename().string());
  appendBgaTimeline(cancelledLayerChart, 0, -1, 7);
  std::atomic_bool cancelled = true;
  jukebox.loadVisuals(cancelledLayerChart, cancelled);
  require(settledBgfxTextureCount() == baselineTextures,
          "a cancelled layer chart owns no image textures");

  bms_parser::Chart baseChart;
  baseChart.Meta.Folder = imagePath.parent_path();
  baseChart.ReferencedBmpTable.emplace(7, imagePath.filename().string());
  appendBgaTimeline(baseChart, 0, 7, -1);
  cancelled = false;
  jukebox.loadVisuals(baseChart, cancelled);
  require(settledBgfxTextureCount() == baselineTextures + 1,
          "a reused base-only ID does not inherit a cancelled chart's layer role");

  bms_parser::Chart layerProbe;
  appendBgaTimeline(layerProbe, 0, -1, 7);
  jukebox.schedule(layerProbe, false, cancelled);
  const auto frame = jukebox.prepareVisualFrameAt(807, 0, {});
  const auto target = makeLoadedImageTarget(GameplayBgaRole::Layer, 17);
  const auto result = jukebox.preflight(frame, std::span(&target, 1));
  require(frame.layer.has_value() && !result.ready && result.failure &&
              result.failure->code == "gameplay_bga.image.layer_texture",
          "a reused base-only ID retains no lazily materialized layer companion");
  jukebox.finalizePrepared(frame);
  jukebox.unloadVisuals();
  require(settledBgfxTextureCount() == baselineTextures,
          "reused image ownership is released after cancellation recovery");
}

void testPreparedImageLeaseSurvivesMediaResetUntilFullscreenFallback() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch,
                  std::make_unique<JukeboxTestBackendFactory>(control));
  bms_parser::Chart chart;
  const auto imagePath = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/beatoraja_skin/resources/fixture.png";
  chart.Meta.Folder = imagePath.parent_path();
  chart.ReferencedBmpTable.emplace(1, imagePath.filename().string());
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->Timing = 0;
  timeline->BgaBase = 1;
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);

  const auto before = jukebox.gameplayBgaSubmissionStats();
  const auto frame = jukebox.prepareVisualFrameAt(702, 0, {});
  const auto prepared = jukebox.gameplayBgaSubmissionStats();
  require(frame.base.has_value() &&
              prepared.pinnedFrames == before.pinnedFrames + 1,
          "prepared image media is pinned for the frame");

  jukebox.unloadVisuals();
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames ==
              prepared.pinnedFrames,
          "media reset defers destruction of a prepared image lease");
  jukebox.loadVisuals(chart, cancelled);
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames ==
              prepared.pinnedFrames,
          "replacement media does not invalidate the prior frame's pinned image");
  const auto replacement = jukebox.prepareVisualFrameAt(703, 0, {});
  require(replacement.base && frame.base->contentIdentity != 0 &&
              replacement.base->surfaceToken == frame.base->surfaceToken &&
              replacement.base->contentIdentity != frame.base->contentIdentity &&
              replacement.contentEpoch != frame.contentEpoch,
          "reloading the same authored image ID changes content and lifetime identity");
  jukebox.finalizePrepared(replacement);

  jukebox.submitFullscreen(frame);
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames ==
              before.pinnedFrames,
          "same-frame fullscreen fallback consumes and releases the pinned image lease");
}

bgfx::TextureHandle makeOwnershipTestTexture() {
  const std::uint32_t pixel = 0xffffffffU;
  return bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
                               BGFX_TEXTURE_NONE,
                               bgfx::copy(&pixel, sizeof(pixel)));
}

void testFullscreenBgaIdentityCompletionAndLeases() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  ImageData data{.texture = makeOwnershipTestTexture(), .width = 1, .height = 1};
  auto image = AdoptImageTextureToSharedOwner(data);
  const auto first = jukebox.prepareImageFrame(image);
  const std::array<int, 8> viewport{32, 16, 32, 16, 0, 0, 32, 16};
  const auto keyFor = [&](const PreparedGameplayBgaFrame &frame) {
    return rendering::fullscreenBgaOutputKey(frame, 42, viewport, 0);
  };
  const auto key = keyFor(first);
  rendering::FullscreenBgaCache cache;
  require(key && !cache.canReuse(key), "unproduced output cannot be reused");
  cache.complete(key, false, true);
  require(!cache.canReuse(key), "incomplete output does not populate the cache");
  cache.complete(key, true, true);
  jukebox.finalizePrepared(first);
  const auto repeated = jukebox.prepareImageFrame(image);
  require(repeated.sequence != first.sequence &&
              repeated.base->surfaceToken != first.base->surfaceToken &&
              keyFor(repeated) == key && cache.canReuse(keyFor(repeated)),
          "repeated image content reuses independently of presentation/lease sequence");
  auto stale = repeated;
  ++stale.base->contentIdentity;
  require(!jukebox.consumeFullscreenReuse(stale) &&
              jukebox.gameplayBgaSubmissionStats().pinnedFrames == 1,
          "a mismatched content descriptor cannot consume the live lease");
  require(jukebox.consumeFullscreenReuse(repeated) &&
              !jukebox.consumeFullscreenReuse(repeated) &&
              jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0 &&
              jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions == 0,
          "a reuse consumes exactly one lease without submitting composition");

  auto changed = first;
  changed.layer = changed.base;
  changed.layer->role = GameplayBgaRole::Layer;
  require(!cache.canReuse(keyFor(changed)), "adding a layer invalidates composition");
  changed = first;
  changed.composition = GameplayBgaComposition::MissOnly;
  changed.miss = changed.base;
  changed.miss->role = GameplayBgaRole::Miss;
  changed.base.reset();
  require(!cache.canReuse(keyFor(changed)), "miss-only composition has a distinct identity");
  changed.miss.reset();
  require(!cache.canReuse(keyFor(changed)), "blank miss suppresses the previous image");
  changed.composition = GameplayBgaComposition::Blank;
  require(!cache.canReuse(keyFor(changed)), "blank composition cannot reuse an old base");
  changed = first;
  changed.base->contentIdentity = 0;
  require(!keyFor(changed), "unknown/mutable content keeps conservative recomposition");
  auto viewportChange = viewport;
  ++viewportChange[4];
  require(!cache.canReuse(rendering::fullscreenBgaOutputKey(first, 42, viewportChange, 0)) &&
              !cache.canReuse(rendering::fullscreenBgaOutputKey(first, 43, viewport, 0)) &&
              !cache.canReuse(rendering::fullscreenBgaOutputKey(first, 42, viewport, 1)),
          "viewport, target generation and display mode invalidate output");

  ImageData replacementData{.texture = makeOwnershipTestTexture(), .width = 1, .height = 1};
  auto replacementImage = AdoptImageTextureToSharedOwner(replacementData);
  const auto replacement = jukebox.prepareImageFrame(replacementImage);
  require(!cache.canReuse(keyFor(replacement)), "same-sized replacement pixels get a new identity");
  jukebox.finalizePrepared(replacement);
  const auto beforeSuspend = jukebox.prepareImageFrame(image);
  jukebox.setVisualsSuspended(true);
  jukebox.setVisualsSuspended(false);
  require(!jukebox.consumeFullscreenReuse(beforeSuspend),
          "lifecycle interruption rejects a previously prepared reuse lease");
  jukebox.finalizePrepared(beforeSuspend);
  const auto resumed = jukebox.prepareImageFrame(image);
  require(!cache.canReuse(keyFor(resumed)), "resume invalidates even unchanged still pixels");
  jukebox.finalizePrepared(resumed);
  jukebox.seekVisualsToSongTime(0);
  const auto sought = jukebox.prepareImageFrame(image);
  require(keyFor(sought) != keyFor(resumed), "explicit seek invalidates unchanged still pixels");
  jukebox.finalizePrepared(sought);
  cache.invalidate();
  require(!cache.canReuse(key), "cancelled/export frames cannot retain a completed key");
  cache.complete(key, true, true);
  cache.complete(key, false, true);
  require(!cache.canReuse(key), "a partial overwrite evicts previously complete output");
  cache.complete(key, true, true);
  cache.complete(std::nullopt, true, true);
  require(!cache.canReuse(key), "legacy unknown output never becomes a reusable entry");
  cache.complete(key, true, true);
  require(cache.canReuse(key), "exact render-item capacity accepts every requested draw");
  cache.complete(key, true, false);
  require(!cache.canReuse(key), "dropped render items evict otherwise complete output");
  cache.complete(key, true, false);
  require(!cache.canReuse(key), "repeated dropped frames remain untrustworthy");
  cache.complete(key, true, false);
  require(!cache.canReuse(key), "unknown draw acceptance cannot certify output");
}

void testImageTextureOwnershipTransferIsSingleAndExceptionSafe() {
  ImageData successful{.texture = makeOwnershipTestTexture(),
                       .width = 1,
                       .height = 1,
                       .channels = 4,
                       .layerTexture = makeOwnershipTestTexture()};
  require(bgfx::isValid(successful.texture) &&
              bgfx::isValid(successful.layerTexture),
          "ownership test image textures are created");
  const auto successfulHandle = successful.texture;
  const auto successfulLayerHandle = successful.layerTexture;
  auto owner = AdoptImageTextureToSharedOwner(successful);
  require(owner && owner->texture.idx == successfulHandle.idx &&
              owner->layerTexture.idx == successfulLayerHandle.idx &&
              !bgfx::isValid(successful.texture) &&
              !bgfx::isValid(successful.layerTexture),
          "successful adoption invalidates the loader guard at the exact "
          "shared-owner transfer");
  owner.reset();

  ImageData controlBlockFailure{.texture = makeOwnershipTestTexture(),
                                .width = 1,
                                .height = 1,
                                .channels = 4,
                                .layerTexture = makeOwnershipTestTexture()};
  require(bgfx::isValid(controlBlockFailure.texture) &&
              bgfx::isValid(controlBlockFailure.layerTexture),
          "control-block failure image textures are created");
  allocationFailureCountdown = 1;
  bool threw = false;
  try {
    (void)AdoptImageTextureToSharedOwner(controlBlockFailure);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  allocationFailureCountdown = -1;
  require(threw && !bgfx::isValid(controlBlockFailure.texture) &&
              !bgfx::isValid(controlBlockFailure.layerTexture),
          "a throwing shared control-block allocation leaves only its deleter "
          "owning the transferred textures, never the loader guard");

  ImageData rawAllocationFailure{.texture = makeOwnershipTestTexture(),
                                 .width = 1,
                                 .height = 1,
                                 .channels = 4,
                                 .layerTexture = makeOwnershipTestTexture()};
  require(bgfx::isValid(rawAllocationFailure.texture) &&
              bgfx::isValid(rawAllocationFailure.layerTexture),
          "raw-owner failure image textures are created");
  allocationFailureCountdown = 0;
  threw = false;
  try {
    (void)AdoptImageTextureToSharedOwner(rawAllocationFailure);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  allocationFailureCountdown = -1;
  require(threw && bgfx::isValid(rawAllocationFailure.texture) &&
              bgfx::isValid(rawAllocationFailure.layerTexture),
          "failure before the transfer leaves the loader guard as the sole "
          "texture owner for both image representations");
  bgfx::destroy(rawAllocationFailure.texture);
  bgfx::destroy(rawAllocationFailure.layerTexture);
  rawAllocationFailure.texture = BGFX_INVALID_HANDLE;
  rawAllocationFailure.layerTexture = BGFX_INVALID_HANDLE;
}

void testPreparedFrameRetainsExplicitSurfaceRoles() {
  PreparedGameplayBgaFrame frame{
      .sequence = 42,
      .composition = GameplayBgaComposition::BaseThenLayer,
      .base = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Base,
                                         .mediaKind = GameplayBgaMediaKind::Image,
                                         .surfaceToken = 7,
                                         .sourceWidth = 640,
                                         .sourceHeight = 480},
      .layer = PreparedGameplayBgaSurface{.role = GameplayBgaRole::Layer,
                                          .mediaKind = GameplayBgaMediaKind::Video,
                                          .surfaceToken = 8,
                                          .sourceWidth = 1280,
                                          .sourceHeight = 720},
  };

  require(frame.sequence == 42 && frame.base.has_value() &&
              frame.layer.has_value() && !frame.miss.has_value(),
          "prepared BGA frame keeps its optional role surfaces");
  require(frame.base->role == GameplayBgaRole::Base &&
              frame.layer->role == GameplayBgaRole::Layer &&
              frame.layer->mediaKind == GameplayBgaMediaKind::Video,
          "prepared BGA surfaces retain explicit roles independent of view IDs");
}

void testNoneJudgeDoesNotTriggerMissState() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(None, 0), 0, clockAt(123));

  const auto state = tracker.snapshot();
  require(!state.active && state.triggerSerial == 0,
          "None judgement never creates a miss trigger");
}

void testComboZeroUsesBgaClockAndRepeatedZeroRetriggers() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(Great, 0), 0, clockAt(123, 900'000));
  auto state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 123 &&
              state.durationMicros == kDefaultMissLayerDurationMicros &&
              state.triggerSerial == 1,
          "a real combo-zero judgement triggers from BGA time, not visual time");

  tracker.onJudge(JudgeResult(Kpoor, 0), 0, clockAt(456, 1));
  state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 456 &&
              state.triggerSerial == 2,
          "repeated Kpoor at combo zero restarts the miss sequence");
}

void testZeroStartAndFrameBoundariesAreDeterministic() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(PGreat, 0), 0, clockAt(0));
  auto state = tracker.snapshot();
  require(state.active && state.startedBgaMicros == 0 &&
              state.triggerSerial == 1 && state.isActiveAt(0) &&
              state.isActiveAt(499'999) && state.frameIndexAt(0, 4) == 0,
          "BGA timestamp zero starts an active miss sequence");

  tracker.onJudge(JudgeResult(Poor, 0), 0, clockAt(1));
  state = tracker.snapshot();
  require(state.frameIndexAt(1, 4) == 0 &&
              state.frameIndexAt(166'667, 4) == 0 &&
              state.frameIndexAt(166'668, 4) == 1 &&
              state.frameIndexAt(333'334, 4) == 1 &&
              state.frameIndexAt(333'335, 4) == 2 &&
              state.frameIndexAt(500'000, 4) == 2 &&
              !state.frameIndexAt(500'001, 4).has_value(),
          "four-frame miss indexing matches the end-exclusive pinned boundaries");
  require(!state.frameIndexAt(1, 0).has_value() &&
              state.frameIndexAt(1, 1) == 0,
          "empty and single-frame sequences have deterministic selection");
  require(!state.isActiveAt(0) && state.isActiveAt(1),
          "backward seek recomputes activity without mutating the trigger");

  tracker.reset();
  state = tracker.snapshot();
  require(!state.active && state.startedBgaMicros == 0 &&
              state.durationMicros == kDefaultMissLayerDurationMicros &&
              state.triggerSerial == 0,
          "reset restores the deterministic default miss state");
}

void testMissCompositionSuppressesBaseAndLayerWithoutFallback() {
  GameplayBgaMissStateTracker tracker;
  tracker.onJudge(JudgeResult(Poor, 0), 0, clockAt(100));
  const auto activeMiss = tracker.snapshot();
  const auto noSequence = SelectGameplayBgaMissComposition(
      std::nullopt, activeMiss, 100);
  require(noSequence.composition == GameplayBgaComposition::BaseThenLayer &&
              !noSequence.resourceId.has_value(),
          "an absent channel-06 sequence leaves base and layer visible");

  const auto beforeStart = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss, 99);
  const auto atEnd = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss,
      100 + kDefaultMissLayerDurationMicros);
  require(beforeStart.composition == GameplayBgaComposition::BaseThenLayer &&
              atEnd.composition == GameplayBgaComposition::BaseThenLayer,
          "miss suppression is inactive before its start and at its exclusive end");

  const auto zeroFrames = SelectGameplayBgaMissComposition(
      std::span<const int>{}, activeMiss, 100);
  require(zeroFrames.composition == GameplayBgaComposition::MissOnly &&
              !zeroFrames.resourceId.has_value(),
          "an active empty sequence suppresses base and layer without a resource");

  const std::vector<int> oneFrame{41};
  const auto one = SelectGameplayBgaMissComposition(
      std::span<const int>(oneFrame), activeMiss, 300'000);
  require(one.composition == GameplayBgaComposition::MissOnly &&
              one.resourceId == 41,
          "a one-frame miss sequence selects its only authored resource");

  const std::vector<int> fourFrames{10, 20, 30, 40};
  const auto first = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 100);
  const auto second = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 166'767);
  const auto third = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 333'434);
  const auto fourth = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 499'999);
  require(first.resourceId == 10 && second.resourceId == 20 &&
              third.resourceId == 30 && fourth.resourceId == 30,
          "a four-frame miss sequence preserves the pinned end-exclusive mapping");

  const std::vector<int> blankFrame{kGameplayBgaAuthoredBlank};
  const auto blank = SelectGameplayBgaMissComposition(
      std::span<const int>(blankFrame), activeMiss, 100);
  require(blank.composition == GameplayBgaComposition::MissOnly &&
              !blank.resourceId.has_value(),
          "an authored blank suppresses base and layer without fallback");

  const auto afterSeekBack = SelectGameplayBgaMissComposition(
      std::span<const int>(fourFrames), activeMiss, 100);
  require(afterSeekBack.resourceId == 10,
          "backward BGA time recomputes the selected miss frame without mutation");
}

void testRealVideoAdapterHonorsBoundedGeneratedFixture() {
  Stopwatch stopwatch;
  stopwatch.start();
  VideoPlayer player(&stopwatch);
  std::atomic_bool cancelled{false};
  const auto path = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/charts/"
                    "acceptance_bga_video.mp4";
  const VideoPlayer::LoadLimits limits{
      .maximumDimension = 8,
      .maximumRgbaBytes = 8U * 8U * 4U,
      .maximumDecodedBytes = 4U * 1024U,
      .requirePreallocationBounds = true};
  require(player.loadVideo(path.string(), cancelled, limits) &&
              player.getFrameWidth() == 2 && player.getFrameHeight() == 2 &&
              player.getReservedDecodedBytes() > 0 &&
              player.getReservedDecodedBytes() <= limits.maximumDecodedBytes,
          "real codec adapter opens the deterministic generated 2x2 fixture "
          "within its pre-allocation decoded budget");
}

void testNativeVideoPlaybackSelectsSurfaceRendering() {
  storage_test::TempDirectory directory;
  storage_test::VideoFixture fixture;
  fixture.h264 = true; fixture.width = 320; fixture.height = 240;
  fixture.timestamps = {0, 1, 2, 3, 4, 5};
  fixture.colorRange = AVCOL_RANGE_MPEG; fixture.colorSpace = AVCOL_SPC_BT709;
  const auto path = directory.root / "native-bga.nut";
  storage_test::makeVideo(path, fixture);
  Stopwatch stopwatch;
  VideoPlayer player(&stopwatch);
  std::atomic_bool cancelled{false};
  require(player.loadVideo(path.string(), cancelled), "native BGA fixture loads");
  player.playFrom(0);
  const std::array<video::VideoQuadPoint, 4> corners{{{0, 24}, {32, 24}, {32, 0}, {0, 0}}};
  const std::array<video::VideoQuadPoint, 4> uvs{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
  const auto quad = video::makeEmbeddedYuvQuadLayout(corners, uvs, {});
  std::optional<VideoPlayer::PreparedEmbeddedSubmission> submission;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!submission && std::chrono::steady_clock::now() < deadline) {
    player.update();
    submission = player.prepareEmbeddedSubmission(*quad, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    if (!submission) std::this_thread::yield();
  }
  require(submission.has_value(), "native BGA decoder presents a frame");
  const auto nativeProgram = rendering::ShaderManager::getInstance().getProgram(
      "vs_skin_yuvrgb.bin", "fs_skin_video_nv12.bin");
  require(submission->program.idx == nativeProgram.idx,
          "H264 playback samples native decoder surfaces instead of CPU-uploaded YUV planes");
  const auto nativeIdentity = player.contentIdentity();
  for (int repeat = 0; repeat < 8; ++repeat) player.update();
  require(nativeIdentity != 0 && player.contentIdentity() == nativeIdentity,
          "repeated native surface presentation keeps its completed content identity");

  const auto output = bgfx::createTexture2D(32, 24, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(32, 24, false, 1, bgfx::TextureFormat::RGBA8,
      BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  float projection[16];
  bx::mtxOrtho(projection, 0.0F, 32.0F, 24.0F, 0.0F, 0.0F, 100.0F, 0.0F,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(0, framebuffer);
  bgfx::setViewRect(0, 0, 0, 32, 24);
  bgfx::setViewTransform(0, nullptr, projection);
  const auto renderPixels = [&] {
    player.update();
    player.renderEmbedded(0, *quad, BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::blit(1, readback, 0, 0, output);
    std::vector<uint8_t> pixels(32 * 24 * 4);
    const auto ready = bgfx::readTexture(readback, pixels.data());
    while (bgfx::frame() < ready) {}
    return pixels;
  };
  const auto requireTintedPixels = [&] {
    // Hold the same decoded frame for both paths; this is a pixel comparison,
    // not a comparison between two decoder timestamps.
    player.render(0, 0, 0, 32, 24);
    bgfx::blit(1, readback, 0, 0, output);
    std::vector<std::uint8_t> original(32 * 24 * 4), tinted(original.size());
    auto ready = bgfx::readTexture(readback, original.data());
    while (bgfx::frame() < ready) {}
    require(player.renderFullscreenTinted(0, 0, 0, 32, 24, 0.5F),
            "decoded video supports direct fullscreen brightness");
    bgfx::blit(1, readback, 0, 0, output);
    ready = bgfx::readTexture(readback, tinted.data());
    while (bgfx::frame() < ready) {}
    for (std::size_t offset = 0; offset < original.size(); offset += 4) {
      for (int channel = 0; channel < 3; ++channel)
        require(std::abs(int(tinted[offset + channel]) * 2 - original[offset + channel]) <= 2,
                "direct video clamps converted RGB before applying half brightness");
      require(tinted[offset + 3] == 255, "direct video output is opaque");
    }
  };
  requireTintedPixels();
  const auto firstFrame = renderPixels();
  const auto awaitPixels = [&](bool same) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
      if ((renderPixels() == firstFrame) == same) return true;
      std::this_thread::yield();
    }
    return false;
  };
  player.playFrom(160'000);
  require(player.contentIdentity() != nativeIdentity,
          "forward seek invalidates even the retained native surface before decoding");
  require(awaitPixels(false), "native playback replaces the visible surface after forward seek");
  player.playFrom(0);
  require(awaitPixels(true), "native playback reproduces its first frame after backward seek");
  player.stop();
  require(player.contentIdentity() == 0, "stopped native content cannot be reused");
  require(!player.prepareEmbeddedSubmission(*quad, 0), "stopped native video no longer submits an image");

  // This matrix is valid video but deliberately outside the native shader's
  // supported set. Import must fail safely and reopen the ordinary decoder.
  fixture.colorSpace = AVCOL_SPC_SMPTE240M;
  storage_test::makeVideo(path, fixture);
  require(player.loadVideo(path.string(), cancelled), "software fallback fixture loads");
  require(player.contentIdentity() == 0, "reload cannot expose a previous decoded content identity");
  player.playFrom(0);
  submission.reset();
  const auto fallbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!submission && std::chrono::steady_clock::now() < fallbackDeadline) {
    player.update();
    submission = player.prepareEmbeddedSubmission(*quad, BGFX_STATE_WRITE_RGB);
    if (!submission) std::this_thread::yield();
  }
  const auto softwareProgram = rendering::ShaderManager::getInstance().getProgram(
      "vs_skin_yuvrgb.bin", "fs_skin_yuvrgb.bin");
  require(submission && submission->program.idx == softwareProgram.idx,
          "failed native surface import resumes software playback at the current clock");
  const auto softwareIdentity = player.contentIdentity();
  for (int repeat = 0; repeat < 8; ++repeat) player.update();
  require(softwareIdentity != 0 && softwareIdentity != nativeIdentity &&
              player.contentIdentity() == softwareIdentity,
          "software fallback issues a fresh stable identity after a complete upload");
  player.setDecodeSuspended(true);
  player.setDecodeSuspended(false);
  require(player.contentIdentity() != softwareIdentity,
          "native/software lifecycle changes invalidate retained content identity");
  requireTintedPixels();
  bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer); bgfx::destroy(output); bgfx::destroy(readback);
}

void testDirectFullscreenBgaOnMetal() {
  constexpr int width = 32, height = 16;
  const int oldWidth = rendering::window_width, oldHeight = rendering::window_height;
  rendering::window_width = width;
  rendering::window_height = height;
  const auto restore = makeScopeExit([&] {
    rendering::window_width = oldWidth;
    rendering::window_height = oldHeight;
  });
  const auto output = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  float ortho[16];
  bx::mtxOrtho(ortho, 0.0F, float(width), float(height), 0.0F, 0.0F,
                 100.0F, 0.0F, bgfx::getCaps()->homogeneousDepth);
  for (const auto view : {rendering::bga_view, rendering::final_view}) {
    bgfx::setViewFrameBuffer(view, framebuffer);
    bgfx::setViewRect(view, 0, 0, width, height);
    bgfx::setViewTransform(view, nullptr, ortho);
    bgfx::setViewClear(view, BGFX_CLEAR_COLOR, 0x00ff0000);
  }
  const std::array<std::uint8_t, 16> source{
      192, 0, 0, 255, 192, 0, 0, 255,
      0, 0, 192, 255, 0, 0, 192, 255};
  ImageData data{.texture = bgfx::createTexture2D(2, 2, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_POINT,
      bgfx::copy(source.data(), source.size())), .width = 2, .height = 2, .channels = 3};
  auto image = AdoptImageTextureToSharedOwner(data);
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  jukebox.setBgaDisplayMode(AppSettings::BgaDisplayMode::Fit);
  const auto lookups = jukebox.gameplayBgaProgramLookupCount();
  const auto frame = jukebox.prepareImageFrame(image);
  auto stale = frame;
  ++stale.base->surfaceToken;
  require(!jukebox.trySubmitFullscreenDirect(stale, rendering::final_view, 0.0F, 0.5F) &&
              !jukebox.trySubmitFullscreenDirect(frame, rendering::final_view,
                  std::numeric_limits<float>::quiet_NaN(), 0.5F) &&
              !jukebox.trySubmitFullscreenDirect(frame, rendering::final_view, 0.0F,
                  std::numeric_limits<float>::infinity()),
          "stale frames and nonfinite post-process inputs retain fallback ownership");
  require(!jukebox.trySubmitFullscreenDirect(frame, rendering::final_view, 1.0F, 0.5F),
          "positive blur must retain the composited path");
  auto layered = frame;
  layered.layer = frame.base;
  require(!jukebox.trySubmitFullscreenDirect(layered, rendering::final_view, 0.0F, 0.5F),
          "a layered frame cannot bypass composition");
  image->channels = 4;
  require(!jukebox.trySubmitFullscreenDirect(frame, rendering::final_view, 0.0F, 0.5F),
          "unknown or alpha-bearing images cannot bypass composition");
  image->channels = 3;
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 1 &&
              jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions == 0,
          "rejected direct attempts leave the same-frame lease and submission untouched");
  require(jukebox.trySubmitFullscreenDirect(frame, rendering::final_view, 0.0F, 0.5F),
          "single opaque zero-blur BGA submits directly");
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0 &&
              jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions == 1 &&
              jukebox.gameplayBgaProgramLookupCount() == lookups,
          "direct submission releases its lease without a per-frame shader lookup");
  require(!jukebox.trySubmitFullscreenDirect(frame, rendering::final_view, 0.0F, 1.0F),
          "a consumed frame cannot submit again");
  bgfx::blit(rendering::readback_view, readback, 0, 0, output);
  auto current = bgfx::frame();
  std::array<std::uint8_t, width * height * 4> pixels{};
  const auto ready = bgfx::readTexture(readback, pixels.data());
  current = bgfx::frame();
  require(bgfx::getStats()->numDraw == 1,
          "direct BGA uses exactly one draw without intermediate composition");
  for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
  require(current >= ready, "direct BGA readback completes");
  const auto top = (3 * width + 16) * 4, bottom = (12 * width + 16) * 4;
  require(std::abs(int(pixels[top]) - 96) <= 1 && pixels[top + 2] == 0 &&
              pixels[bottom] == 0 && std::abs(int(pixels[bottom + 2]) - 96) <= 1,
          "direct BGA preserves orientation and half brightness");
  for (const int x : {2, 29}) {
    const auto offset = (8 * width + x) * 4;
    require(pixels[offset] == 0 && pixels[offset + 1] == 0 &&
                pixels[offset + 2] == 0 && pixels[offset + 3] == 255,
            "fit-mode letterbox bars are opaque black");
  }
  require(pixels[top + 3] == 255 && pixels[bottom + 3] == 255,
          "direct BGA content remains opaque");
  const auto readDirectPixels = [&](AppSettings::BgaDisplayMode mode, float brightness) {
    jukebox.setBgaDisplayMode(mode);
    const auto next = jukebox.prepareImageFrame(image);
    require(jukebox.trySubmitFullscreenDirect(next, rendering::final_view, 0.0F, brightness),
            "eligible frames remain direct across display-mode changes");
    bgfx::blit(rendering::readback_view, readback, 0, 0, output);
    const auto ready = bgfx::readTexture(readback, pixels.data());
    auto current = bgfx::frame();
    for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
    require(current >= ready, "display-mode readback completes");
  };
  readDirectPixels(AppSettings::BgaDisplayMode::Fill, 2.0F);
  require(pixels[(3 * width + 2) * 4] == 192 && pixels[(12 * width + 29) * 4 + 2] == 192,
          "fill mode covers the viewport and brightness clamps at one");
  readDirectPixels(AppSettings::BgaDisplayMode::NoExpand, 1.0F);
  require(pixels[(7 * width + 15) * 4] == 192 && pixels[(8 * width + 16) * 4 + 2] == 192 &&
              pixels[(3 * width + 16) * 4] == 0,
          "no-expand preserves native image dimensions at the centered position");
  bgfx::setViewRect(rendering::final_view, 4, 2, 24, 12);
  readDirectPixels(AppSettings::BgaDisplayMode::Fit, 0.5F);
  require(std::abs(int(pixels[(4 * width + 16) * 4]) - 96) <= 1 &&
              pixels[(8 * width + 5) * 4] == 0 && pixels[(8 * width + 5) * 4 + 3] == 255,
          "direct composition respects the caller's offset output viewport");
  bgfx::setViewRect(rendering::final_view, 0, 0, width, height);
  const auto delayed = jukebox.prepareImageFrame(image);
  bgfx::TransientVertexBuffer exhausted{};
  const auto available = bgfx::getAvailTransientVertexBuffer(
      std::numeric_limits<std::uint32_t>::max(), rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientVertexBuffer(&exhausted, available, rendering::PosTexCoord0Vertex::ms_decl);
  require(!jukebox.trySubmitFullscreenDirect(delayed, rendering::final_view, 0.0F, 1.0F) &&
              jukebox.gameplayBgaSubmissionStats().pinnedFrames == 1,
          "transient exhaustion rejects direct submission without consuming the lease");
  bgfx::frame();
  require(jukebox.trySubmitFullscreenDirect(delayed, rendering::final_view, 0.0F, 0.0F),
          "a rejected frame can submit after transient capacity returns");
  bgfx::blit(rendering::readback_view, readback, 0, 0, output);
  const auto blackReady = bgfx::readTexture(readback, pixels.data());
  while (bgfx::frame() < blackReady) {}
  require(pixels[top] == 0 && pixels[top + 1] == 0 && pixels[top + 2] == 0 &&
              pixels[top + 3] == 255,
          "zero brightness renders opaque black");
  const auto beforeFallback = jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions;
  const auto fallback = jukebox.prepareImageFrame(image);
  require(!jukebox.trySubmitFullscreenDirect(fallback, rendering::final_view, 2.0F, 1.0F),
          "switching blur back on rejects the direct path");
  jukebox.submitFullscreen(fallback);
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0 &&
              jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions == beforeFallback + 1,
          "same-frame fullscreen fallback consumes the preserved lease once");
  bgfx::frame();
  for (const auto view : {rendering::bga_view, rendering::final_view})
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer); bgfx::destroy(output); bgfx::destroy(readback);
}

void testFullscreenBgaReuseOnMetal() {
  constexpr int width = 32, height = 16;
  const int oldWidth = rendering::window_width, oldHeight = rendering::window_height;
  rendering::window_width = width;
  rendering::window_height = height;
  const auto restore = makeScopeExit([&] {
    rendering::window_width = oldWidth;
    rendering::window_height = oldHeight;
  });
  const auto output = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  require(bgfx::isValid(output) && bgfx::isValid(readback) && bgfx::isValid(framebuffer),
          "reuse probe creates real Metal readback targets");
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  auto displayMode = AppSettings::BgaDisplayMode::Stretch;
  jukebox.setBgaDisplayMode(displayMode);
  rendering::BlurPass blur(2, 0.6F);
  blur.init(width, height);
  blur.setBlurStrength(2);
  blur.setCompositeEnabled(false);
  blur.setInputViews({rendering::bga_view, rendering::bga_layer_view});
  rendering::applyViewOrder(blur.blurViewH(), blur.blurViewV(), blur.finalView());
  float ortho[16];
  bx::mtxOrtho(ortho, 0.0F, float(width), float(height), 0.0F, 0.0F,
                100.0F, 0.0F, bgfx::getCaps()->homogeneousDepth);
  for (auto view : {rendering::bga_view, rendering::bga_layer_view, blur.finalView()}) {
    bgfx::setViewTransform(view, nullptr, ortho);
    bgfx::setViewClear(view, view == rendering::bga_layer_view ? BGFX_CLEAR_NONE : BGFX_CLEAR_COLOR, 0);
  }
  bgfx::setViewFrameBuffer(blur.finalView(), framebuffer);
  bgfx::setViewRect(blur.finalView(), 0, 0, width, height);
  rendering::FullscreenBgaCache cache;
  const auto exhaust = [] {
    bgfx::TransientVertexBuffer buffer{};
    // bgfx computes offset + requested * stride in uint32_t. UINT32_MAX
    // wraps once composition has consumed vertices and fails to exhaust the
    // pool. Bound the request by the actual configured byte capacity.
    const auto requested = bgfx::getCaps()->limits.maxTransientVbSize /
        rendering::PosTexCoord0Vertex::ms_decl.getStride();
    const auto count = bgfx::getAvailTransientVertexBuffer(
        requested, rendering::PosTexCoord0Vertex::ms_decl);
    if (count) bgfx::allocTransientVertexBuffer(&buffer, count, rendering::PosTexCoord0Vertex::ms_decl);
    require(bgfx::getAvailTransientVertexBuffer(4, rendering::PosTexCoord0Vertex::ms_decl) < 4,
            "failure fixture exhausts capacity even after prior composition/blur allocations");
  };
  struct Result {
    std::array<std::uint8_t, width * height * 4> pixels{};
    std::uint32_t draws = 0;
  };
  // Stages 1/2/3 exhaust real transient capacity before composition/blur/final;
  // stage 4 discards the entire queued frame after successful submissions;
  // stage 5 fills render items with touches while leaving geometry available.
  const auto present = [&](const PreparedGameplayBgaFrame &frame, bool expectReuse,
                           float brightness = 0.5F, int failureStage = 0) {
    require(blur.prepareOutput(), "reuse probe prepares complete targets");
    bgfx::setViewRect(rendering::bga_view, 0, 0, blur.sceneWidth(), blur.sceneHeight());
    bgfx::setViewRect(rendering::bga_layer_view, 0, 0, blur.sceneWidth(), blur.sceneHeight());
    const auto key = rendering::fullscreenBgaOutputKey(frame, blur.targetGeneration(),
        {width, height, width, height, 0, 0, width, height}, static_cast<int>(displayMode));
    const bool reuse = cache.canReuse(key) && jukebox.consumeFullscreenReuse(frame);
    require(reuse == expectReuse, "production identity chooses the expected reuse path");
    bool compositionComplete = reuse, blurComplete = reuse;
    if (!reuse) {
      cache.invalidate();
      if (failureStage == 5) {
        const auto limit = bgfx::getCaps()->limits.maxDrawCalls;
        require(limit > 0 && limit <= 65'535, "render-item saturation fixture stays bounded");
        const auto vertices = bgfx::getAvailTransientVertexBuffer(16, rendering::PosTexCoord0Vertex::ms_decl);
        const auto indices = bgfx::getAvailTransientIndexBuffer(24);
        require(vertices == 16 && indices == 24, "all BGA geometry fits before render-item saturation");
        bgfx::setViewFrameBuffer(rendering::main_view, framebuffer);
        bgfx::setViewRect(rendering::main_view, 0, 0, width, height);
        bgfx::setViewClear(rendering::main_view, BGFX_CLEAR_NONE);
        for (std::uint32_t item = 0; item < limit; ++item) bgfx::touch(rendering::main_view);
        require(bgfx::getAvailTransientVertexBuffer(16, rendering::PosTexCoord0Vertex::ms_decl) == vertices &&
                    bgfx::getAvailTransientIndexBuffer(24) == indices,
                "render-item exhaustion consumes no transient vertex or index capacity");
      }
      bgfx::touch(rendering::bga_view);
      bgfx::touch(rendering::bga_layer_view);
      if (failureStage == 1) exhaust();
      compositionComplete = jukebox.submitFullscreenComplete(frame);
      if (failureStage == 2) exhaust();
      blurComplete = blur.executeComplete();
    }
    if (failureStage == 3) exhaust();
    const bool finalComplete = blurComplete && blur.drawOutput(brightness);
    const bool complete = compositionComplete && finalComplete && failureStage != 4;
    const bool expectComplete = failureStage == 0 || failureStage == 5;
    if (complete != expectComplete) {
      std::cerr << "BGA reuse completion: sequence=" << frame.sequence
                << " stage=" << failureStage << " reuse=" << reuse
                << " composition=" << compositionComplete << " blur=" << blurComplete
                << " final=" << finalComplete << " key=" << key.has_value()
                << " baseIdentity=" << (frame.base ? frame.base->contentIdentity : 0)
                << " targetGeneration=" << blur.targetGeneration()
                << " vertexCapacity=" << bgfx::getAvailTransientVertexBuffer(
                       1'000'000, rendering::PosTexCoord0Vertex::ms_decl)
                << " indexCapacity=" << bgfx::getAvailTransientIndexBuffer(1'000'000)
                << " leases=" << jukebox.gameplayBgaSubmissionStats().pinnedFrames << '\n';
    }
    require(complete == expectComplete, "complete output reports transient failures");
    require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0,
            "recompute, reuse and failed submission all release their prepared leases");
    Result result;
    if (complete) bgfx::blit(rendering::readback_view, readback, 0, 0, output);
    const bool drawsAccepted = rendering::frameDrawsAccepted();
    auto current = bgfx::frame(failureStage == 4 ? BGFX_FRAME_DISCARD : BGFX_FRAME_NONE);
    // Read immediately after frame(), before any extra readback/drain frames:
    // encoder demand is current here even though numDraw is still delayed.
    const auto drawPeak = bgfx::getStats()->numDrawCallsPeak;
    const auto drawCapacity = bgfx::getCaps()->limits.maxDrawCalls;
    cache.complete(key, complete, drawsAccepted);
    if (failureStage == 5)
      require(drawPeak > drawCapacity && !cache.canReuse(key),
              "same-frame CPU draw demand rejects the changed content before any reuse opportunity");
    require(cache.canReuse(key) == (complete && key.has_value() && drawsAccepted),
            "only complete output with accepted render items becomes reusable");
    const auto ready = complete ? bgfx::readTexture(readback, result.pixels.data()) : current;
    current = bgfx::frame();
    result.draws = bgfx::getStats()->numDraw;
    for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
    require(current >= ready, "reuse probe readback finishes within a bounded frame count");
    if (complete) {
      const unsigned surfaces = frame.base.has_value() + frame.layer.has_value() + frame.miss.has_value();
      require(result.draws == (failureStage == 5 ? drawCapacity : reuse ? 1U : surfaces + 5U),
              "reuse submits only the final quad; recompute includes inputs, clears and both blur passes");
    }
    return result;
  };
  const auto makeImage = [&](bool replacement) {
    std::array<std::uint8_t, 16> pixels{192, 0, 0, 255, 0, 192, 0, 255,
                                     0, 0, 192, 255, 192, 192, 0, 255};
    if (replacement) std::swap(pixels[0], pixels[2]);
    ImageData data{.texture = bgfx::createTexture2D(2, 2, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_POINT, bgfx::copy(pixels.data(), pixels.size())),
        .width = 2, .height = 2, .channels = 4};
    return AdoptImageTextureToSharedOwner(data);
  };
  auto image = makeImage(false);
  const auto original = present(jukebox.prepareImageFrame(image), false);
  const auto reused = present(jukebox.prepareImageFrame(image), true);
  require(original.pixels == reused.pixels &&
              std::any_of(original.pixels.begin(), original.pixels.end(), [](auto c) { return c > 0 && c < 255; }),
          "unchanged still image reuse matches every real Metal pixel and is nonblack");
  const auto brighter = present(jukebox.prepareImageFrame(image), true, 1.0F);
  cache.invalidate();
  require(present(jukebox.prepareImageFrame(image), false, 1.0F).pixels == brighter.pixels &&
              brighter.pixels != original.pixels,
          "live brightness on reused output matches fresh composition");
  const auto generation = blur.targetGeneration();
  blur.setBlurStrength(2);
  require(blur.targetGeneration() == generation, "unchanged strength preserves target identity");
  image = makeImage(true);
  require(present(jukebox.prepareImageFrame(image), false).pixels != original.pixels,
          "replacement still pixels never reuse the old output");
  for (int stage : {1, 2, 3, 4}) {
    cache.invalidate();
    present(jukebox.prepareImageFrame(image), false, 0.5F, stage);
    const auto recovered = present(jukebox.prepareImageFrame(image), false);
    require(present(jukebox.prepareImageFrame(image), true).pixels == recovered.pixels,
            "each failed output stage recovers with full recomposition before reuse");
  }
  present(jukebox.prepareImageFrame(image), true, 0.5F, 3);
  present(jukebox.prepareImageFrame(image), false);
  blur.setBlurStrength(3);
  present(jukebox.prepareImageFrame(image), false);
  blur.resize(width, height); // Same dimensions, recreated handles/contents.
  present(jukebox.prepareImageFrame(image), false);
  blur.setDownsample(1);
  present(jukebox.prepareImageFrame(image), false);
  blur.resize(width * 2, height * 2);
  present(jukebox.prepareImageFrame(image), false);
  displayMode = AppSettings::BgaDisplayMode::Fit;
  jukebox.setBgaDisplayMode(displayMode);
  present(jukebox.prepareImageFrame(image), false);
  blur.setInputViews({rendering::bga_view, rendering::bga_layer_view}); // Reset/export restore.
  present(jukebox.prepareImageFrame(image), false);
  blur.shutdown();
  blur.init(width, height);
  present(jukebox.prepareImageFrame(image), false);

  std::atomic_bool cancelled{false};
  bms_parser::Chart chart;
  chart.Meta.Folder = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests/fixtures/beatoraja_skin/charts";
  chart.ReferencedBmpTable = {{1, "acceptance_bga_base.png"}, {2, "acceptance_bga_layer.png"},
                             {3, "acceptance_bga_miss.png"}};
  appendBgaTimeline(chart, 0, 1, 2);
  appendBgaPoorTimeline(chart, 0, {3});
  appendBgaPoorTimeline(chart, 100, {kGameplayBgaAuthoredBlank});
  jukebox.loadVisuals(chart, cancelled);
  std::uint64_t serial = 1;
  auto prepare = [&](long long micros, GameplayBgaMissState miss = {}) {
    return jukebox.prepareVisualFrameAt(serial++, micros, miss);
  };
  const auto layered = present(prepare(1), false);
  require(present(prepare(2), true).pixels == layered.pixels,
          "layered images reuse the complete composed pixels");
  const GameplayBgaMissState miss{.active = true, .startedBgaMicros = 1};
  const auto poor = present(prepare(3, miss), false);
  require(poor.pixels != layered.pixels && present(prepare(4, miss), true).pixels == poor.pixels,
          "miss-only output suppresses base/layer and then reuses unchanged poor content");
  const auto blank = present(prepare(100, miss), false);
  for (std::size_t offset = 0; offset < blank.pixels.size(); offset += 4)
    require(blank.pixels[offset] == 0 && blank.pixels[offset + 1] == 0 &&
                blank.pixels[offset + 2] == 0 && blank.pixels[offset + 3] == 255,
            "blank miss clears the prior image to opaque black");
  require(present(prepare(101, miss), true).pixels == blank.pixels,
          "blank output can be reused without reviving the old image");
  present(prepare(102), false);
  jukebox.seekVisualsToSongTime(0);
  present(prepare(0), false);
  jukebox.setVisualsSuspended(true);
  present(prepare(1), false);
  jukebox.setVisualsSuspended(false);
  present(prepare(2), false);

  // Six bounded decoded frames per backend, paused logical time, no sleeps.
  storage_test::TempDirectory directory;
  for (bool native : {false, true}) {
    storage_test::VideoFixture fixture;
    fixture.width = native ? 320 : 32; fixture.height = native ? 240 : 16;
    fixture.h264 = native;
    fixture.lossless = !native; fixture.timestamps = {0, 1, 2, 3, 4, 5};
    fixture.colorRange = AVCOL_RANGE_MPEG; fixture.colorSpace = AVCOL_SPC_BT709;
    // BGA resolution accepts .mp4; FFmpeg sniffs the fixture's NUT container.
    const auto path = directory.root / (native ? "native.mp4" : "software.mp4");
    storage_test::makeVideo(path, fixture);
    bms_parser::Chart videoChart;
    videoChart.Meta.Folder = directory.root;
    videoChart.ReferencedBmpTable.emplace(1, path.filename().string());
    appendBgaTimeline(videoChart, 0, 1, -1);
    jukebox.loadVisuals(videoChart, cancelled);
    const auto awaitVideo = [&](long long micros, std::uint64_t previous = 0) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      do {
        auto frame = prepare(micros);
        if (frame.base && frame.base->contentIdentity != 0 &&
            frame.base->contentIdentity != previous) return frame;
        jukebox.finalizePrepared(frame);
        std::this_thread::yield();
      } while (std::chrono::steady_clock::now() < deadline);
      require(false, "bounded video decoder produces the requested content identity");
      return PreparedGameplayBgaFrame{};
    };
    auto frame = awaitVideo(0);
    const auto firstIdentity = frame.base->contentIdentity;
    const auto firstPixels = present(frame, false).pixels;
    require(present(prepare(0), true).pixels == firstPixels,
            "repeated software/native decoded frame reuses identical pixels");
    frame = awaitVideo(40'000, firstIdentity);
    const auto newPixels = present(frame, false).pixels;
    require(newPixels != firstPixels && present(prepare(40'000), true).pixels == newPixels,
            "new software/native decoded content invalidates then reuses");
    jukebox.seekVisualsToSongTime(0);
    frame = awaitVideo(0);
    require(frame.base->contentIdentity != firstIdentity,
            "backward seek cannot collide with an earlier decoded frame identity");
    require(present(frame, false).pixels == firstPixels,
            "backward seek recomposes the original decoded pixels");
  }
  // Recovery must restore reuse despite the excessive historical demand peak.
  image = makeImage(false);
  const auto beforeSaturation = present(jukebox.prepareImageFrame(image), false);
  require(present(jukebox.prepareImageFrame(image), true).pixels == beforeSaturation.pixels,
          "output A is reusable before saturation");
  image = makeImage(true);
  const auto droppedReplacement = present(jukebox.prepareImageFrame(image), false, 0.5F, 5);
  require(droppedReplacement.pixels == beforeSaturation.pixels,
          "actual render-item saturation drops B's composition and retains A pixels");
  const auto recoveredReplacement = present(jukebox.prepareImageFrame(image), false);
  require(recoveredReplacement.draws == 6 && recoveredReplacement.pixels != beforeSaturation.pixels &&
              present(jukebox.prepareImageFrame(image), true).pixels == recoveredReplacement.pixels,
          "the next successful B frame recomposes and subsequent frames reuse correct pixels");
  jukebox.unloadVisuals();
  for (const auto view : {rendering::bga_view, rendering::bga_layer_view,
                          blur.blurViewH(), blur.blurViewV(), blur.finalView(), rendering::main_view}) {
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    bgfx::setViewTransform(view, nullptr, nullptr);
  }
  bgfx::setViewOrder(0);
  bgfx::destroy(framebuffer); bgfx::destroy(output); bgfx::destroy(readback);
  bgfx::frame();
}

void testFullscreenBlurBypassOnMetal() {
  constexpr int width = 32, height = 16;
  const int oldWidth = rendering::window_width, oldHeight = rendering::window_height;
  rendering::window_width = width;
  rendering::window_height = height;
  const auto restore = makeScopeExit([&] {
    rendering::window_width = oldWidth;
    rendering::window_height = oldHeight;
  });
  const auto output = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  // Distinct halves expose stale blurred output; partial alpha must not dim
  // the final fullscreen composite, which has always been opaque.
  std::array<std::uint8_t, width * height * 4> source{};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto offset = (y * width + x) * 4;
      source[offset] = x < width / 2 ? 192 : 0;
      source[offset + 1] = x < width / 2 ? 0 : 192;
      source[offset + 2] = y < height / 2 ? 0 : 96;
      source[offset + 3] = 64;
    }
  }
  const auto input = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_POINT,
      bgfx::copy(source.data(), source.size()));
  require(bgfx::isValid(output) && bgfx::isValid(readback) &&
              bgfx::isValid(framebuffer) && bgfx::isValid(input),
          "blur probe creates Metal resources");
  const auto inputProgram = rendering::ShaderManager::getInstance().getProgram(
      "blur/vs_blur.bin", "fs_text.bin");
  const auto sampler = rendering::UniformCache::getInstance().getSampler("s_texColor");
  const auto baseTextures = settledBgfxTextureCount();
  {
    rendering::BlurPass blur(2, 0.6F);
    blur.setBlurStrength(0);
    blur.init(width, height);
    blur.setCompositeEnabled(false);
    blur.setInputViews({rendering::bga_view, rendering::bga_layer_view});
    rendering::applyViewOrder(blur.blurViewH(), blur.blurViewV(), rendering::final_view);
    bgfx::setDebug(BGFX_DEBUG_PROFILER);
    for (const auto view : {rendering::clear_view, rendering::main_view, rendering::ui_view}) {
      bgfx::setViewFrameBuffer(view, framebuffer);
      bgfx::setViewRect(view, 0, 0, width, height);
      bgfx::setViewClear(view, view == rendering::clear_view ? BGFX_CLEAR_COLOR : BGFX_CLEAR_NONE,
                         0x0000ffffU);
    }
    float ortho[16];
    bx::mtxOrtho(ortho, 0.0F, float(width), float(height), 0.0F, 0.0F,
                 100.0F, 0.0F, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewFrameBuffer(rendering::final_view, framebuffer);
    bgfx::setViewRect(rendering::final_view, 0, 0, width, height);
    bgfx::setViewTransform(rendering::final_view, nullptr, ortho);
    bgfx::setViewClear(rendering::final_view, BGFX_CLEAR_NONE);
    const std::array strengths{0.0F, 1.0F, 0.0F, -1.0F};
    for (std::size_t frame = 0; frame < strengths.size(); ++frame) {
      blur.setBlurStrength(strengths[frame]);
      if (frame == 2) blur.resize(width * 2, height * 2);
      bgfx::setViewRect(rendering::bga_view, 0, 0, blur.sceneWidth(), blur.sceneHeight());
      rendering::screenSpaceQuad();
      bgfx::setTexture(0, sampler, input);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
      bgfx::submit(rendering::bga_view, inputProgram);
      bgfx::touch(rendering::bga_layer_view);
      bgfx::touch(rendering::clear_view);
      bgfx::touch(rendering::main_view);
      bgfx::touch(rendering::ui_view);
      blur.execute();
      rendering::renderFullscreenTextureTint(blur.outputTexture(), rendering::final_view, 0.5F);
      bgfx::blit(rendering::readback_view, readback, 0, 0, output);
      auto current = bgfx::frame();
      std::array<std::uint8_t, width * height * 4> pixels{};
      const auto ready = bgfx::readTexture(readback, pixels.data());
      current = bgfx::frame(); // Retire the submitted frame before reading its stats.
      const auto *stats = bgfx::getStats();
      const auto viewPosition = [&](bgfx::ViewId view) {
        for (unsigned i = 0; i < stats->numViews; ++i)
          if (stats->viewStats[i].view == view) return i;
        require(false, "every active composition view executes");
        return 0U;
      };
      require(viewPosition(rendering::bga_view) < viewPosition(rendering::bga_layer_view) &&
                  viewPosition(rendering::bga_layer_view) < viewPosition(rendering::clear_view),
              "offscreen BGA must execute before acquiring the output drawable");
      if (frame == 1)
        require(viewPosition(rendering::bga_layer_view) < viewPosition(blur.blurViewH()) &&
                    viewPosition(blur.blurViewH()) < viewPosition(blur.blurViewV()) &&
                    viewPosition(blur.blurViewV()) < viewPosition(rendering::clear_view),
                "both blur passes must finish before the output clear");
      require(viewPosition(rendering::clear_view) < viewPosition(rendering::final_view) &&
                  viewPosition(rendering::final_view) < viewPosition(rendering::main_view) &&
                  viewPosition(rendering::main_view) < viewPosition(rendering::ui_view),
              "output clear must preserve final, gameplay and UI composition order");
      const auto draws = stats->numDraw;
      // bgfx counts the four touch() submissions as well as real draws.
      require(draws == (frame == 1 ? 8U : 6U),
              "zero/negative blur omits exactly the two blur submissions");
      for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
      require(current >= ready, "blur probe completes readback");
      for (const auto point : {std::array{4, 3}, std::array{27, 12}}) {
        const auto offset = (point[1] * width + point[0]) * 4;
        if (frame != 1) {
          for (int channel = 0; channel < 3; ++channel) {
            require(std::abs(int(pixels[offset + channel]) - source[offset + channel] / 2) <= 2,
                    "blur bypass preserves orientation and half brightness");
          }
        }
        require(pixels[offset + 3] == 255, "fullscreen blur output remains opaque");
      }
      const auto edge = (3 * width + 15) * 4;
      require(frame == 1 ? pixels[edge + 1] > 10 : pixels[edge + 1] <= 26,
              "positive blur softens the boundary and disabling it removes stale blur");
      require(settledBgfxTextureCount() == baseTextures + (frame == 1 ? 3 : 1),
              "disabled blur retains only its scene texture, including after resize");
    }
    for (const auto view : {rendering::bga_view, rendering::bga_layer_view,
                            rendering::clear_view, rendering::main_view, rendering::ui_view})
      bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
    bgfx::setViewOrder(0);
    bgfx::setDebug(BGFX_DEBUG_NONE);
  }
  require(settledBgfxTextureCount() == baseTextures, "blur destruction releases all targets");
  bgfx::setViewFrameBuffer(rendering::final_view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer);
  bgfx::destroy(output);
  bgfx::destroy(readback);
  bgfx::destroy(input);
  bgfx::frame();
}

void testFullscreenBlurClampsScreenEdgesOnMetal() {
  constexpr int width = 64, height = 64;
  const int oldWidth = rendering::window_width, oldHeight = rendering::window_height;
  rendering::window_width = width;
  rendering::window_height = height;
  const auto restore = makeScopeExit([&] {
    rendering::window_width = oldWidth;
    rendering::window_height = oldHeight;
  });
  const auto output = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(width, height, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  require(bgfx::isValid(output) && bgfx::isValid(readback) && bgfx::isValid(framebuffer),
          "blur edge probe creates Metal readback targets");
  {
    // Independent color channels expose horizontal and vertical wrapping in
    // one readback. Each strip is wider than the eight-texel blur radius.
    std::array<std::uint8_t, width * height * 4> source{};
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto offset = (y * width + x) * 4;
        source[offset] = x < 16 ? 255 : 0;
        source[offset + 1] = y < 16 ? 255 : 0;
        source[offset + 3] = 255;
      }
    }
    ImageData data{.texture = bgfx::createTexture2D(width, height, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_POINT,
        bgfx::copy(source.data(), source.size())),
        .width = width, .height = height, .channels = 4};
    require(bgfx::isValid(data.texture), "blur edge probe creates its input image");
    auto image = AdoptImageTextureToSharedOwner(data);
    Stopwatch stopwatch;
    auto control = std::make_shared<JukeboxBackendControl>();
    Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
    jukebox.setBgaDisplayMode(AppSettings::BgaDisplayMode::Stretch);
    rendering::BlurPass blur(1, 1.0F);
    blur.init(width, height);
    blur.setBlurStrength(2);
    blur.setCompositeEnabled(false);
    blur.setInputViews({rendering::bga_view, rendering::bga_layer_view});
    rendering::applyViewOrder(blur.blurViewH(), blur.blurViewV(), blur.finalView());
    float ortho[16];
    bx::mtxOrtho(ortho, 0.0F, float(width), float(height), 0.0F, 0.0F,
                 100.0F, 0.0F, bgfx::getCaps()->homogeneousDepth);
    for (const auto view : {rendering::bga_view, rendering::bga_layer_view, blur.finalView()}) {
      bgfx::setViewTransform(view, nullptr, ortho);
      bgfx::setViewRect(view, 0, 0, width, height);
      bgfx::setViewClear(view, view == rendering::bga_layer_view ? BGFX_CLEAR_NONE : BGFX_CLEAR_COLOR, 0);
    }
    bgfx::setViewFrameBuffer(blur.finalView(), framebuffer);
    require(blur.prepareOutput(), "blur edge probe prepares all targets");
    const auto frame = jukebox.prepareImageFrame(image);
    bgfx::touch(rendering::bga_view);
    bgfx::touch(rendering::bga_layer_view);
    require(jukebox.submitFullscreenComplete(frame), "blur edge probe composes its image");
    require(blur.executeComplete() && blur.drawOutput(1.0F), "blur edge probe submits both blur passes and output");
    bgfx::blit(rendering::readback_view, readback, 0, 0, output);
    std::array<std::uint8_t, width * height * 4> pixels{};
    const auto ready = bgfx::readTexture(readback, pixels.data());
    auto current = bgfx::frame();
    for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
    require(current >= ready, "blur edge probe completes readback");
    // Screen edges retain their own colors, never the opposite edge's. At
    // the interior strip boundaries, four taps contribute 0.41 * 255 = 105.
    for (const auto sample : {std::array{0, 0, 255, 255}, std::array{63, 0, 0, 255},
                              std::array{0, 63, 255, 0}, std::array{63, 63, 0, 0},
                              std::array{32, 32, 0, 0}, std::array{16, 32, 105, 0},
                              std::array{32, 16, 0, 105}}) {
      const auto offset = (sample[1] * width + sample[0]) * 4;
      require(std::abs(int(pixels[offset]) - sample[2]) <= 1,
              "horizontal blur clamps screen edges and still softens the interior boundary");
      require(std::abs(int(pixels[offset + 1]) - sample[3]) <= 1,
              "vertical blur clamps screen edges and still softens the interior boundary");
      require(pixels[offset + 2] == 0 && pixels[offset + 3] == 255,
              "blur edge output preserves blue and opaque alpha");
    }
    for (const auto view : {rendering::bga_view, rendering::bga_layer_view, blur.finalView()})
      bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
  }
  bgfx::setViewOrder(0);
  bgfx::destroy(framebuffer); bgfx::destroy(output); bgfx::destroy(readback);
  bgfx::frame();
}

void testEmbeddedImageResetsSkinSamplingOnMetal() {
  const auto fixtureDirectory = std::filesystem::temp_directory_path() /
      ("asobmashow-bga-sampling-" + std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(fixtureDirectory);
  const auto cleanup = makeScopeExit([&] {
    std::error_code ignored;
    std::filesystem::remove_all(fixtureDirectory, ignored);
  });
  // Uncompressed top-origin BGRA TGA: white opaque / red transparent.
  const std::array<unsigned char, 34> tga{
      0,0,2,0,0,0,0,0,0,0,0,0,2,0,2,0,32,40,
      255,255,255,255, 0,0,255,0, 255,255,255,255, 0,0,255,0};
  {
    std::ofstream file(fixtureDirectory / "sampling.tga", std::ios::binary);
    file.write(reinterpret_cast<const char *>(tga.data()), tga.size());
  }
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  bms_parser::Chart chart;
  chart.Meta.Folder = fixtureDirectory;
  chart.ReferencedBmpTable.emplace(1, "sampling.tga");
  appendBgaTimeline(chart, 0, 1, -1);
  std::atomic_bool cancelled = false;
  jukebox.loadVisuals(chart, cancelled);

  const auto output = bgfx::createTexture2D(8, 4, false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(8, 4, false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  require(bgfx::isValid(output) && bgfx::isValid(readback) && bgfx::isValid(framebuffer),
          "BGA sampling probe creates real Metal readback resources");
  float ortho[16];
  bx::mtxOrtho(ortho, 0.0F, 8.0F, 4.0F, 0.0F, 0.0F, 100.0F, 0.0F,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
  bgfx::setViewRect(rendering::ui_view, 0, 0, 8, 4);
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x080a0effU);
  auto target = makeLoadedImageTarget(GameplayBgaRole::Base, 1);
  target.destination = {{{.x = 0, .y = 4}, {.x = 8, .y = 4},
                         {.x = 8, .y = 0}, {.x = 0, .y = 0}}};
  const auto sampling = rendering::UniformCache::getInstance().getVec4("u_skinSampling");
  const auto sampler = rendering::UniformCache::getInstance().getSampler("s_texColor");
  const auto program = rendering::ShaderManager::getInstance().getProgram(
      "vs_skin_quad.bin", "fs_skin_quad.bin");
  const std::uint32_t white = 0xffffffffU;
  const auto primeTexture = bgfx::createTexture2D(1, 1, false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, bgfx::copy(&white, sizeof(white)));
  struct PrimeVertex { float x, y, z, u, v; std::uint32_t rgba; };
  bgfx::VertexLayout primeLayout;
  primeLayout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
  const auto render = [&](float precedingSkinSampling, std::uint64_t sequence) {
    const auto frame = jukebox.prepareVisualFrameAt(sequence, 0, {});
    require(frame.base.has_value() && jukebox.preflight(frame, std::span(&target, 1)).ready,
            "BGA sampling probe prepares the actual loaded image");
    jukebox.commitPrepared(frame);
    const std::array<float, 4> prior{precedingSkinSampling, 0, 0, 0};
    bgfx::touch(rendering::ui_view);
    bgfx::TransientVertexBuffer primeBuffer;
    bgfx::allocTransientVertexBuffer(&primeBuffer, 3, primeLayout);
    const std::array<PrimeVertex, 3> primeVertices{{
        {-3, -3, 0, 0, 0, white}, {-2, -3, 0, 1, 0, white}, {-2, -2, 0, 1, 1, white}}};
    std::memcpy(primeBuffer.data, primeVertices.data(), sizeof(primeVertices));
    bgfx::setVertexBuffer(0, &primeBuffer);
    bgfx::setTexture(0, sampler, primeTexture, BGFX_SAMPLER_POINT);
    bgfx::setUniform(sampling, prior.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(rendering::ui_view, program);
    jukebox.submitPrepared(frame, target);
    bgfx::blit(rendering::readback_view, readback, 0, 0, output);
    auto current = bgfx::frame();
    std::vector<std::uint8_t> pixels(8 * 4 * 4);
    const auto ready = bgfx::readTexture(readback, pixels.data());
    require(ready != std::numeric_limits<std::uint32_t>::max(),
            "BGA sampling probe schedules readback");
    for (unsigned guard = 0; current < ready && guard < 16; ++guard) current = bgfx::frame();
    require(current >= ready, "BGA sampling probe completes readback");
    jukebox.finalizePrepared(frame);
    return pixels;
  };
  const auto ordinary = render(0, 901);
  const auto afterFilteredSkin = render(1, 902);
  require(ordinary[0] > 100, "BGA sampling probe actually renders the image");
  require(ordinary == afterFilteredSkin,
          "embedded BGA hardware filtering ignores the preceding skin sampling uniform");
  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer);
  bgfx::destroy(readback);
  bgfx::destroy(output);
  bgfx::destroy(primeTexture);
  bgfx::frame();
}

void testPreviewBgaUsesNormalSubmissionAndLiveSettings() {
  Stopwatch stopwatch;
  auto control = std::make_shared<JukeboxBackendControl>();
  Jukebox jukebox(&stopwatch, std::make_unique<JukeboxTestBackendFactory>(control));
  AppSettings settings;
  settings.bgaEnabled = true;
  settings.bgaBrightnessPercent = 35;
  settings.bgaDisplayMode = AppSettings::BgaDisplayMode::Fit;
  PreparedGameplayBgaFrame pinned;
  {
    settings_scene::PreviewBga preview(jukebox, settings);
    auto frame = preview.prepareVisualFrameAt(1, 1'000'000, {});
    require(frame.composition == GameplayBgaComposition::BaseThenLayer && frame.base &&
                frame.base->sourceWidth == settings_scene::kPreviewBgaWidth &&
                frame.base->sourceHeight == settings_scene::kPreviewBgaHeight &&
                !frame.layer && !frame.miss && !jukebox.hasActiveVisuals(),
            "preview BGA pins its own image without activating or replacing chart media");
    require(std::abs(jukebox.embeddedBgaBrightnessMultiplier() - 0.35F) < 0.00001F,
            "preview BGA reads live brightness for the normal embedded skin renderer");
    auto target = makeLoadedImageTarget(GameplayBgaRole::Base, 1);
    const auto preflight = preview.preflight(frame, std::span(&target, 1));
    require(preflight.ready && !preflight.requirements.empty(),
            "preview BGA reserves the ordinary image renderer's transient geometry");
    preview.commitPrepared(frame);
    preview.submitPrepared(frame, target);
    preview.finalizePrepared(frame);
    require(jukebox.gameplayBgaSubmissionStats().embeddedSubmissions == 1 &&
                jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0,
            "preview skin composition submits and releases the image through Jukebox");
    settings.bgaEnabled = false;
    frame = preview.prepareVisualFrameAt(2, 1'000'000, {});
    require(frame.composition == GameplayBgaComposition::Blank && !frame.base,
            "turning BGA off blanks the preview immediately even at paused chart time");
    preview.submitFullscreen(frame);
    settings.bgaEnabled = true;
    settings.bgaBrightnessPercent = 80;
    settings.bgaDisplayMode = AppSettings::BgaDisplayMode::Fill;
    pinned = preview.prepareVisualFrameAt(3, 1'000'000, {});
    require(pinned.base && std::abs(jukebox.embeddedBgaBrightnessMultiplier() - 0.8F) < 0.00001F,
            "reenabling BGA applies edited settings without moving chart time");
  }
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 1,
          "the prepared frame retains its image when the preview provider is destroyed");
  jukebox.submitFullscreen(pinned);
  require(jukebox.gameplayBgaSubmissionStats().pinnedFrames == 0 &&
              jukebox.gameplayBgaSubmissionStats().fullscreenSubmissions == 2 &&
              !jukebox.hasActiveVisuals(),
          "normal fullscreen BGA submission releases the preview image and leaves media state intact");
  const auto ordinary = jukebox.prepareVisualFrameAt(4, 1'000'000, {});
  require(ordinary.composition == GameplayBgaComposition::Blank,
          "ordinary playback does not inherit the preview image");
  jukebox.finalizePrepared(ordinary);
  if (const char *path = std::getenv("ASOBMASHOW_PREVIEW_BGA_PPM")) {
    const auto rgba = settings_scene::previewBgaPixels();
    std::ofstream image(path, std::ios::binary);
    image << "P6\n" << settings_scene::kPreviewBgaWidth << " "
          << settings_scene::kPreviewBgaHeight << "\n255\n";
    for (std::size_t i = 0; i < rgba.size(); i += 4)
      image.write(reinterpret_cast<const char *>(rgba.data() + i), 3);
  }
}

} // namespace

int main(int argc, char **argv) {
  const bool metalSampling = argc == 2 && std::string_view(argv[1]) == "--metal-sampling";
  bgfx::Init init;
  init.type = metalSampling ? bgfx::RendererType::Metal : bgfx::RendererType::Noop;
  init.fallback = false;
  init.resolution.width = metalSampling ? 0 : 64;
  init.resolution.height = metalSampling ? 0 : 64;
  require(bgfx::init(init), "headless bgfx initializes for Jukebox BGA tests");
  rendering::PosTexCoord0Vertex::init();
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  if (metalSampling) {
    testDirectFullscreenBgaOnMetal();
    testFullscreenBgaReuseOnMetal();
    testFullscreenBlurBypassOnMetal();
    testFullscreenBlurClampsScreenEdgesOnMetal();
    testNativeVideoPlaybackSelectsSurfaceRendering();
    testEmbeddedImageResetsSkinSamplingOnMetal();
  } else {
  testGameplayBgaCompositeStateDefaultsToBuiltInFullscreen();
  testBgaDrawTargetRoleIsIndependentOfViewId();
  testPinnedRoleAndMediaSelectExactBgaMaterials();
  testLayerMaterialMakesOnlyExactBlackTransparent();
  testEmbeddedBrightnessMultipliesAuthoredRgbAndPreservesAlpha();
  testEmbeddedYuvUsesOneBrightnessAdjustedTintOnEveryVertex();
  testBgaSubmitterPreflightsExactMultipleTargets();
  testBgaSubmitterPreparedFrameIsAnImmutableValue();
  testBgaSubmitterHasVirtualDestruction();
  testJukeboxPreflightPerformsNoShaderLookupOrFilesystemIo();
  testJukeboxPreparesOneValueFrameAndNeverUpdatesOnSubmission();
  testPreviewBgaUsesNormalSubmissionAndLiveSettings();
  testJukeboxBgaTargetStretchAndTrimmedUvs();
  testBgaStretchPrecedesNonuniformViewportProjection();
  testAuthoredProjectionPreservesBgaUvOrientation();
  testLaterBgaValidationFailureLeavesTransientCapacityUntouched();
  testDuplicateTargetsConsumeDistinctPreparedEntriesExactlyOnce();
  testAuthoredProjectionParticipatesInPreparedTargetIdentity();
  testBmsonConditionalMissSequencesReachRenderedFrames();
  testOnlyScheduledLayerImageIdsReceiveCompanionTextures();
  testCancelledLayerScanDoesNotLeakRoleIntoReusedBaseId();
  testPreparedImageLeaseSurvivesMediaResetUntilFullscreenFallback();
  testFullscreenBgaIdentityCompletionAndLeases();
  testImageTextureOwnershipTransferIsSingleAndExceptionSafe();
  testPreparedFrameRetainsExplicitSurfaceRoles();
  testNoneJudgeDoesNotTriggerMissState();
  testComboZeroUsesBgaClockAndRepeatedZeroRetriggers();
  testZeroStartAndFrameBoundariesAreDeterministic();
  testMissCompositionSuppressesBaseAndLayerWithoutFallback();
  testRealVideoAdapterHonorsBoundedGeneratedFixture();
  }
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
