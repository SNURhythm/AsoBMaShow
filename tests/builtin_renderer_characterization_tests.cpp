#include "rendering/RenderPlan.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/play/BMSRenderer.h"
#include "scene/play/GameplayGeometry.h"
#include "scene/play/PlayfieldChartVisualModel.h"
#include "scene/play/PlayfieldProjection.h"
#include "scene/play/PlayfieldVisualState.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <lodepng.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION
#error "The built-in characterization target requires the recorder seam"
#endif

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = 1280;
int render_height = 720;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 2.0F / 3.0F;
float ui_scale_y = 2.0F / 3.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = 1280;
int ui_view_height = 720;
Camera *main_camera = nullptr;
Camera game_camera{main_view};

void updateUIScale(int renderW, int renderH) {
  render_width = renderW;
  render_height = renderH;
  window_width = design_width;
  ui_scale_x = static_cast<float>(renderW) / static_cast<float>(window_width);
  ui_scale_y = ui_scale_x;
  window_height = static_cast<int>(renderH / ui_scale_y);
  ui_view_width = renderW;
  ui_view_height = renderH;
  ui_offset_x = 0;
  ui_offset_y = 0;
}
} // namespace rendering

namespace {

using Json = nlohmann::ordered_json;
namespace characterization = bms_renderer_characterization;

constexpr unsigned kDrawableWidth = 1280;
constexpr unsigned kDrawableHeight = 720;
constexpr long long kRenderMicros = 1'625'000;
constexpr long long kGameplayMicros = 1'750'000;
constexpr long long kBgaMicros = 1'700'000;
constexpr long long kPastInvisibleRenderMicros = 2'125'000;
constexpr std::uint64_t kCapturedEquivalenceFrameSerial = 3;
constexpr long long kInvisibleProbePlayedTime = 1'998'765;
constexpr int kBeforeCoverPercent = 0;
constexpr int kAfterCoverPercent = 24;
constexpr int kDraggedCoverPercent = 40;
constexpr std::string_view kBeatorajaCommit =
    "c2ed5db1a46145ed10790c3872f717e95b59db9d";

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

double canonical(double value) {
  if (!std::isfinite(value)) {
    return value;
  }
  return std::round(value * 1'000'000.0) / 1'000'000.0;
}

std::string judgementName(Judgement judgement) {
  switch (judgement) {
  case PGreat:
    return "pgreat";
  case Great:
    return "great";
  case Good:
    return "good";
  case Bad:
    return "bad";
  case Poor:
    return "poor";
  case Kpoor:
    return "kpoor";
  case None:
  default:
    return "none";
  }
}

std::string submissionKindName(characterization::SubmissionKind kind) {
  using Kind = characterization::SubmissionKind;
  switch (kind) {
  case Kind::Background:
    return "background";
  case Kind::JudgeLine:
    return "judgeLine";
  case Kind::MeasureLine:
    return "measureLine";
  case Kind::NormalNote:
    return "normal";
  case Kind::InvisiblePrimitive:
    return "invisible";
  case Kind::Mine:
    return "mine";
  case Kind::LongBody:
    return "longBody";
  case Kind::LongTail:
    return "longTail";
  case Kind::LongHead:
    return "longHead";
  case Kind::ReplayGhostPrimitive:
    return "replayGhost";
  case Kind::ReplayMissPrimitive:
    return "replayMiss";
  case Kind::LaneBeamPass:
    return "laneBeamPass";
  case Kind::LaneCoverPass:
    return "laneCoverPass";
  case Kind::StartIndicatorPass:
    return "startIndicatorPass";
  case Kind::JudgementIndicatorPass:
    return "judgementIndicatorPass";
  case Kind::GaugePass:
    return "gaugePass";
  case Kind::HudPass:
    return "hudPass";
  case Kind::TouchPass:
    return "touchPass";
  }
  return "unknown";
}

std::string longBodyStateName(characterization::LongBodyState state) {
  using State = characterization::LongBodyState;
  switch (state) {
  case State::Off:
    return "off";
  case State::On:
    return "on";
  case State::Damage:
    return "damage";
  case State::None:
  default:
    return "none";
  }
}

std::string surfaceName(characterization::Surface surface) {
  return surface == characterization::Surface::Ui ? "ui" : "main";
}

std::string noteKindName(ChartVisualNoteKind kind) {
  switch (kind) {
  case ChartVisualNoteKind::Normal:
    return "normal";
  case ChartVisualNoteKind::Invisible:
    return "invisible";
  case ChartVisualNoteKind::Mine:
    return "mine";
  case ChartVisualNoteKind::LongHead:
    return "longHead";
  case ChartVisualNoteKind::LongBody:
    return "longBody";
  case ChartVisualNoteKind::LongTail:
    return "longTail";
  }
  return "unknown";
}

std::string longModeName(ChartLongNoteMode mode) {
  switch (mode) {
  case ChartLongNoteMode::CN:
    return "CN";
  case ChartLongNoteMode::HCN:
    return "HCN";
  case ChartLongNoteMode::LN:
  default:
    return "LN";
  }
}

struct SyntheticChartFixture {
  std::unique_ptr<bms_parser::Chart> chart =
      std::make_unique<bms_parser::Chart>();
  bms_parser::Note *probeNote = nullptr;
  bms_parser::Note *invisibleProbeNote = nullptr;

  SyntheticChartFixture() {
    chart->Meta.SHA256 = std::string(64, '7');
    chart->Meta.Title = "Synthetic Built-in 7K";
    chart->Meta.SubTitle = "Pre-refactor Characterization";
    chart->Meta.Artist = "AsoBMaShow";
    chart->Meta.Genre = "REDISTRIBUTABLE TEST";
    chart->Meta.Bpm = 120.0;
    chart->Meta.MinBpm = 120.0;
    chart->Meta.MaxBpm = 180.0;
    chart->Meta.MostPrevalentBpm = 120.0;
    chart->Meta.KeyMode = 7;
    chart->Meta.IsDP = false;
    chart->Meta.Rank = 3;
    chart->Meta.LnMode = 0;
    chart->Meta.Total = 100.0;
    chart->Meta.HasTotal = true;
    chart->Meta.TotalNotes = 12;
    chart->Meta.TotalLongNotes = 3;
    chart->Meta.TotalScratchNotes = 1;
    chart->Meta.TotalLandmineNotes = 1;
    chart->Meta.PlayLength = 4'500'000;
    chart->Meta.TotalLength = 4'500'000;

    auto *measure = new bms_parser::Measure();
    measure->Timing = 0;
    measure->Scale = 4.0;
    measure->Pos = 0.0;
    chart->Measures.push_back(measure);

    const auto addTimeline = [measure](long long micros, double beat,
                                       double bpm = 120.0,
                                       double scroll = 1.0,
                                       bool section = false) {
      auto *timeline = new bms_parser::TimeLine(16, false);
      timeline->Timing = micros;
      timeline->BeatPosition = beat;
      timeline->Bpm = bpm;
      timeline->Scroll = scroll;
      timeline->IsFirstInMeasure = section;
      measure->TimeLines.push_back(timeline);
      return timeline;
    };

    auto *section0 = addTimeline(0, 0.0, 120.0, 1.0, true);
    auto *normal0 = addTimeline(350'000, 0.7);
    auto *normal2 = addTimeline(650'000, 1.3);
    auto *lnHead = addTimeline(1'100'000, 2.2);
    auto *cnHead = addTimeline(1'350'000, 2.7);
    auto *scratch = addTimeline(1'550'000, 3.1);
    auto *hcnHead = addTimeline(1'700'000, 3.4);
    auto *invisible = addTimeline(2'000'000, 4.0);
    auto *mine = addTimeline(2'250'000, 4.5, 120.0, 1.0, true);
    auto *bpmChange = addTimeline(2'800'000, 5.6, 180.0);
    bpmChange->BpmChange = true;
    auto *stop = addTimeline(3'050'000, 6.1, 180.0);
    stop->StopLength = 28.8; // 200 ms at 180 BPM.
    auto *scrollChange = addTimeline(3'300'000, 6.6, 180.0, 0.5);
    scrollChange->ScrollChange = true;
    auto *bgaOnly = addTimeline(3'450'000, 6.9, 180.0, 0.5);
    bgaOnly->BgaBase = 1;
    auto *lnTail = addTimeline(3'600'000, 7.2, 180.0, 0.5);
    auto *cnTail = addTimeline(3'850'000, 7.7, 180.0, 0.5);
    auto *hcnTail = addTimeline(4'100'000, 8.2, 180.0, 0.5);
    auto *section1 = addTimeline(4'500'000, 9.0, 180.0, 0.5, true);

    (void)section0;
    normal0->SetNote(0, new bms_parser::Note(bms_parser::Parser::NoWav));
    normal2->SetNote(2, new bms_parser::Note(bms_parser::Parser::NoWav));
    scratch->SetNote(7, new bms_parser::Note(bms_parser::Parser::NoWav));
    invisibleProbeNote = new bms_parser::Note(bms_parser::Parser::NoWav);
    invisible->SetInvisibleNote(1, invisibleProbeNote);
    mine->SetLandmineNote(5, new bms_parser::LandmineNote(12.0F));
    probeNote = new bms_parser::Note(bms_parser::Parser::NoWav);
    mine->SetNote(0, probeNote);
    bpmChange->SetNote(1, new bms_parser::Note(bms_parser::Parser::NoWav));
    section1->SetNote(6, new bms_parser::Note(bms_parser::Parser::NoWav));

    addLongNote(lnHead, lnTail, 3, bms_parser::LongNoteType::LongNote,
                false);
    addLongNote(cnHead, cnTail, 4, bms_parser::LongNoteType::ChargeNote,
                true);
    addLongNote(hcnHead, hcnTail, 6,
                bms_parser::LongNoteType::HellChargeNote, false);
  }

  static void addLongNote(bms_parser::TimeLine *headTimeline,
                          bms_parser::TimeLine *tailTimeline, int lane,
                          bms_parser::LongNoteType type, bool holding) {
    auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
    auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
    head->Tail = tail;
    tail->Head = head;
    head->IsHolding = holding;
    tail->IsHolding = holding;
    headTimeline->SetNote(lane, head);
    tailTimeline->SetNote(lane, tail);
  }
};

class Recorder final : public characterization::Recorder {
public:
  std::vector<characterization::FrameSnapshot> frames;
  std::vector<characterization::TimelineProjection> projections;
  std::vector<characterization::Submission> submissions;

  void beginFrame(const characterization::FrameSnapshot &frame) override {
    frames.push_back(frame);
  }

  void project(const characterization::TimelineProjection &projection) override {
    projections.push_back(projection);
  }

  void submit(const characterization::Submission &submission) override {
    submissions.push_back(submission);
  }
};

class SubmissionPhaseThrowingRecorder final
    : public characterization::Recorder {
public:
  bool reachedLaneBeamPass = false;

  void beginFrame(const characterization::FrameSnapshot &) override {}
  void project(const characterization::TimelineProjection &) override {}
  void submit(const characterization::Submission &submission) override {
    if (submission.kind == characterization::SubmissionKind::LaneBeamPass) {
      reachedLaneBeamPass = true;
      throw std::runtime_error("submission-phase characterization failure");
    }
  }
};

struct RenderTarget {
  bgfx::TextureHandle output = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
};

RenderTarget createRenderTarget() {
  RenderTarget target;
  target.output = bgfx::createTexture2D(
      kDrawableWidth, kDrawableHeight, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_RT);
  target.readback = bgfx::createTexture2D(
      kDrawableWidth, kDrawableHeight, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  if (bgfx::isValid(target.output)) {
    target.framebuffer = bgfx::createFrameBuffer(1, &target.output, false);
  }
  expect(bgfx::isValid(target.output) && bgfx::isValid(target.readback) &&
             bgfx::isValid(target.framebuffer),
         "headless Metal render/readback resources are created");
  return target;
}

void configureGeometryAndViews(bgfx::FrameBufferHandle framebuffer) {
  rendering::updateUIScale(kDrawableWidth, kDrawableHeight);
  rendering::widthScale = 1.0F;
  rendering::heightScale = 1.0F;

  float ortho[16];
  bx::mtxOrtho(ortho, 0.0F, static_cast<float>(rendering::window_width),
               static_cast<float>(rendering::window_height), 0.0F, 0.0F,
               100.0F, 0.0F, bgfx::getCaps()->homogeneousDepth);

  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, framebuffer);
    bgfx::setViewRect(view, 0, 0, kDrawableWidth, kDrawableHeight);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(rendering::readback_view, 0, 0, kDrawableWidth,
                    kDrawableHeight);
  bgfx::setViewTransform(rendering::clear_view, nullptr, ortho);
  bgfx::setViewTransform(rendering::final_view, nullptr, ortho);
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::clear_view, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(rendering::main_view, bgfx::ViewMode::DepthAscending);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(rendering::readback_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::clear_view,
                     BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x070b12ffU, 1.0F,
                     0);

  constexpr float cameraDepth = 2.1F;
  constexpr float laneLookAtY = AppSettings::kDefaultLaneLength * 0.25F;
  const float laneAngle = bx::toRad(AppSettings::kDefaultLaneAngleDegrees);
  const bx::Vec3 at = {gameplay_geometry::kPlayAreaCenterX, laneLookAtY, 0.0F};
  const bx::Vec3 eye = {gameplay_geometry::kPlayAreaCenterX,
                        laneLookAtY - std::tan(laneAngle) * cameraDepth,
                        -cameraDepth};
  rendering::game_camera.edit()
      .setPosition(eye)
      .setLookAt(at)
      .setAspectRatio(static_cast<float>(rendering::window_width) /
                      static_cast<float>(rendering::window_height))
      .setNearClip(rendering::near_clip)
      .setFarClip(rendering::far_clip)
      .setViewRect(0, 0, kDrawableWidth, kDrawableHeight)
      .commit();
  rendering::game_camera.render(true);
  rendering::main_camera = &rendering::game_camera;
  rendering::applyViewOrder(rendering::blur_view_h, rendering::blur_view_v,
                            rendering::final_view);
}

PlayfieldPresentationConfig presentationConfig(int coverPercent) {
  return {
      .visibleTimeGreenNumber = 1500,
      .visibleTimeUseMilliseconds = true,
      .visibleTimeBpmStrategy = AppSettings::VisibleTimeBpmStrategy::Chart,
      .playAreaWidth = AppSettings::kDefaultPlayAreaWidth,
      .laneBeamsEnabled = true,
      .laneCoverHispeedFactor =
          1.0F - static_cast<float>(coverPercent) / 100.0F,
      .laneBeamLengthPercent = 82,
      .noteStartPositionPercent = coverPercent,
      .laneBeamClockUsesRenderTime = true,
      .showInvisibleNotes = true,
      .judgementIndicatorEnabled = true,
      .judgementIndicatorY = 0.28F,
      .judgementIndicatorWidthScale = 1.1F,
      .judgementIndicatorHudMode = false,
      .judgementIndicatorRangeMilliseconds = 180,
      .judgementTextY = 0.34F,
      .judgementCounterEnabled = true,
      .judgementCounterPosition =
          AppSettings::JudgementCounterPosition::Right,
      .fastSlowCriteria =
          AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow,
      .millisecondsCriteria =
          AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow,
      .gaugeBarPosition = AppSettings::GaugeBarPosition::Right,
      .touchVisualizationEnabled = true,
      .replayGhostRenderingEnabled = false,
  };
}

PlayfieldAuthorityUpdate authorityFor(const bms_parser::Chart &chart,
                                      int coverPercent) {
  PlayfieldAuthorityUpdate authority;
  authority.currentBpm = 120.0;
  authority.judgementCounters = {
      {PGreat, 17}, {Great, 5}, {Good, 1}, {Bad, 0}, {Poor, 0}, {Kpoor, 0}};
  authority.comboBreak = 2;
  authority.gaugeType = GaugeType::Hard;
  authority.gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
  authority.currentGauge = 64.5F;
  authority.gaugeRules = compileGameplayGaugeRules(
      kDefaultGameplayRuleset, chart.Meta, GaugeProfile::Standard);
  authority.pacemakerTarget = {.enabled = true,
                               .label = "AA",
                               .finalScore = 180000,
                               .maxScore = 200000,
                               .totalNotes = chart.Meta.TotalNotes};
  authority.pacemakerStatus = {.enabled = true,
                               .label = "AA",
                               .currentScore = 123456,
                               .targetScore = 123444,
                               .finalTargetScore = 180000,
                               .maxScore = 200000,
                               .delta = 12,
                               .playedNotes = 24,
                               .totalNotes = chart.Meta.TotalNotes};
  authority.playOptionLabel = "MIRROR / HARD";
  authority.autoPlayMarkVisible = true;
  authority.startLaneIndicators = {7, 0};
  authority.startLaneIndicatorsVisible = true;
  authority.laneCoverPercent = coverPercent;
  authority.laneCoverEnabled = true;
  authority.resetLaneCoverVisibleTimeReference = true;
  return authority;
}

void applyAuthority(BMSRenderer &renderer,
                    const PlayfieldAuthorityUpdate &authority) {
  renderer.setCurrentBpm(authority.currentBpm);
  renderer.setJudgementCounters(authority.judgementCounters,
                                authority.comboBreak);
  renderer.setGaugeStatus(authority.gaugeType, authority.gaugeAutoShift,
                          authority.currentGauge, authority.gaugeRules);
  renderer.setPacemakerTarget(authority.pacemakerTarget);
  renderer.setPacemakerStatus(authority.pacemakerStatus);
  renderer.setPlayOptionStatus(authority.playOptionLabel);
  renderer.setAutoPlayMarkVisible(authority.autoPlayMarkVisible);
  renderer.setStartLaneIndicators(authority.startLaneIndicators);
  renderer.setStartLaneIndicatorsVisible(
      authority.startLaneIndicatorsVisible);
  renderer.applyLaneCoverState(
      authority.laneCoverPercent,
      authority.resetLaneCoverVisibleTimeReference);
}

struct ScenarioResult {
  struct ParserNoteState {
    bool isDead = false;
    bool isPlayed = false;
    long long playedTime = 0;

    bool operator==(const ParserNoteState &) const = default;
  };

  int coverPercent = 0;
  Recorder recorder;
  PlayfieldVisualState state;
  PlayfieldProjectionResult projection;
  ParserNoteState invisibleBeforeRender;
  ParserNoteState invisibleAfterRender;
  Json chart;
  std::array<std::pair<float, float>, 4> touchBounds{};
  bool hasTouchBounds = false;
  std::optional<float> handleGrabOffset;
  int draggedCoverPercent = 0;
  float dragRenderX = 0.0F;
  float dragRenderY = 0.0F;
  std::vector<std::uint8_t> rgba;
};

enum class ScenarioRenderPath {
  Legacy,
  Captured,
};

std::vector<bms_parser::Note *>
chartVisualNoteSources(const bms_parser::Chart &chart) {
  std::vector<bms_parser::Note *> result;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      const auto append = [&result](const auto &notes) {
        for (auto *note : notes) {
          if (note != nullptr) {
            result.push_back(note);
          }
        }
      };
      append(timeline->Notes);
      append(timeline->InvisibleNotes);
      append(timeline->LandmineNotes);
    }
  }
  return result;
}

std::vector<NotePresentationState>
captureNoteStates(const bms_parser::Chart &chart,
                  const PlayfieldChartVisualModel &model,
                  long long visualTimeMicros) {
  const auto sources = chartVisualNoteSources(chart);
  expect(sources.size() == model.notes.size(),
         "the immutable snapshot covers every parser-backed visual note");

  std::vector<NotePresentationState> result;
  const std::size_t noteCount = std::min(sources.size(), model.notes.size());
  result.reserve(noteCount);
  for (std::size_t index = 0; index < noteCount; ++index) {
    const auto *source = sources[index];
    const auto &note = model.notes[index];
    NotePresentationState state{
        .id = note.id,
        .judged = source->IsPlayed,
        .dead = source->IsDead,
        .playedTimeMicros = source->IsPlayed ? source->PlayedTime
                                             : kPlayfieldTimestampOff,
    };
    if (const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(source);
        longNote != nullptr) {
      const auto *head = longNote->IsTail() && longNote->Head != nullptr
                             ? longNote->Head
                             : longNote;
      const bool headReachedJudge =
          head->IsPlayed || head->IsDead ||
          (head->Timeline != nullptr &&
           head->Timeline->Timing <= visualTimeMicros);
      state.longReactive = false;
      state.longActive = head->IsHolding;
      state.longDamaged =
          note.longNoteMode == ChartLongNoteMode::HCN &&
          headReachedJudge && !state.longActive;
    }
    result.push_back(state);
  }
  return result;
}

ScenarioResult::ParserNoteState
parserNoteState(const bms_parser::Note &note) {
  return {
      .isDead = note.IsDead,
      .isPlayed = note.IsPlayed,
      .playedTime = note.PlayedTime,
  };
}

Json chartJson(const PlayfieldChartVisualModel &model) {
  Json timelines = Json::array();
  for (const auto &timeline : model.timelines) {
    timelines.push_back({
        {"id", timeline.id},
        {"timeMicros", timeline.timeMicros},
        {"beat", canonical(timeline.beat)},
        {"bpm", canonical(timeline.bpm)},
        {"stopMicros", timeline.stopMicros},
        {"scroll", canonical(timeline.scrollRate)},
        {"section", timeline.sectionLine},
        {"bgaOnly", timeline.bgaOnly},
    });
  }

  Json notes = Json::array();
  for (const auto &note : model.notes) {
    notes.push_back({
        {"id", note.id},
        {"timelineId", note.timelineId},
        {"lane", note.lane},
        {"kind", noteKindName(note.kind)},
        {"longMode", longModeName(note.longNoteMode)},
        {"pairId", note.pairId},
    });
  }

  return {
      {"sha256", model.chartSha256},
      {"keyMode", 7},
      {"laneOrder", model.laneOrder},
      {"baseBpm", 120.0},
      {"timelines", std::move(timelines)},
      {"notes", std::move(notes)},
  };
}

std::vector<std::uint8_t> readPixels(const RenderTarget &target) {
  bgfx::blit(rendering::readback_view, target.readback, 0, 0, target.output);
  std::uint32_t currentFrame = bgfx::frame();
  std::vector<std::uint8_t> bgra(
      static_cast<std::size_t>(kDrawableWidth) * kDrawableHeight * 4U);
  const std::uint32_t expectedFrame =
      bgfx::readTexture(target.readback, bgra.data());
  expect(expectedFrame != std::numeric_limits<std::uint32_t>::max(),
         "headless Metal pixel readback is scheduled");
  for (unsigned guard = 0; currentFrame < expectedFrame && guard < 16;
       ++guard) {
    currentFrame = bgfx::frame();
  }
  expect(currentFrame >= expectedFrame,
         "headless Metal pixel readback completes");
  for (std::size_t offset = 0; offset < bgra.size(); offset += 4U) {
    std::swap(bgra[offset], bgra[offset + 2U]);
  }
  return bgra;
}

ScenarioResult renderScenario(
    const RenderTarget &target, int coverPercent, bool capturePixels,
    ScenarioRenderPath renderPath = ScenarioRenderPath::Legacy,
    long long visualTimeMicros = kRenderMicros,
    std::uint64_t frameSerial = 0,
    bool seedPastInvisibleProbe = false,
    bool primeRendererTraversal = false) {
  configureGeometryAndViews(target.framebuffer);
  bgfx::touch(rendering::clear_view);

  SyntheticChartFixture fixture;
  if (seedPastInvisibleProbe) {
    fixture.invisibleProbeNote->IsDead = false;
    fixture.invisibleProbeNote->IsPlayed = true;
    fixture.invisibleProbeNote->PlayedTime = kInvisibleProbePlayedTime;
  }
  const PlayfieldChartVisualModel model =
      buildPlayfieldChartVisualModel(*fixture.chart, 0);
  PlayfieldVisualStateStore store(model);
  ScenarioResult result;
  result.coverPercent = coverPercent;
  result.chart = chartJson(model);

  const auto configuration = presentationConfig(coverPercent);
  const auto authority = authorityFor(*fixture.chart, coverPercent);
  store.setConfiguration(configuration);
  store.applyAuthorityUpdate(authority);
  store.setSceneStartMicros(100'000);
  store.setPlayStartMicros(250'000);

  Judge judge(fixture.chart->Meta.Rank);
  {
    BMSRenderer renderer(fixture.chart.get(), judge.timingWindows,
                         configuration.visibleTimeGreenNumber, true);
    renderer.setCharacterizationRecorder(&result.recorder);
    const PlayfieldJudgeEventClock judgeClock{
        .songTimeMicros = kGameplayMicros,
        .visualTimeMicros = kRenderMicros,
        .bgaTimeMicros = kBgaMicros,
    };
    if (renderPath == ScenarioRenderPath::Legacy) {
      renderer.configure(configuration);
      applyAuthority(renderer, authority);

      PlayfieldPresentationEventFanout fanout(store, renderer);
      fanout.onLanePressed(4, JudgeResult(PGreat, -250), 1'575'000);
      fanout.onLaneReleased(4, 1'600'000);
      fanout.onJudge(JudgeResult(Great, 1'500), 24, 123456, judgeClock, true);
      renderer.setLiveTouchPoint(42, ReplayTouchAction::Down, 0.33F, 0.72F,
                                 1'600'000);
    } else {
      store.onLanePressed(4, JudgeResult(PGreat, -250), 1'575'000);
      store.onLaneReleased(4, 1'600'000);
      store.onJudge(JudgeResult(Great, 1'500), 24, 123456, judgeClock, true);
    }
    store.setLiveTouchPoint(42, ReplayTouchAction::Down, 0.33F, 0.72F,
                            1'600'000);
    if (primeRendererTraversal) {
      const PlayfieldFrameClock warmupClock{
          .serial = 1,
          .visualTimeMicros = kRenderMicros,
          .gameplayTimeMicros = kGameplayMicros,
          .replayTouchTimeMicros = kRenderMicros,
          .bgaTimeMicros = kBgaMicros,
      };
      RenderContext warmupContext;
      if (renderPath == ScenarioRenderPath::Captured) {
        store.setNoteStates(
            captureNoteStates(*fixture.chart, model, kRenderMicros));
        const PlayfieldVisualState warmupState = store.capture(warmupClock);
        renderer.configure(warmupState.configuration);
        PlayfieldProjection projector;
        const PlayfieldProjectionResult warmupProjection = projector.project(
            model, warmupState,
            {.includeInvisibleNotes =
                 warmupState.configuration.showInvisibleNotes,
             .latePoorTimingMicros = renderer.projectionLatePoorTimingMicros(),
             .builtInTraversal = renderer.builtInProjectionTraversal()});
        renderer.render(warmupContext, warmupState, warmupProjection);
      } else {
        renderer.render(warmupContext, warmupClock.visualTimeMicros,
                        warmupClock.replayTouchTimeMicros);
      }
      bgfx::frame();
      result.recorder = {};
      bgfx::touch(rendering::clear_view);
    }

    store.setNoteStates(captureNoteStates(*fixture.chart, model,
                                          visualTimeMicros));

    const PlayfieldFrameClock frameClock{
        .serial = frameSerial != 0
                      ? frameSerial
                      : (coverPercent == kBeforeCoverPercent ? 1U : 2U),
        .visualTimeMicros = visualTimeMicros,
        .gameplayTimeMicros = kGameplayMicros,
        .replayTouchTimeMicros = visualTimeMicros,
        .bgaTimeMicros = kBgaMicros,
    };
    const PlayfieldVisualState capturedState = store.capture(frameClock);
    result.state = capturedState;
    result.invisibleBeforeRender =
        parserNoteState(*fixture.invisibleProbeNote);

    RenderContext context;
    if (renderPath == ScenarioRenderPath::Captured) {
      // Projection freezes the renderer's own cutoff and incremental forward
      // geometry; GamePlayScene uses this same value-returning API.
      renderer.configure(capturedState.configuration);
      PlayfieldProjection projector;
      const PlayfieldProjectionResult capturedProjection = projector.project(
          model, capturedState,
          {.includeInvisibleNotes =
               capturedState.configuration.showInvisibleNotes,
           .latePoorTimingMicros = renderer.projectionLatePoorTimingMicros(),
           .builtInTraversal = renderer.builtInProjectionTraversal()});
      result.projection = capturedProjection;
      renderer.render(context, capturedState, capturedProjection);
    } else {
      renderer.render(context, frameClock.visualTimeMicros,
                      frameClock.replayTouchTimeMicros);
    }
    result.invisibleAfterRender =
        parserNoteState(*fixture.invisibleProbeNote);

    const auto touchBounds = renderer.gameplayTouchBoundsUi();
    if (touchBounds.has_value()) {
      result.touchBounds = *touchBounds;
      result.hasTouchBounds = true;
    }

    if (!result.recorder.frames.empty()) {
      const auto &frame = result.recorder.frames.back();
      const float handleCenterX = frame.laneCoverHandle.x +
                                  frame.laneCoverHandle.width * 0.5F;
      const float handleCenterY = frame.laneCoverHandle.y +
                                  frame.laneCoverHandle.height * 0.5F;
      const bx::Vec3 handleScreen =
          rendering::game_camera.project({handleCenterX, handleCenterY, 0.0F});
      result.handleGrabOffset =
          renderer.laneCoverHandleGrabOffset(handleScreen.x, handleScreen.y);

      const float laneHeight = frame.upperBound - frame.judgeY;
      const float targetY =
          frame.judgeY + laneHeight * (1.0F - kDraggedCoverPercent / 100.0F);
      const bx::Vec3 targetScreen = rendering::game_camera.project(
          {handleCenterX, targetY, 0.0F});
      result.dragRenderX = targetScreen.x;
      result.dragRenderY = targetScreen.y;
      result.draggedCoverPercent = renderer.dragLaneCoverHandleTo(
          targetScreen.x, targetScreen.y, 0.0F);
    }

    if (capturePixels) {
      result.rgba = readPixels(target);
    } else {
      bgfx::frame();
    }
  }
  bgfx::frame();
  return result;
}

struct PresentationInput {
  PlayfieldVisualState state;
  PlayfieldProjectionResult projection;
};

class PresentationStateFixture {
public:
  SyntheticChartFixture chartFixture;
  PlayfieldChartVisualModel model;
  PlayfieldVisualStateStore store;
  Judge judge;
  Recorder recorder;
  BMSRenderer renderer;

  PresentationStateFixture()
      : model(buildPlayfieldChartVisualModel(*chartFixture.chart, 0)),
        store(model), judge(chartFixture.chart->Meta.Rank),
        renderer(chartFixture.chart.get(), judge.timingWindows,
                 presentationConfig(kAfterCoverPercent).visibleTimeGreenNumber,
                 true) {
    const auto configuration = presentationConfig(kAfterCoverPercent);
    store.setConfiguration(configuration);
    store.applyAuthorityUpdate(
        authorityFor(*chartFixture.chart, kAfterCoverPercent));
    store.setSceneStartMicros(100'000);
    store.setPlayStartMicros(250'000);
    renderer.setCharacterizationRecorder(&recorder);
  }

  PresentationInput input(std::uint64_t frameSerial) {
    store.setNoteStates(captureNoteStates(*chartFixture.chart, model,
                                          kPastInvisibleRenderMicros));
    const PlayfieldVisualState state = store.capture({
        .serial = frameSerial,
        .visualTimeMicros = kPastInvisibleRenderMicros,
        .gameplayTimeMicros = kGameplayMicros,
        .replayTouchTimeMicros = kPastInvisibleRenderMicros,
        .bgaTimeMicros = kBgaMicros,
    });
    PlayfieldProjection projector;
    PlayfieldProjectionResult projection = projector.project(
        model, state,
        {.includeInvisibleNotes = state.configuration.showInvisibleNotes,
         .latePoorTimingMicros = renderer.projectionLatePoorTimingMicros(),
         .builtInTraversal = renderer.projectionTraversal()});
    return {.state = state, .projection = std::move(projection)};
  }
};

void verifyPreparedPresentationIsOneShot(const RenderTarget &target) {
  configureGeometryAndViews(target.framebuffer);
  bgfx::touch(rendering::clear_view);

  PresentationStateFixture fixture;
  const PresentationInput frame = fixture.input(11);
  expect(fixture.renderer.prepareFrame(frame.state, frame.projection) ==
             PresentationFrameOutcome::Ready,
         "a matched nonzero built-in frame prepares successfully");

  RenderContext context;
  const PresentationFrameResult first = fixture.renderer.render(context);
  expect(first.frameSerial == 11 &&
             first.outcome == PresentationFrameOutcome::Ready &&
             first.submittedMode == PresentationMode::BuiltIn &&
             first.bgaCompositeMode ==
                 GameplayBgaCompositeMode::FullscreenBuiltIn &&
             !first.failure.has_value(),
         "the first result render submits the exact prepared built-in frame");
  const std::size_t submittedFrameCount = fixture.recorder.frames.size();

  const PresentationFrameResult second = fixture.renderer.render(context);
  expect(second.frameSerial == 0 &&
             second.outcome == PresentationFrameOutcome::CriticalFailure &&
             second.failure.has_value() &&
             second.failure->diagnostic.code ==
                 "presentation.frame_not_prepared" &&
             fixture.recorder.frames.size() == submittedFrameCount,
         "result rendering consumes its prepared frame and cannot submit it twice");
  const auto failure = fixture.renderer.lastFailure();
  expect(failure.has_value() &&
             failure->diagnostic.code == "presentation.frame_not_prepared",
         "the consumed-frame failure is retained by lastFailure");
  bgfx::frame();
}

void verifyRenderDoesNotRewindPreparedTraversal(const RenderTarget &target) {
  configureGeometryAndViews(target.framebuffer);
  bgfx::touch(rendering::clear_view);

  PresentationStateFixture fixture;
  const PresentationInput initial = fixture.input(61);
  PresentationInput regressive = fixture.input(62);
  regressive.projection.builtInPlan.traversedTimelineOrdinals = {0};
  regressive.projection.builtInPlan.nextStartRetainedOrdinal = 0;

  expect(fixture.renderer.prepareFrame(initial.state, initial.projection) ==
             PresentationFrameOutcome::Ready,
         "the initial serial advances the retained traversal cursor");
  const std::uint32_t advancedCursor =
      fixture.renderer.projectionTraversal().startRetainedOrdinal;
  expect(advancedCursor > 0,
         "the initial prepared frame moves the retained traversal forward");

  expect(fixture.renderer.prepareFrame(regressive.state, regressive.projection) ==
             PresentationFrameOutcome::Ready,
         "a newer serial accepts a projection captured before the advance");

  RenderContext context;
  const PresentationFrameResult result = fixture.renderer.render(context);
  expect(result.frameSerial == 62 &&
             result.outcome == PresentationFrameOutcome::Ready &&
             fixture.renderer.projectionTraversal().startRetainedOrdinal ==
                 advancedCursor,
         "rendering a regressive prepared plan keeps traversal forward and "
         "returns its one-shot ready result");
  bgfx::frame();
}

void verifySerialOrderingAndReset() {
  PresentationStateFixture fixture;
  const PresentationInput newest = fixture.input(31);
  expect(fixture.renderer.prepareFrame(newest.state, newest.projection) ==
             PresentationFrameOutcome::Ready,
         "the first serial in an epoch prepares successfully");
  const std::uint32_t advancedCursor =
      fixture.renderer.projectionTraversal().startRetainedOrdinal;
  expect(advancedCursor > 0,
         "successful prepare advances the retained Beatoraja traversal cursor");

  expect(fixture.renderer.prepareFrame(newest.state, newest.projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "an exact duplicate frame serial is rejected");
  const auto duplicateFailure = fixture.renderer.lastFailure();
  expect(duplicateFailure.has_value() &&
             duplicateFailure->frameSerial == 31 &&
             duplicateFailure->diagnostic.code ==
                 "presentation.frame_duplicate" &&
             fixture.renderer.projectionTraversal().startRetainedOrdinal ==
                 advancedCursor,
         "duplicate rejection identifies the serial and preserves traversal");

  PresentationInput stale = newest;
  stale.state.clock.serial = 30;
  stale.projection.frameSerial = 30;
  stale.projection.builtInPlan.traversedTimelineOrdinals = {0};
  stale.projection.builtInPlan.nextStartRetainedOrdinal = 0;
  expect(fixture.renderer.prepareFrame(stale.state, stale.projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "an out-of-order frame serial is rejected");
  const auto staleFailure = fixture.renderer.lastFailure();
  expect(staleFailure.has_value() && staleFailure->frameSerial == 30 &&
             staleFailure->diagnostic.code == "presentation.frame_stale" &&
             fixture.renderer.projectionTraversal().startRetainedOrdinal ==
                 advancedCursor,
         "stale rejection cannot rewind the retained traversal cursor");

  fixture.renderer.reset();
  expect(fixture.renderer.projectionTraversal().startRetainedOrdinal == 0 &&
             !fixture.renderer.lastFailure().has_value(),
         "reset clears traversal and presentation failure state");
  const PresentationInput reopened = fixture.input(31);
  expect(fixture.renderer.prepareFrame(reopened.state, reopened.projection) ==
             PresentationFrameOutcome::Ready,
         "reset opens a new serial epoch that may reuse the prior serial");
}

void verifyPrepareFailurePrecedesSubmission() {
  PresentationStateFixture fixture;
  PresentationInput mismatch = fixture.input(41);
  mismatch.projection.frameSerial = 42;
  expect(fixture.renderer.prepareFrame(mismatch.state, mismatch.projection) ==
             PresentationFrameOutcome::CriticalFailure,
         "a state/projection serial mismatch fails during prepare");
  expect(fixture.recorder.frames.empty() &&
             fixture.recorder.submissions.empty(),
         "prepare failure occurs before any built-in GPU submission path");

  RenderContext context;
  const PresentationFrameResult result = fixture.renderer.render(context);
  expect(result.frameSerial == 41 &&
             result.outcome == PresentationFrameOutcome::CriticalFailure &&
             result.failure.has_value() &&
             result.failure->frameSerial == result.frameSerial &&
             result.failure->diagnostic.code ==
                 "presentation.frame_mismatch" &&
             fixture.recorder.frames.empty() &&
             fixture.recorder.submissions.empty(),
         "a pre-submit prepare failure returns the exact failed serial "
         "without drawing");

  const PresentationFrameResult consumed = fixture.renderer.render(context);
  expect(consumed.frameSerial == 0 && consumed.failure.has_value() &&
             consumed.failure->diagnostic.code ==
                 "presentation.frame_not_prepared" &&
             fixture.recorder.frames.empty() &&
             fixture.recorder.submissions.empty(),
         "even a failed prepared transaction is consumed exactly once");
}

void verifySubmissionFailurePropagatesAfterConsumption() {
  PresentationStateFixture fixture;
  SubmissionPhaseThrowingRecorder recorder;
  fixture.renderer.setCharacterizationRecorder(&recorder);
  const PresentationInput frame = fixture.input(51);
  expect(fixture.renderer.prepareFrame(frame.state, frame.projection) ==
             PresentationFrameOutcome::Ready,
         "the submission-exception fixture prepares before drawing");

  RenderContext context;
  bool propagated = false;
  std::optional<PresentationFrameResult> fabricatedFailureResult;
  try {
    fabricatedFailureResult = fixture.renderer.render(context);
  } catch (const std::runtime_error &error) {
    propagated =
        std::string_view(error.what()) ==
        "submission-phase characterization failure";
  }
  expect(recorder.reachedLaneBeamPass && propagated &&
             !fabricatedFailureResult.has_value(),
         "a submission-phase exception propagates instead of reporting an "
         "atomic recoverable frame failure");

  const PresentationFrameResult consumed = fixture.renderer.render(context);
  expect(consumed.frameSerial == 0 && consumed.failure.has_value() &&
             consumed.failure->diagnostic.code ==
                 "presentation.frame_not_prepared",
         "a throwing submission path still consumes the prepared transaction");
  bgfx::frame();
}

void verifyFactoryUsesBorrowedNonNullChart() {
  SyntheticChartFixture fixture;
  Judge judge(fixture.chart->Meta.Rank);
  std::unique_ptr<BuiltInPlayfieldPresentation> presentation =
      createBuiltInPlayfieldPresentation({
          .chart = *fixture.chart,
          .timingWindows = judge.timingWindows,
          .visibleTimeGreenNumber =
              presentationConfig(kAfterCoverPercent).visibleTimeGreenNumber,
          .renderHud = false,
          .playbackRate = {},
      });
  expect(presentation != nullptr &&
             presentation->activeMode() == PresentationMode::BuiltIn &&
             presentation->projectionLatePoorTimingMicros() > 0,
         "the factory constructs a concrete built-in presentation from a "
         "non-null chart reference");

  presentation.reset();
  expect(fixture.chart->Meta.Title == "Synthetic Built-in 7K",
         "the caller-owned chart remains alive after the borrowed "
         "presentation is destroyed first");
}

Json configurationJson(const PlayfieldPresentationConfig &config) {
  return {
      {"visibleTimeGreenNumber", config.visibleTimeGreenNumber},
      {"visibleTimeUseMilliseconds", config.visibleTimeUseMilliseconds},
      {"visibleTimeBpmStrategy", static_cast<int>(config.visibleTimeBpmStrategy)},
      {"playAreaWidth", canonical(config.playAreaWidth)},
      {"laneBeamsEnabled", config.laneBeamsEnabled},
      {"laneCoverHispeedFactor", canonical(config.laneCoverHispeedFactor)},
      {"laneBeamLengthPercent", config.laneBeamLengthPercent},
      {"noteStartPositionPercent", config.noteStartPositionPercent},
      {"laneBeamClockUsesRenderTime", config.laneBeamClockUsesRenderTime},
      {"showInvisibleNotes", config.showInvisibleNotes},
      {"judgementIndicatorEnabled", config.judgementIndicatorEnabled},
      {"judgementIndicatorY", canonical(config.judgementIndicatorY)},
      {"judgementIndicatorWidthScale",
       canonical(config.judgementIndicatorWidthScale)},
      {"judgementIndicatorHudMode", config.judgementIndicatorHudMode},
      {"judgementIndicatorRangeMilliseconds",
       config.judgementIndicatorRangeMilliseconds},
      {"judgementTextY", canonical(config.judgementTextY)},
      {"judgementCounterEnabled", config.judgementCounterEnabled},
      {"judgementCounterPosition",
       static_cast<int>(config.judgementCounterPosition)},
      {"fastSlowCriteria", static_cast<int>(config.fastSlowCriteria)},
      {"millisecondsCriteria", static_cast<int>(config.millisecondsCriteria)},
      {"gaugeBarPosition", static_cast<int>(config.gaugeBarPosition)},
      {"touchVisualizationEnabled", config.touchVisualizationEnabled},
      {"replayGhostRenderingEnabled", config.replayGhostRenderingEnabled},
  };
}

Json inputEventsJson() {
  return Json::array({
      {{"sequence", 0},
       {"kind", "lanePress"},
       {"lane", 4},
       {"eventTimeMicros", 1'575'000},
       {"judge", "pgreat"},
       {"diffMicros", -250}},
      {{"sequence", 1},
       {"kind", "laneRelease"},
       {"lane", 4},
       {"eventTimeMicros", 1'600'000}},
      {{"sequence", 2},
       {"kind", "judge"},
       {"judge", "great"},
       {"diffMicros", 1'500},
       {"combo", 24},
       {"score", 123456},
       {"recordTimingSample", true},
       {"clock",
        {{"songTimeMicros", kGameplayMicros},
         {"visualTimeMicros", kRenderMicros},
         {"bgaTimeMicros", kBgaMicros}}}},
      {{"sequence", 3},
       {"kind", "touchDown"},
       {"fingerId", 42},
       {"songTimeMicros", 1'600'000},
       {"normalized", {0.33, 0.72}}},
  });
}

Json hudJson(const PlayfieldVisualState &state) {
  Json counters = Json::object();
  for (const auto judgement : {PGreat, Great, Good, Bad, Poor, Kpoor}) {
    const auto found = state.authority.judgementCounters.find(judgement);
    counters[judgementName(judgement)] =
        found == state.authority.judgementCounters.end() ? 0 : found->second;
  }
  return {
      {"lastJudge", judgementName(state.lastJudge.judgement)},
      {"lastJudgeVisualMicros", state.lastJudgeVisualMicros},
      {"combo", state.combo},
      {"score", state.score},
      {"fastSlowMicros", state.fastSlowMicros},
      {"judgementCounters", std::move(counters)},
      {"comboBreak", state.authority.comboBreak},
      {"gauge",
       {{"type", static_cast<int>(state.authority.gaugeType)},
        {"autoShift", static_cast<int>(state.authority.gaugeAutoShift)},
        {"value", canonical(state.authority.currentGauge)},
        {"maximum",
         canonical(state.authority
                       .gaugeRules
                       .gauges[gaugeTypeIndex(state.authority.gaugeType)]
                       .maximum)}}},
      {"pacemaker",
       {{"enabled", state.authority.pacemakerStatus.enabled},
        {"label", state.authority.pacemakerStatus.label},
        {"currentScore", state.authority.pacemakerStatus.currentScore},
        {"targetScore", state.authority.pacemakerStatus.targetScore},
        {"delta", state.authority.pacemakerStatus.delta},
        {"playedNotes", state.authority.pacemakerStatus.playedNotes}}},
      {"playOptionLabel", state.authority.playOptionLabel},
      {"autoPlayMarkVisible", state.authority.autoPlayMarkVisible},
  };
}

Json touchBoundsJson(const ScenarioResult &scenario) {
  Json uiQuad = Json::array();
  Json normalizedQuad = Json::array();
  if (scenario.hasTouchBounds) {
    for (const auto &[x, y] : scenario.touchBounds) {
      uiQuad.push_back({canonical(x), canonical(y)});
      normalizedQuad.push_back(
          {canonical(x / rendering::window_width),
           canonical(y / rendering::window_height)});
    }
  }
  return {
      {"order", {"bottomLeft", "bottomRight", "topLeft", "topRight"}},
      {"uiQuad", std::move(uiQuad)},
      {"normalizedQuad", std::move(normalizedQuad)},
      {"lanes", {7, 0, 1, 2, 3, 4, 5, 6}},
      {"scratch", {true, false, false, false, false, false, false, false}},
  };
}

Json scenarioJson(const ScenarioResult &scenario) {
  expect(scenario.recorder.frames.size() == 1,
         "each independent characterization scenario records one frame");
  if (scenario.recorder.frames.empty()) {
    return Json::object();
  }
  const auto &frame = scenario.recorder.frames.front();
  Json projections = Json::array();
  for (std::size_t sequence = 0;
       sequence < scenario.recorder.projections.size(); ++sequence) {
    const auto &projection = scenario.recorder.projections[sequence];
    projections.push_back({
        {"sequence", sequence},
        {"timelineOrdinal", projection.timelineOrdinal},
        {"timeMicros", projection.timelineMicros},
        {"y", canonical(projection.y)},
        {"future", projection.future},
        {"finite", projection.finite},
    });
  }

  Json submissions = Json::array();
  for (std::size_t sequence = 0;
       sequence < scenario.recorder.submissions.size(); ++sequence) {
    const auto &submission = scenario.recorder.submissions[sequence];
    Json item = {
        {"sequence", sequence},
        {"kind", submissionKindName(submission.kind)},
        {"view", surfaceName(submission.surface)},
        {"depth", submission.depth},
        {"timelineOrdinal", submission.timelineOrdinal},
        {"pairedTimelineOrdinal", submission.pairedTimelineOrdinal},
        {"timeMicros", submission.timelineMicros},
        {"lane", submission.lane},
        {"primitiveOrdinal", submission.primitiveOrdinal},
        {"longBodyState", longBodyStateName(submission.longBodyState)},
        {"rect",
         {canonical(submission.rect.x), canonical(submission.rect.y),
          canonical(submission.rect.width),
          canonical(submission.rect.height)}},
    };
    submissions.push_back(std::move(item));
  }

  return {
      {"name", scenario.coverPercent == kBeforeCoverPercent ? "cover0"
                                                            : "cover24"},
      {"frameSerial", scenario.state.clock.serial},
      {"clock",
       {{"visualTimeMicros", scenario.state.clock.visualTimeMicros},
        {"gameplayTimeMicros", scenario.state.clock.gameplayTimeMicros},
        {"replayTouchTimeMicros",
         scenario.state.clock.replayTouchTimeMicros},
        {"bgaTimeMicros", scenario.state.clock.bgaTimeMicros}}},
      {"projectionBounds",
       {{"currentScrollPosition", canonical(frame.currentScrollPosition)},
        {"visibleScrollMin", canonical(frame.visibleScrollMinimum)},
        {"visibleScrollMax", canonical(frame.visibleScrollMaximum)},
        {"visibleReferenceBpm", canonical(frame.visibleReferenceBpm)},
        {"hispeed", canonical(frame.hispeed)},
        {"rxhs", canonical(frame.rxhs)},
        {"playAreaLeftX", canonical(frame.playAreaLeftX)},
        {"playAreaWidth", canonical(frame.playAreaWidth)},
        {"noteWidth", canonical(frame.noteRenderWidth)},
        {"noteHeight", canonical(frame.noteRenderHeight)},
        {"lowerY", canonical(frame.lowerBound)},
        {"judgeY", canonical(frame.judgeY)},
        {"upperY", canonical(frame.upperBound)},
        {"noteVisibleUpperY", canonical(frame.noteVisibleUpperBound)}}},
      {"timelineProjection", std::move(projections)},
      {"submissions", std::move(submissions)},
      {"hud", hudJson(scenario.state)},
      {"touchBounds", touchBoundsJson(scenario)},
  };
}

std::optional<float> submittedY(const ScenarioResult &scenario,
                                characterization::SubmissionKind kind,
                                long long timelineMicros, int lane) {
  const auto found = std::ranges::find_if(
      scenario.recorder.submissions,
      [kind, timelineMicros, lane](const auto &submission) {
        return submission.kind == kind &&
               submission.timelineMicros == timelineMicros &&
               submission.lane == lane;
      });
  if (found == scenario.recorder.submissions.end()) {
    return std::nullopt;
  }
  return found->rect.y;
}

Json buildCharacterization(const ScenarioResult &before,
                           const ScenarioResult &after) {
  const auto beforeProbe = submittedY(
      before, characterization::SubmissionKind::NormalNote, 2'250'000, 0);
  const auto afterProbe = submittedY(
      after, characterization::SubmissionKind::NormalNote, 2'250'000, 0);
  expect(beforeProbe.has_value() && afterProbe.has_value(),
         "the lane-cover response retains the same future probe note");

  return {
      {"schemaVersion", 1},
      {"fixtureId", "builtin-7k-16x9-v1"},
      {"provenance",
       {{"kind", "synthetic"},
        {"redistributable", true},
        {"beatorajaCommit", kBeatorajaCommit},
        {"renderer", "legacy BMSRenderer::render(RenderContext&,long long,long long)"},
        {"goldenPng", "builtin_7k_16x9.png"}}},
      {"surface",
       {{"drawableWidth", kDrawableWidth},
        {"drawableHeight", kDrawableHeight},
        {"logicalWidth", rendering::design_width},
        {"logicalHeight", rendering::design_height},
        {"backend", "Metal"},
        {"pixelFormat", "RGBA8"},
        {"channelTolerance", 2}}},
      {"chart", after.chart},
      {"configuration", configurationJson(after.state.configuration)},
      {"events", inputEventsJson()},
      {"frames", Json::array({scenarioJson(before), scenarioJson(after)})},
      {"laneCoverResponse",
       {{"before",
         {{"percent", before.coverPercent},
          {"noteVisibleUpperY",
           canonical(before.recorder.frames.front().noteVisibleUpperBound)},
          {"visibleReferenceBpm",
           canonical(before.recorder.frames.front().visibleReferenceBpm)},
          {"probeNoteY",
           beforeProbe.has_value() ? canonical(*beforeProbe) : 0.0}}},
        {"after",
         {{"percent", after.coverPercent},
          {"noteVisibleUpperY",
           canonical(after.recorder.frames.front().noteVisibleUpperBound)},
          {"visibleReferenceBpm",
           canonical(after.recorder.frames.front().visibleReferenceBpm)},
          {"probeNoteY",
           afterProbe.has_value() ? canonical(*afterProbe) : 0.0}}},
        {"dragProbe",
         {{"requestedPercent", kDraggedCoverPercent},
          {"renderX", canonical(after.dragRenderX)},
          {"renderY", canonical(after.dragRenderY)},
          {"grabOffset",
           after.handleGrabOffset.has_value()
               ? Json(canonical(*after.handleGrabOffset))
               : Json(nullptr)},
          {"returnedPercent", after.draggedCoverPercent}}}}},
  };
}

bool hasSubmission(const ScenarioResult &scenario,
                   characterization::SubmissionKind kind) {
  return std::ranges::any_of(
      scenario.recorder.submissions,
      [kind](const auto &submission) { return submission.kind == kind; });
}

void verifyBehavioralCoverage(const ScenarioResult &before,
                              const ScenarioResult &after) {
  expect(after.chart.at("laneOrder") == Json::array({7, 0, 1, 2, 3, 4, 5, 6}),
         "the fixture freezes the built-in 7-key scratch-first lane order");
  expect(after.state.lastJudge.judgement == Great &&
             after.state.lastJudgeVisualMicros == kRenderMicros &&
             after.state.combo == 24 && after.state.score == 123456 &&
             after.state.fastSlowMicros == 1'500,
         "HUD snapshot preserves the exact judge source/visual timing inputs");
  expect(after.state.lanes.size() == 8 &&
             !after.state.lanes.at(5).pressed &&
             after.state.lanes.at(5).pressMicros == 1'575'000 &&
             after.state.lanes.at(5).releaseMicros == 1'600'000,
         "lane event timestamps survive fanout in the scratch-first layout");
  expect(after.hasTouchBounds,
         "the production camera exposes gameplay touch bounds");
  expect(after.state.touches.size() == 1 &&
             after.state.touches.front().fingerId == 42 &&
             after.state.touches.front().songTimeMicros == 1'600'000,
         "HUD snapshot preserves deterministic touch timing");
  expect(after.draggedCoverPercent == kDraggedCoverPercent,
         "lane-cover drag deprojection returns the requested percent");
  expect(after.handleGrabOffset.has_value(),
         "the captured lane-cover handle center is hittable");

  for (const auto kind : {
           characterization::SubmissionKind::Background,
           characterization::SubmissionKind::JudgeLine,
           characterization::SubmissionKind::MeasureLine,
           characterization::SubmissionKind::NormalNote,
           characterization::SubmissionKind::InvisiblePrimitive,
           characterization::SubmissionKind::Mine,
           characterization::SubmissionKind::LongBody,
           characterization::SubmissionKind::LongTail,
           characterization::SubmissionKind::LongHead,
           characterization::SubmissionKind::LaneBeamPass,
           characterization::SubmissionKind::LaneCoverPass,
           characterization::SubmissionKind::StartIndicatorPass,
           characterization::SubmissionKind::JudgementIndicatorPass,
           characterization::SubmissionKind::GaugePass,
           characterization::SubmissionKind::HudPass,
           characterization::SubmissionKind::TouchPass,
       }) {
    const std::string message =
        "the semantic recorder covers the built-in " +
        submissionKindName(kind) + " pass";
    expect(hasSubmission(after, kind), message);
  }

  expect(before.recorder.frames.front().noteVisibleUpperBound >
             after.recorder.frames.front().noteVisibleUpperBound &&
             before.recorder.frames.front().rxhs >
                 after.recorder.frames.front().rxhs,
         "lane cover changes both the visible boundary and projection scale");
}

void verifyCapturedOverloadEquivalence(const ScenarioResult &legacy,
                                       const ScenarioResult &captured) {
  expect(legacy.state.clock.serial != 0 &&
             legacy.state.clock.serial == captured.state.clock.serial &&
             captured.state.clock.serial == captured.projection.frameSerial,
         "legacy, immutable state, and projection share a nonzero frame serial");
  expect(captured.state.notes.size() == captured.chart.at("notes").size(),
         "the immutable render state includes every modeled note state");
  expect(scenarioJson(legacy) == scenarioJson(captured),
         "captured-state overload matches the legacy characterization trace");

  expect(!legacy.rgba.empty() && legacy.rgba.size() == captured.rgba.size(),
         "legacy and captured-state renders produce comparable RGBA frames");
  if (legacy.rgba.size() == captured.rgba.size()) {
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < legacy.rgba.size(); ++index) {
      if (std::abs(static_cast<int>(legacy.rgba[index]) -
                   static_cast<int>(captured.rgba[index])) > 2) {
        ++mismatches;
      }
    }
    expect(mismatches == 0,
           "captured-state PNG matches legacy within channel tolerance 2");
  }

  expect(!captured.invisibleBeforeRender.isDead &&
             captured.invisibleBeforeRender.isPlayed &&
             captured.invisibleBeforeRender.playedTime ==
                 kInvisibleProbePlayedTime,
         "past invisible probe starts with non-default parser note state");
  expect(captured.invisibleAfterRender == captured.invisibleBeforeRender,
         "captured-state rendering does not mutate past invisible parser "
         "IsDead/IsPlayed/PlayedTime");
}

std::filesystem::path fixturePath(std::string_view filename) {
  return std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests" / "fixtures" /
         "gameplay_presentation" / filename;
}

void verifyOrUpdateJson(const Json &actual) {
  const auto path = fixturePath("builtin_timing_v1.json");
  const std::string serialized = actual.dump(2) + "\n";
  if (std::getenv("ASOBMASHOW_UPDATE_BUILTIN_GOLDEN") != nullptr) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << serialized;
    expect(output.good(), "updated timing characterization is written");
    return;
  }

  std::ifstream input(path, std::ios::binary);
  const std::string expected((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  expect(input.good() || input.eof(),
         "committed timing characterization can be read");
  expect(expected == serialized,
         "legacy renderer timing characterization remains byte-exact");
}

void verifyOrUpdatePng(std::span<const std::uint8_t> actual) {
  expect(actual.size() ==
             static_cast<std::size_t>(kDrawableWidth) * kDrawableHeight * 4U,
         "pixel readback has the exact RGBA byte count");
  if (actual.empty()) {
    return;
  }

  const auto path = fixturePath("builtin_7k_16x9.png");
  if (std::getenv("ASOBMASHOW_UPDATE_BUILTIN_GOLDEN") != nullptr) {
    std::filesystem::create_directories(path.parent_path());
    const unsigned error = lodepng::encode(
        path.string(), std::vector<std::uint8_t>(actual.begin(), actual.end()),
        kDrawableWidth, kDrawableHeight);
    expect(error == 0, "updated built-in renderer PNG encodes");
    return;
  }

  std::vector<std::uint8_t> expected;
  unsigned width = 0;
  unsigned height = 0;
  const unsigned error =
      lodepng::decode(expected, width, height, path.string());
  expect(error == 0, "committed built-in renderer PNG decodes");
  expect(width == kDrawableWidth && height == kDrawableHeight,
         "committed built-in renderer PNG dimensions are exact");
  if (error != 0 || width != kDrawableWidth || height != kDrawableHeight ||
      expected.size() != actual.size()) {
    return;
  }

  std::size_t mismatches = 0;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (std::abs(static_cast<int>(actual[index]) -
                 static_cast<int>(expected[index])) > 2) {
      ++mismatches;
    }
  }
  expect(mismatches == 0,
         "built-in renderer pixels stay within per-channel tolerance 2");
}

void destroyRenderTarget(RenderTarget &target) {
  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
  }
  if (bgfx::isValid(target.framebuffer)) {
    bgfx::destroy(target.framebuffer);
  }
  if (bgfx::isValid(target.readback)) {
    bgfx::destroy(target.readback);
  }
  if (bgfx::isValid(target.output)) {
    bgfx::destroy(target.output);
  }
  target = {};
}

} // namespace

int main() {
  if (SDL_Init(SDL_INIT_TIMER) != 0) {
    std::cerr << "FAIL: SDL timer initialization failed: " << SDL_GetError()
              << '\n';
    return 1;
  }

  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless Metal initialization failed\n";
    SDL_Quit();
    return 1;
  }

  const auto *caps = bgfx::getCaps();
  expect(bgfx::getRendererType() == bgfx::RendererType::Metal,
         "characterization executes on Metal without backend fallback");
  expect(caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
             (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0,
         "Metal supports texture blit and readback");

  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  rendering::PosTexCoord0Vertex::init();
  RenderTarget target = createRenderTarget();

  if (failures == 0) {
    try {
      verifyPreparedPresentationIsOneShot(target);
      verifyRenderDoesNotRewindPreparedTraversal(target);
      verifySerialOrderingAndReset();
      verifyPrepareFailurePrecedesSubmission();
      verifySubmissionFailurePropagatesAfterConsumption();
      verifyFactoryUsesBorrowedNonNullChart();

      const auto before =
          renderScenario(target, kBeforeCoverPercent, false);
      const auto after = renderScenario(target, kAfterCoverPercent, true);
      verifyBehavioralCoverage(before, after);
      verifyOrUpdateJson(buildCharacterization(before, after));
      verifyOrUpdatePng(after.rgba);

      const auto legacyPastInvisible = renderScenario(
          target, kAfterCoverPercent, true, ScenarioRenderPath::Legacy,
          kPastInvisibleRenderMicros, kCapturedEquivalenceFrameSerial, true);
      const auto capturedPastInvisible = renderScenario(
          target, kAfterCoverPercent, true, ScenarioRenderPath::Captured,
          kPastInvisibleRenderMicros, kCapturedEquivalenceFrameSerial, true);
      verifyCapturedOverloadEquivalence(legacyPastInvisible,
                                        capturedPastInvisible);

      const auto legacyCursorAdvanced = renderScenario(
          target, kAfterCoverPercent, true, ScenarioRenderPath::Legacy,
          kPastInvisibleRenderMicros, kCapturedEquivalenceFrameSerial, true,
          true);
      const auto capturedCursorAdvanced = renderScenario(
          target, kAfterCoverPercent, true, ScenarioRenderPath::Captured,
          kPastInvisibleRenderMicros, kCapturedEquivalenceFrameSerial, true,
          true);
      expect(std::ranges::any_of(
                 capturedCursorAdvanced.projection.builtInPlan.longNotes,
                 [](const auto &longNote) {
                   return longNote.headTimeMicros == 1'100'000 &&
                          longNote.tailTimeMicros == 3'600'000;
                 }),
             "cursor-advanced capture retains the LN spanning the renderer "
             "start cursor");
      verifyCapturedOverloadEquivalence(legacyCursorAdvanced,
                                        capturedCursorAdvanced);
    } catch (const std::exception &error) {
      std::cerr << "FAIL: characterization threw: " << error.what() << '\n';
      ++failures;
    }
  }

  destroyRenderTarget(target);
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::frame();
  bgfx::frame();
  bgfx::shutdown();
  SDL_Quit();

  if (failures != 0) {
    return 1;
  }
  std::cout << "Built-in renderer characterization tests passed\n";
  return 0;
}
