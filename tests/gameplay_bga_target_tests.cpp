#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "audio/GameplayBgaFrame.h"
#include "audio/GameplayBgaMissStateTracker.h"
#include "audio/Jukebox.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "skin/beatoraja/SkinDestinationEvaluator.cpp"
#include "utils/Stopwatch.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <filesystem>
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
           left->sourceHeight == right->sourceHeight));
}

bool sameFrame(const PreparedGameplayBgaFrame &left,
               const PreparedGameplayBgaFrame &right) {
  return left.sequence == right.sequence && left.composition == right.composition &&
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

void testImageTextureOwnershipTransferIsSingleAndExceptionSafe() {
  ImageData successful{.texture = makeOwnershipTestTexture(),
                       .width = 1,
                       .height = 1,
                       .channels = 4};
  require(bgfx::isValid(successful.texture),
          "ownership test texture is created");
  const auto successfulHandle = successful.texture;
  auto owner = AdoptImageTextureToSharedOwner(successful);
  require(owner && owner->texture.idx == successfulHandle.idx &&
              !bgfx::isValid(successful.texture),
          "successful adoption invalidates the loader guard at the exact "
          "shared-owner transfer");
  owner.reset();

  ImageData controlBlockFailure{.texture = makeOwnershipTestTexture(),
                                .width = 1,
                                .height = 1,
                                .channels = 4};
  require(bgfx::isValid(controlBlockFailure.texture),
          "control-block failure texture is created");
  allocationFailureCountdown = 1;
  bool threw = false;
  try {
    (void)AdoptImageTextureToSharedOwner(controlBlockFailure);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  allocationFailureCountdown = -1;
  require(threw && !bgfx::isValid(controlBlockFailure.texture),
          "a throwing shared control-block allocation leaves only its deleter "
          "owning the transferred texture, never the loader guard");

  ImageData rawAllocationFailure{.texture = makeOwnershipTestTexture(),
                                 .width = 1,
                                 .height = 1,
                                 .channels = 4};
  require(bgfx::isValid(rawAllocationFailure.texture),
          "raw-owner failure texture is created");
  allocationFailureCountdown = 0;
  threw = false;
  try {
    (void)AdoptImageTextureToSharedOwner(rawAllocationFailure);
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  allocationFailureCountdown = -1;
  require(threw && bgfx::isValid(rawAllocationFailure.texture),
          "failure before the transfer leaves the loader guard as the sole "
          "texture owner");
  bgfx::destroy(rawAllocationFailure.texture);
  rawAllocationFailure.texture = BGFX_INVALID_HANDLE;
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
              state.triggerSerial == 1 && !state.isActiveAt(0) &&
              !state.isActiveAt(499'999) &&
              !state.frameIndexAt(0, 4).has_value(),
          "BGA timestamp zero is retained but preserves the pinned invisible "
          "misslayertime sentinel");

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

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for Jukebox BGA tests");
  rendering::PosTexCoord0Vertex::init();
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  testBgaDrawTargetRoleIsIndependentOfViewId();
  testBgaSubmitterPreflightsExactMultipleTargets();
  testBgaSubmitterPreparedFrameIsAnImmutableValue();
  testBgaSubmitterHasVirtualDestruction();
  testJukeboxPreparesOneValueFrameAndNeverUpdatesOnSubmission();
  testJukeboxBgaTargetStretchAndTrimmedUvs();
  testBgaStretchPrecedesNonuniformViewportProjection();
  testAuthoredProjectionPreservesBgaUvOrientation();
  testLaterBgaValidationFailureLeavesTransientCapacityUntouched();
  testDuplicateTargetsConsumeDistinctPreparedEntriesExactlyOnce();
  testAuthoredProjectionParticipatesInPreparedTargetIdentity();
  testPreparedImageLeaseSurvivesMediaResetUntilFullscreenFallback();
  testImageTextureOwnershipTransferIsSingleAndExceptionSafe();
  testPreparedFrameRetainsExplicitSurfaceRoles();
  testNoneJudgeDoesNotTriggerMissState();
  testComboZeroUsesBgaClockAndRepeatedZeroRetriggers();
  testZeroStartAndFrameBoundariesAreDeterministic();
  testMissCompositionSuppressesBaseAndLayerWithoutFallback();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
