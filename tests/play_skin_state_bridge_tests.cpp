#include "skin/beatoraja/PlaySkinStateBridge.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

nlohmann::json readJsonFixture(std::string_view relativePath) {
  std::ifstream input(fs::path{ASOBMASHOW_SOURCE_DIR} / relativePath);
  if (!input) {
    expect(false, "JSON fixture is readable");
    return {};
  }
  return nlohmann::json::parse(input, nullptr, false);
}

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-play-skin-state-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  [[nodiscard]] const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class RuntimeHarness final {
public:
  RuntimeHarness()
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("PlayStateContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry) {
    const auto source = temp_.root() / "source";
    writeText(source / "skin/main.luaskin", R"lua(
local captured_main_state = require("main_state")
return {
  type=0,
  probe_timer=function() return captured_main_state.timer(41) end,
  normalized_writer=function(value)
    assert(value >= 0 and value <= 1)
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  readonly_writer=function(value)
    assert(captured_main_state.event_exec(301))
    assert(captured_main_state.event_exec(302, 1))
    for event_id = 303, 308 do
      assert(captured_main_state.event_exec(event_id, 1, 2))
    end
    return value
  end,
  forbidden_writer=function() captured_main_state.event_exec(74) end,
  unknown_writer=function() captured_main_state.event_exec(999) end,
  excessive_arity_writer=function()
    captured_main_state.event_exec(301, 1, 2, 3)
  end,
  stage_then_fail_writer=function()
    captured_main_state.event_exec(900, 77)
    error("rollback staged event")
  end,
  custom_timer_high=function()
    _G.custom_trace = (_G.custom_trace or "") .. "timer-high,"
    return 222
  end,
  custom_timer_low=function()
    _G.custom_trace = (_G.custom_trace or "") .. "timer-low,"
    return 111
  end,
  custom_event=function()
    captured_main_state.event_exec(900, 10)
    _G.custom_trace = (_G.custom_trace or "") .. "event,"
    return true
  end,
  custom_event_second=function()
    captured_main_state.event_exec(900, 20)
    _G.custom_trace = (_G.custom_trace or "") .. "event-second,"
    return true
  end,
  custom_manual=function(...)
    _G.custom_trace = (_G.custom_trace or "") .. "manual:" .. select("#", ...) .. ","
    return true
  end,
  custom_stage_then_fail=function()
    captured_main_state.event_exec(900, 77)
    error("rollback custom event")
  end,
  custom_stage=function()
    captured_main_state.event_exec(900, 1)
    return true
  end,
  custom_budget=function() while true do end end,
  custom_after_budget=function()
    _G.custom_trace = (_G.custom_trace or "") .. "after-budget,"
    return true
  end,
  custom_trace=function() return _G.custom_trace or "" end,
}
)lua");
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "runtime fixture snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = prepared_->readView(),
         .entry = entry_,
         .storageRoots = roots_,
         .profileId =
             *makeSkinProfileId("44444444-4444-4444-8444-444444444444")});
    expect(fileSystem.fileSystem != nullptr, "runtime filesystem creates");
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    runtime_ = std::move(created.runtime);
    expect(runtime_ != nullptr, "gameplay runtime creates");
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "runtime header executes");
    if (header.value) {
      retainCallbacks(*header.value);
    }
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "runtime configured phase executes");
    if (configured.value) {
      probeTimer_ = configured.value->callbackNamed("probe_timer");
      retainCallbacks(*configured.value);
    }
    expect(probeTimer_.has_value(), "runtime bridge callback is retained");
    configured.value.reset();
    expect(runtime_->enterRenderPhase().ok, "runtime enters render phase");
  }

  [[nodiscard]] bool ready() const noexcept { return runtime_ != nullptr; }
  LuaSkinRuntime &runtime() { return *runtime_; }
  [[nodiscard]] LuaCallbackId probeTimer() const { return *probeTimer_; }
  [[nodiscard]] LuaCallbackId callback(std::string_view name) const {
    const auto found = callbacks_.find(name);
    return found == callbacks_.end() ? LuaCallbackId{} : found->second;
  }

private:
  void retainCallbacks(LuaValueHandle &value) {
    for (const std::string_view name : {
             "normalized_writer", "readonly_writer", "forbidden_writer",
             "unknown_writer", "excessive_arity_writer",
             "stage_then_fail_writer", "custom_timer_high",
             "custom_timer_low", "custom_event", "custom_manual",
             "custom_stage_then_fail", "custom_budget",
             "custom_stage", "custom_after_budget", "custom_event_second",
             "custom_trace"}) {
      if (const auto callback = value.callbackNamed(name)) {
        callbacks_.insert_or_assign(std::string{name}, *callback);
      }
    }
  }

  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> probeTimer_;
  std::map<std::string, LuaCallbackId, std::less<>> callbacks_;
};

PlayfieldVisualState stateAt(std::uint64_t serial) {
  PlayfieldVisualState state;
  state.clock.serial = serial;
  state.clock.visualTimeMicros = 2'500'000;
  state.clock.gameplayTimeMicros = 2'400'000;
  state.authority.currentBpm = 172.5;
  state.authority.laneCoverPercent = 45;
  state.authority.currentGauge = 62.0F;
  state.authority.gaugeType = GaugeType::Normal;
  state.authority.gaugeRules.compiled = true;
  state.authority.gaugeRules.gauges[gaugeTypeIndex(GaugeType::Normal)] = {
      .initial = 20.0F,
      .minimum = 2.0F,
      .maximum = 100.0F,
      .clearBorder = 80.0F};
  state.lanes.resize(1);
  state.lanes[0] = {.pressed = true,
                    .pressMicros = 1'000,
                    .releaseMicros = 2'000,
                    .bombMicros = 3'000};
  state.lastJudge = JudgeResult(PGreat, 0);
  state.lastJudgeVisualMicros = 0;
  state.combo = 123;
  state.score = 456;
  return state;
}

PlayfieldProjectionResult projectionAt(std::uint64_t serial) {
  PlayfieldProjectionResult projection;
  projection.frameSerial = serial;
  projection.notes.push_back({.noteId = 1,
                              .lane = 0,
                              .kind = ChartVisualNoteKind::Mine,
                              .scrollDelta = 12.0,
                              .submissionOrdinal = 1});
  return projection;
}

bool hasDiagnostic(const PlaySkinStateBridge &bridge, std::string_view code) {
  return std::ranges::any_of(bridge.diagnostics(),
                             [code](const SkinDiagnostic &diagnostic) {
                               return diagnostic.code == code;
                             });
}

void testPinnedMutationTableMatchesFrozenFixtureExhaustively() {
  const auto fixture = readJsonFixture(
      "tests/fixtures/beatoraja_skin/event_mutation_table_v1.json");
  expect(!fixture.is_discarded(), "event mutation fixture parses");
  expect(fixture.value("schemaVersion", 0U) ==
             SkinEventMutationTable::schemaVersion,
         "event mutation fixture schema matches code");
  expect(fixture.value("beatorajaCommit", "") ==
             "c2ed5db1a46145ed10790c3872f717e95b59db9d",
         "event mutation fixture pins the reviewed Beatoraja commit");

  const auto table = makePinnedSkinEventMutationTableV1();
  std::vector<int> fixtureIds;
  for (const auto &entry : fixture.at("events")) {
    const int id = entry.at("id").get<int>();
    fixtureIds.push_back(id);
    const auto *rule = table.find(id);
    expect(rule != nullptr, "every frozen event exists in the mutation table");
    if (!rule) {
      continue;
    }
    const auto expectedKind = entry.at("kind").get<std::string>();
    const auto actualKind = [&] {
      switch (rule->kind) {
      case SkinEventMutationKind::SessionPresentation:
        return "SessionPresentation";
      case SkinEventMutationKind::SetOption:
        return "SetOption";
      case SkinEventMutationKind::SetFilePath:
        return "SetFilePath";
      case SkinEventMutationKind::SetOffset:
        return "SetOffset";
      case SkinEventMutationKind::ReadOnly:
        return "ReadOnly";
      case SkinEventMutationKind::Unsupported:
        return "Unsupported";
      }
      return "";
    }();
    expect(expectedKind == actualKind, "frozen event kind matches code");
    expect(entry.value("minimumArguments", 0) == rule->minimumArguments &&
               entry.value("maximumArguments", 0) == rule->maximumArguments,
           "frozen event argument bounds match code");
    expect(entry.value("configurationKey", "") == rule->configurationKey,
           "frozen event configuration key matches code");
  }
  std::ranges::sort(fixtureIds);
  std::vector<int> tableIds;
  tableIds.reserve(table.rules().size());
  for (const auto &rule : table.rules()) {
    tableIds.push_back(rule.builtInEventId);
  }
  expect(fixtureIds ==
             std::vector<int>({74, 301, 302, 303, 304, 305, 306, 307, 308}),
         "frozen fixture enumerates every v1 event exactly once");
  expect(tableIds == fixtureIds,
         "frozen fixture has no missing or extra implementation rules");
  expect(table.find(73) == nullptr && table.find(75) == nullptr &&
             table.find(300) == nullptr && table.find(309) == nullptr,
         "mutation table has no neighboring implicit rules");
}

void testBridgeOwnsSnapshotAndClosesEachFrameExactlyOnce() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.text = {.title = "title",
                .subtitle = "subtitle",
                .artist = "artist",
                .subartist = "subartist",
                .genre = "genre"};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(3, ConfigOffset{.x = 7, .y = 9});
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(71);
  state.playStartMicros = 0;
  auto projection = projectionAt(71);
  bridge.beginFrame(state, projection);
  state.authority.currentBpm = 999.0;
  state.lanes.front().pressMicros = 999'999;
  projection.notes.clear();

  expect(bridge.frameSerial() == 71, "bridge exposes the active serial");
  expect(bridge.integerProperty({160}).supported &&
             bridge.integerProperty({160}).value == 172,
         "bridge owns an immutable frame-state copy");
  expect(bridge.timerProperty({100}) == 1'000,
         "owned state protects lane timers from caller mutation");
  expect(bridge.projectedNotes().size() == 1,
         "bridge owns the adapted projection");
  expect(runtime.runtime().beginFrame(71).ok,
         "renderer-owned runtime frame begins after state binding");
  const auto probe = runtime.runtime().invoke(runtime.probeTimer(), {});
  expect(
      probe.value && std::get<std::int64_t>(*probe.value) == 0,
      "bridge begin binds authoritative state without owning Lua frame time");

  const auto committed = bridge.commitFrame();
  expect(committed.frameSerial == 71 && committed.orderedMutations.empty(),
         "first commit closes and returns the active transaction");
  expect(bridge.frameSerial() == 0 && !bridge.stringProperty({10}).supported &&
             !bridge.offsetProperty(3).supported &&
             bridge.projectedNotes().empty(),
         "committed frames gate every borrowed-looking accessor");
  const auto afterCommit = runtime.runtime().invoke(runtime.probeTimer(), {});
  expect(afterCommit.value &&
             std::get<std::int64_t>(*afterCommit.value) == INT64_MIN,
         "commit unbinds the bridge from the runtime");
  expect(bridge.commitFrame().frameSerial == 0 &&
             hasDiagnostic(bridge, "skin.play_state.frame_already_closed"),
         "double commit cannot replay a transaction");

  bridge.beginFrame(stateAt(71), projectionAt(71));
  expect(
      bridge.frameSerial() == 0 &&
          hasDiagnostic(bridge, "skin.play_state.frame_serial_not_increasing"),
      "a reused serial cannot reopen the runtime frame");
  bridge.beginFrame(stateAt(72), projectionAt(72));
  expect(bridge.frameSerial() == 72, "a strictly increasing serial begins");
  bridge.discardFrame();
  bridge.discardFrame();
  expect(bridge.frameSerial() == 0 && bridge.projectedNotes().empty(),
         "discard is an idempotent frame closure");
}

void testFramePropertiesUseAuthoritativeGaugeAndTimerRules() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.text = {.title = "title",
                .subtitle = "subtitle",
                .artist = "artist",
                .subartist = "subartist",
                .genre = "genre"};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(3, ConfigOffset{.x = 7});
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(81);
  bridge.beginFrame(state, projectionAt(81));

  expect(bridge.booleanProperty({42}).supported &&
             bridge.booleanProperty({42}).value &&
             bridge.booleanProperty({43}).supported &&
             !bridge.booleanProperty({43}).value,
         "normal gauge belongs only to the pinned groove family");
  expect(bridge.booleanProperty({236}).supported &&
             bridge.booleanProperty({236}).value &&
             bridge.booleanProperty({235}).supported &&
             !bridge.booleanProperty({235}).value,
         "gauge decile conditions use the active gauge maximum");
  const auto gauge = bridge.gaugeState();
  expect(gauge.supported && gauge.value == 62.0 && gauge.minimum == 2.0 &&
             gauge.maximum == 100.0 && gauge.border == 80.0 &&
             gauge.gaugeType == gaugeTypeIndex(GaugeType::Normal),
         "gauge view carries the captured compiled definition");
  expect(!bridge.stringProperty({12}).supported,
         "full title is not invented when no audited source exists");

  const auto diagnosticCount = bridge.diagnostics().size();
  expect(bridge.timerProperty({42}) == INT64_MIN &&
             bridge.timerProperty({107}) == INT64_MIN &&
             bridge.timerProperty({INT32_MAX}) == INT64_MIN &&
             bridge.diagnostics().size() == diagnosticCount,
         "recognized nonnegative timers are safely off without diagnostics");
  expect(bridge.timerProperty({-1}) == INT64_MIN &&
             bridge.diagnostics().size() == diagnosticCount + 1,
         "negative model timer IDs remain unsupported");

  for (int id = 20'000; id < 21'000; ++id) {
    (void)bridge.booleanProperty({id});
    (void)bridge.booleanProperty({id});
  }
  expect(bridge.diagnostics().size() <= 128,
         "unsupported-property diagnostics are deduplicated and bounded");
  bridge.discardFrame();
}

void testGameplayModeAndLoadingBooleanProperties() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(201);
  state.authority.loadingState = PlayfieldLoadingState::Loading;
  state.authority.gameplayMode = PlayfieldGameplayMode::Play;
  bridge.beginFrame(state, projectionAt(201));
  expect(bridge.booleanProperty({80}).supported &&
             bridge.booleanProperty({80}).value &&
             bridge.booleanProperty({81}).supported &&
             !bridge.booleanProperty({81}).value &&
             bridge.booleanProperty({84}).supported &&
             !bridge.booleanProperty({84}).value &&
             bridge.booleanProperty({1080}).supported &&
             !bridge.booleanProperty({1080}).value,
         "loading state exactly selects the pinned preload boolean pair");
  expect(!bridge.booleanProperty({32}).supported &&
             !bridge.booleanProperty({33}).supported,
         "autoplay remains closed without a distinct authoritative mode");
  bridge.discardFrame();

  state = stateAt(202);
  state.authority.loadingState = PlayfieldLoadingState::Loaded;
  state.authority.gameplayMode = PlayfieldGameplayMode::Replay;
  bridge.beginFrame(state, projectionAt(202));
  expect(bridge.booleanProperty({80}).supported &&
             !bridge.booleanProperty({80}).value &&
             bridge.booleanProperty({81}).supported &&
             bridge.booleanProperty({81}).value &&
             bridge.booleanProperty({84}).supported &&
             bridge.booleanProperty({84}).value &&
             bridge.booleanProperty({1080}).supported &&
             !bridge.booleanProperty({1080}).value,
         "replay mode and loaded state use the captured gameplay authority");
  bridge.discardFrame();

  state = stateAt(203);
  state.authority.loadingState = PlayfieldLoadingState::Loaded;
  state.authority.gameplayMode = PlayfieldGameplayMode::Practice;
  bridge.beginFrame(state, projectionAt(203));
  expect(bridge.booleanProperty({80}).supported &&
             !bridge.booleanProperty({80}).value &&
             bridge.booleanProperty({81}).supported &&
             bridge.booleanProperty({81}).value &&
             bridge.booleanProperty({84}).supported &&
             !bridge.booleanProperty({84}).value &&
             bridge.booleanProperty({1080}).supported &&
             bridge.booleanProperty({1080}).value,
         "practice mode selects only the pinned practice boolean");
  bridge.discardFrame();

  state = stateAt(204);
  state.authority.loadingState = PlayfieldLoadingState::Unknown;
  state.authority.gameplayMode = PlayfieldGameplayMode::Unknown;
  bridge.beginFrame(state, projectionAt(204));
  expect(bridge.booleanProperty({80}).supported &&
             !bridge.booleanProperty({80}).value &&
             bridge.booleanProperty({81}).supported &&
             !bridge.booleanProperty({81}).value &&
             bridge.booleanProperty({84}).supported &&
             !bridge.booleanProperty({84}).value &&
             bridge.booleanProperty({1080}).supported &&
             !bridge.booleanProperty({1080}).value,
         "unknown lifecycle fails closed instead of being guessed as loaded");
  bridge.discardFrame();
}

void testSelectedScuroMappingsUseOnlyAuthoritativeState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.text = {.title = "title",
                .subtitle = "subtitle",
                .artist = "artist",
                .subartist = "subartist",
                .genre = "genre",
                .auditedStringProperties = {{12, "full title"}}};
  chart.staticMetadata = {.difficulty = 3,
                          .judgeRank = 70,
                          .minimumBpm = 120.9,
                          .maximumBpm = 240.5,
                          .mainBpm = 178.9,
                          .durationMicros = 125'789'000,
                          .playLevel = 12,
                          .normalKeyNotes = 321,
                          .longKeyNotes = 54,
                          .normalScratchNotes = 32,
                          .longScratchNotes = 10,
                          .totalNotes = 417,
                          .hasBga = true,
                          .stageFilePath = "stage.png",
                          .backBmpPath = "back.png"};
  chart.notes = {{.id = 1, .kind = ChartVisualNoteKind::LongHead}};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById = {
      {1, {.x = 1}}, {3, {.x = 3}}, {4, {.x = 4}},
      {30, {.x = 30}}, {32, {.x = 32}}};
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(101);
  state.playStartMicros = 6'001;
  state.lastJudgeVisualMicros = 6'002;
  state.fastSlowMicros = -34;
  state.authority.liftEnabled = false;
  state.authority.liftRatio = 0.3759F;
  state.authority.currentGauge = 62.3F;
  state.authority.judgementCounters = {
      {PGreat, 10}, {Great, 9}, {Good, 8},
      {Bad, 7},    {Kpoor, 6}, {Poor, 5}};
  state.lanes.resize(8);
  for (std::size_t index = 0; index < state.lanes.size(); ++index) {
    state.lanes[index].pressMicros = 1'000 + static_cast<long long>(index);
    state.lanes[index].releaseMicros =
        2'000 + static_cast<long long>(index);
    state.lanes[index].bombMicros = 3'000 + static_cast<long long>(index);
  }
  bridge.beginFrame(state, projectionAt(101));

  expect(!bridge.booleanProperty({32}).supported &&
             !bridge.booleanProperty({33}).supported,
         "autoplay options stay unsupported without a play-mode source");
  expect(bridge.booleanProperty({43}).supported &&
             !bridge.booleanProperty({43}).value,
         "gauge-hard option reads the authoritative gauge type");
  expect(bridge.booleanProperty({241}).supported &&
             bridge.booleanProperty({241}).value,
         "judge-perfect option reads the most recent judgement");
  expect(bridge.integerProperty({160}).supported &&
             bridge.integerProperty({160}).value == 172,
         "now-BPM truncates the authoritative current BPM");
  expect(bridge.booleanProperty({2243}).supported &&
             bridge.booleanProperty({2243}).value &&
             bridge.booleanProperty({2244}).supported &&
             bridge.booleanProperty({2244}).value &&
             bridge.booleanProperty({2245}).supported &&
             bridge.booleanProperty({2245}).value,
         "selected judge-existence options read captured judgement counters");
  expect(bridge.booleanProperty({172}).supported &&
             !bridge.booleanProperty({172}).value &&
             bridge.booleanProperty({173}).supported &&
             bridge.booleanProperty({173}).value,
         "selected long-note options read the immutable chart model");
  for (const auto [id, expected] : std::array{
           std::pair{150, false}, std::pair{151, false},
           std::pair{152, false}, std::pair{153, true},
           std::pair{154, false}, std::pair{155, false},
           std::pair{170, false}, std::pair{171, true},
           std::pair{176, false}, std::pair{177, true},
           std::pair{180, false}, std::pair{181, false},
           std::pair{182, true}, std::pair{183, false},
           std::pair{184, false}, std::pair{190, false},
           std::pair{191, true}, std::pair{194, false},
           std::pair{195, true}}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && value.value == expected,
           "selected static chart option uses its exact immutable source");
  }
  for (const auto [id, expected] : std::array{
           std::pair{14, 450LL}, std::pair{71, 456LL},
           std::pair{101, 456LL}, std::pair{107, 62LL},
           std::pair{110, 10LL}, std::pair{111, 9LL},
           std::pair{112, 8LL}, std::pair{113, 7LL},
           std::pair{114, 5LL}, std::pair{171, 456LL},
           std::pair{314, 375LL},
           std::pair{407, 3LL}, std::pair{427, 18LL},
           std::pair{525, -34LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "selected live score, gauge, judgement, and lift number is exact");
  }
  for (const auto [id, expected] : std::array{
           std::pair{74, 417LL}, std::pair{90, 240LL},
           std::pair{91, 120LL}, std::pair{92, 178LL},
           std::pair{96, 12LL}, std::pair{350, 321LL},
           std::pair{351, 54LL}, std::pair{352, 32LL},
           std::pair{353, 10LL},
           std::pair{1163, 2LL}, std::pair{1164, 5LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "selected static chart number uses its exact immutable source");
  }
  expect(bridge.stringProperty({12}).supported &&
             bridge.stringProperty({12}).value == "full title" &&
             bridge.stringProperty({13}).value == "genre" &&
             bridge.stringProperty({14}).value == "artist" &&
             bridge.stringProperty({15}).value == "subartist",
         "audited full title and chart text use immutable chart metadata");
  for (const int id : {1, 3, 4, 30, 32}) {
    const auto offset = bridge.offsetProperty(id);
    expect(offset.supported && offset.value.x == id,
           "configured selected offsets are returned by their exact IDs");
  }

  for (const auto [id, expected] : std::array{
           std::pair{41, 6'001LL}, std::pair{46, 6'002LL},
           std::pair{50, 3'000LL}, std::pair{51, 3'001LL},
           std::pair{52, 3'002LL}, std::pair{53, 3'003LL},
           std::pair{54, 3'004LL}, std::pair{55, 3'005LL},
           std::pair{56, 3'006LL}, std::pair{57, 3'007LL},
           std::pair{100, 1'000LL}, std::pair{101, 1'001LL},
           std::pair{102, 1'002LL}, std::pair{103, 1'003LL},
           std::pair{104, 1'004LL}, std::pair{105, 1'005LL},
           std::pair{106, 1'006LL}, std::pair{107, 1'007LL},
           std::pair{120, 2'000LL}, std::pair{121, 2'001LL},
           std::pair{122, 2'002LL}, std::pair{123, 2'003LL},
           std::pair{124, 2'004LL}, std::pair{125, 2'005LL},
           std::pair{126, 2'006LL}, std::pair{127, 2'007LL}}) {
    expect(bridge.timerProperty({id}) == expected,
           "selected live timer reads its authoritative presentation clock");
  }
  const auto diagnosticCount = bridge.diagnostics().size();
  for (const int id : {2, 3, 11, 40, 42, 44, 48, 70, 71, 72, 73, 74,
                       75, 76, 77, 140, 143, 172, 173, 351, 352}) {
    expect(bridge.timerProperty({id}) == INT64_MIN,
           "selected timer without an authoritative source is off");
  }
  expect(bridge.diagnostics().size() == diagnosticCount,
         "selected off timers do not report unsupported diagnostics");
  expect(bridge.floatProperty({4}).supported &&
             bridge.floatProperty({4}).value == 0.0,
         "disabled lane cover reports zero despite a retained amount");
  bridge.discardFrame();

  state = stateAt(102);
  state.authority.laneCoverEnabled = true;
  state.authority.laneCoverPercent = 0;
  state.lastJudge = JudgeResult(Great, 20);
  state.fastSlowMicros = 20;
  bridge.beginFrame(state, projectionAt(102));
  expect(bridge.floatProperty({4}).supported &&
             bridge.floatProperty({4}).value == 0.0,
         "enabled zero lane cover remains zero");
  expect(bridge.booleanProperty({1242}).supported &&
             bridge.booleanProperty({1242}).value &&
             bridge.booleanProperty({1243}).supported &&
             !bridge.booleanProperty({1243}).value,
         "positive recent timing selects pinned early option");
  bridge.discardFrame();

  state = stateAt(103);
  state.authority.laneCoverEnabled = true;
  state.authority.liftEnabled = true;
  state.authority.liftRatio = 0.2F;
  state.lastJudge = JudgeResult(Great, -20);
  state.fastSlowMicros = -20;
  bridge.beginFrame(state, projectionAt(103));
  expect(bridge.floatProperty({4}).supported &&
             std::abs(bridge.floatProperty({4}).value - 0.36) < 0.000001,
         "enabled lift scales the lane-cover amount by one minus lift");
  expect(bridge.booleanProperty({1242}).supported &&
             !bridge.booleanProperty({1242}).value &&
             bridge.booleanProperty({1243}).supported &&
             bridge.booleanProperty({1243}).value,
         "negative recent timing selects pinned late option");
  bridge.discardFrame();

  for (const auto [serial, gauge, option] : std::array{
           std::tuple{104U, 5.0F, 230}, std::tuple{105U, 15.0F, 231},
           std::tuple{106U, 25.0F, 232}, std::tuple{107U, 100.0F, 240}}) {
    state = stateAt(serial);
    state.authority.currentGauge = gauge;
    bridge.beginFrame(state, projectionAt(serial));
    expect(bridge.booleanProperty({option}).supported &&
               bridge.booleanProperty({option}).value,
           "selected gauge decile uses the compiled gauge maximum");
    bridge.discardFrame();
  }

  PlayfieldChartVisualModel unauditedChart = chart;
  unauditedChart.text.auditedStringProperties.clear();
  PlaySkinStateBridge unaudited({.chartModel = unauditedChart,
                                 .model = model,
                                 .configuration = configuration,
                                 .runtime = runtime.runtime(),
                                 .mutationTable = mutations});
  unaudited.beginFrame(stateAt(108), projectionAt(108));
  expect(!unaudited.stringProperty({12}).supported,
         "full title remains gated when the chart model lacks an audit value");
  unaudited.discardFrame();
}

void testEmptyCustomObjectsStayZeroCost() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(91), projectionAt(91));
  const auto update = bridge.updateCustomObjects();
  expect(update.status == SkinHostCallStatus::Completed &&
             update.callbacksInvoked == 0 && bridge.diagnostics().empty(),
         "selected SCURO empty custom maps perform no per-frame work");
  (void)bridge.commitFrame();
}

void testCustomTimersPrecedeAutomaticEventsInAuthoredOrder() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.timerProperties = {
      {.id = SkinTimerPropertyId{1}, .source = runtime.callback("custom_timer_high")},
      {.id = SkinTimerPropertyId{2}, .source = runtime.callback("custom_timer_low")},
  };
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1}, .source = SkinBuiltinPropertySelector{.value = 42}},
  };
  model.model.events = {
      {.id = SkinEventBindingId{1}, .source = runtime.callback("custom_event")},
      {.id = SkinEventBindingId{2},
       .source = runtime.callback("custom_event_second")},
  };
  // Deliberately non-sorted IDs: declaration order is the contract.
  model.model.customTimers = {
      {.id = 10'002, .timer = SkinTimerPropertyId{1}},
      {.id = 10'001, .timer = SkinTimerPropertyId{2}},
  };
  model.model.customEvents = {
      {.id = 1'002,
       .action = SkinEventBindingId{1},
       .condition = SkinBooleanPropertyId{1}},
      {.id = 1'001,
       .action = SkinEventBindingId{2},
       .condition = SkinBooleanPropertyId{1}},
  };
  BeatorajaSkinConfiguration configuration;
  auto pinned = makePinnedSkinEventMutationTableV1();
  std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                            pinned.rules().end());
  rules.push_back({.builtInEventId = 900,
                   .kind = SkinEventMutationKind::SessionPresentation,
                   .maximumArguments = 2});
  const SkinEventMutationTable mutations(std::move(rules));
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(111), projectionAt(111));
  expect(runtime.runtime().beginFrame(111).ok,
         "custom-object update uses the already-owned Lua frame");
  const auto update = bridge.updateCustomObjects();
  const auto trace = runtime.runtime().invoke(runtime.callback("custom_trace"), {});
  expect(update.status == SkinHostCallStatus::Completed &&
             update.callbacksInvoked == 4 && trace.value &&
             std::get<std::string>(*trace.value) ==
                 "timer-high,timer-low,event,event-second,",
         "custom timers precede automatic events in authored declaration order");
  expect(hasDiagnostic(bridge, "custom_object_order_authored_divergence"),
         "nonempty custom maps retain the frozen authored-order divergence diagnostic");
  expect(bridge.timerProperty({10'002}) == 222 &&
             bridge.timerProperty({10'001}) == 111,
         "custom timer values are cached for the frame");
  const auto committed = bridge.commitFrame();
  expect(committed.orderedMutations.size() == 2 &&
             std::get<SessionPresentationWrite>(committed.orderedMutations[0])
                     .arguments[0] == 10 &&
             std::get<SessionPresentationWrite>(committed.orderedMutations[1])
                     .arguments[0] == 20,
         "later automatic callbacks retain earlier staged writes in authored order");
}

void testCustomEventsAcceptManualAritiesAndRollbackCriticalFrames() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.events = {
      {.id = SkinEventBindingId{1}, .source = runtime.callback("custom_manual")},
      {.id = SkinEventBindingId{2},
       .source = runtime.callback("custom_stage_then_fail")},
  };
  model.model.customEvents = {
      {.id = 1'001, .action = SkinEventBindingId{1}, .condition = std::nullopt},
      {.id = 1'002, .action = SkinEventBindingId{2}, .condition = std::nullopt},
  };
  auto pinned = makePinnedSkinEventMutationTableV1();
  std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                            pinned.rules().end());
  rules.push_back({.builtInEventId = 900,
                   .kind = SkinEventMutationKind::SessionPresentation,
                   .maximumArguments = 2});
  const SkinEventMutationTable mutations(std::move(rules));
  BeatorajaSkinConfiguration configuration;
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(112), projectionAt(112));
  expect(runtime.runtime().beginFrame(112).ok,
         "manual custom events share the owner frame budget");
  expect(bridge.executeEvent(1'001, {}).status == SkinHostCallStatus::Completed &&
             bridge.executeEvent(1'001, std::array{7}).status ==
                 SkinHostCallStatus::Completed &&
             bridge.executeEvent(1'001, std::array{7, 8}).status ==
                 SkinHostCallStatus::Completed,
         "manual custom events accept exactly zero, one, and two integers");
  const auto trace = runtime.runtime().invoke(runtime.callback("custom_trace"), {});
  expect(trace.value && std::get<std::string>(*trace.value) ==
                            "manual:0,manual:1,manual:2,",
         "manual custom callbacks receive the exact supplied arity");
  expect(bridge.executeEvent(900, std::array{9}).status ==
             SkinHostCallStatus::Completed &&
             bridge.executeEvent(1'002, {}).status ==
                 SkinHostCallStatus::CriticalFailure,
         "a critical custom event failure rejects the whole frame");
  expect(bridge.commitFrame().orderedMutations.empty(),
         "critical custom-event failure rolls back every staged write");
}

void testAutomaticCustomEventUsesTheCapturedFrameClockForMinimumInterval() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1}, .source = SkinBuiltinPropertySelector{.value = 42}},
  };
  model.model.events = {
      {.id = SkinEventBindingId{1}, .source = runtime.callback("custom_event")},
  };
  model.model.customEvents = {
      {.id = 1'010,
       .action = SkinEventBindingId{1},
       .condition = SkinBooleanPropertyId{1},
       .minimumIntervalMillis = 100},
  };
  BeatorajaSkinConfiguration configuration;
  auto pinned = makePinnedSkinEventMutationTableV1();
  std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                            pinned.rules().end());
  rules.push_back({.builtInEventId = 900,
                   .kind = SkinEventMutationKind::SessionPresentation,
                   .maximumArguments = 2});
  const SkinEventMutationTable mutations(std::move(rules));
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto runFrame = [&](std::uint64_t serial, std::int64_t visualMicros) {
    auto state = stateAt(serial);
    state.clock.visualTimeMicros = visualMicros;
    bridge.beginFrame(state, projectionAt(serial));
    expect(runtime.runtime().beginFrame(serial).ok,
           "automatic custom event starts each owner frame once");
    const auto update = bridge.updateCustomObjects();
    expect(update.status == SkinHostCallStatus::Completed,
           "automatic custom event completes below its frame budget");
    return bridge.commitFrame().orderedMutations.size();
  };
  auto manualState = stateAt(114);
  manualState.clock.visualTimeMicros = 1'000'000;
  bridge.beginFrame(manualState, projectionAt(114));
  expect(runtime.runtime().beginFrame(114).ok,
         "manual interval probe begins the owner frame once");
  expect(bridge.executeEvent(1'010, {}).status ==
             SkinHostCallStatus::Completed,
         "manual custom event completes below its frame budget");
  (void)bridge.commitFrame();
  const auto suppressedWrites = runFrame(115, 1'099'999);
  const auto thresholdWrites = runFrame(116, 1'100'000);
  bridge.beginFrame(stateAt(117), projectionAt(117));
  expect(runtime.runtime().beginFrame(117).ok,
         "trace probe has a fresh owner frame");
  const auto trace = runtime.runtime().invoke(runtime.callback("custom_trace"), {});
  expect(suppressedWrites == 0 && thresholdWrites == 1 && trace.value &&
             std::get<std::string>(*trace.value) == "event,event,",
         "manual and automatic executions share the captured visual-clock interval");
  bridge.discardFrame();
}

void testCustomObjectBudgetStopsLaterEventsAndRollsBackWrites() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.booleanProperties = {
      {.id = SkinBooleanPropertyId{1}, .source = SkinBuiltinPropertySelector{.value = 42}},
  };
  model.model.events = {
      {.id = SkinEventBindingId{1}, .source = runtime.callback("custom_stage")},
      {.id = SkinEventBindingId{2}, .source = runtime.callback("custom_budget")},
      {.id = SkinEventBindingId{3},
       .source = runtime.callback("custom_after_budget")},
  };
  model.model.customEvents = {
      {.id = 1'003, .action = SkinEventBindingId{1}, .condition = SkinBooleanPropertyId{1}},
      {.id = 1'004, .action = SkinEventBindingId{2}, .condition = SkinBooleanPropertyId{1}},
      {.id = 1'005, .action = SkinEventBindingId{3}, .condition = SkinBooleanPropertyId{1}},
  };
  auto pinned = makePinnedSkinEventMutationTableV1();
  std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                            pinned.rules().end());
  rules.push_back({.builtInEventId = 900,
                   .kind = SkinEventMutationKind::SessionPresentation,
                   .maximumArguments = 2});
  const SkinEventMutationTable mutations(std::move(rules));
  BeatorajaSkinConfiguration configuration;
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(113), projectionAt(113));
  expect(runtime.runtime().beginFrame(113).ok,
         "budgeted custom-object update begins the owner frame once");
  const auto update = bridge.updateCustomObjects();
  expect(update.status == SkinHostCallStatus::BudgetExceeded &&
             update.callbacksInvoked == 2,
         "budget exhaustion stops automatic custom events deterministically");
  expect(bridge.commitFrame().orderedMutations.empty(),
         "budget exhaustion rolls back writes staged by earlier callbacks");
}

void testFloatWritersResolveLocallyAndRollbackCallbackMutations() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.floatWriters = {
      {.id = SkinFloatWriterId{1},
       .source = runtime.callback("normalized_writer")},
      {.id = SkinFloatWriterId{2},
       .source = SkinBuiltinPropertySelector{.value = 4}},
      {.id = SkinFloatWriterId{3},
       .source = runtime.callback("readonly_writer")},
      {.id = SkinFloatWriterId{4},
       .source = runtime.callback("forbidden_writer")},
      {.id = SkinFloatWriterId{5},
       .source = runtime.callback("unknown_writer")},
      {.id = SkinFloatWriterId{6},
       .source = runtime.callback("excessive_arity_writer")},
      {.id = SkinFloatWriterId{7},
       .source = runtime.callback("stage_then_fail_writer")},
  };
  BeatorajaSkinConfiguration configuration;
  auto rules = makePinnedSkinEventMutationTableV1();
  std::vector<SkinEventMutationRule> testRules(rules.rules().begin(),
                                                rules.rules().end());
  testRules.push_back({.builtInEventId = 900,
                       .kind = SkinEventMutationKind::SessionPresentation,
                       .maximumArguments = 2});
  SkinEventMutationTable mutations(std::move(testRules));
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(121), projectionAt(121));
  expect(runtime.runtime().beginFrame(121).ok,
         "writer transaction shares the renderer-owned Lua frame budget");

  for (const double invalid : {
           std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity(),
           -std::numeric_limits<double>::infinity()}) {
    const auto result = bridge.invokeWriter(SkinFloatWriterId{1}, invalid);
    expect(result.status == SkinHostCallStatus::CriticalFailure &&
               result.callbacksInvoked == 0,
           "nonfinite writer input is rejected before Lua invocation");
  }
  expect(bridge.invokeWriter(SkinFloatWriterId{2}, 0.5).status ==
             SkinHostCallStatus::Unsupported &&
             bridge.invokeWriter(SkinFloatWriterId{999}, 0.5).status ==
                 SkinHostCallStatus::Unsupported,
         "built-in and missing writer sources fail closed in the model");

  for (const double value : {-2.0, 2.0, 0.25}) {
    const auto result = bridge.invokeWriter(SkinFloatWriterId{1}, value);
    expect(result.status == SkinHostCallStatus::Completed &&
               result.callbacksInvoked == 1,
           "finite writer values clamp and invoke one explicit callback");
  }
  expect(bridge.invokeWriter(SkinFloatWriterId{3}, 0.5).status ==
             SkinHostCallStatus::Completed,
         "frozen read-only events accept zero through two arguments as no-ops");
  for (const auto writer : {SkinFloatWriterId{4}, SkinFloatWriterId{5},
                            SkinFloatWriterId{6}, SkinFloatWriterId{7}}) {
    expect(bridge.invokeWriter(writer, 0.5).status ==
               SkinHostCallStatus::CriticalFailure,
           "unsupported event, excess arity, and callback failure reject the writer");
  }

  expect(bridge.executeEvent(74, {}).status == SkinHostCallStatus::Unsupported &&
             bridge.executeEvent(999, {}).status ==
                 SkinHostCallStatus::Unsupported,
         "authority-changing and unknown direct events remain unsupported");
  const auto committed = bridge.commitFrame();
  expect(committed.frameSerial == 121 &&
             committed.orderedMutations.size() == 3,
         "failed callbacks roll back only their savepoint before one commit");
  const std::array<int, 3> expectedArguments{0, 100, 25};
  for (std::size_t index = 0;
       index < committed.orderedMutations.size() &&
       index < expectedArguments.size();
       ++index) {
    const auto *presentation =
        std::get_if<SessionPresentationWrite>(&committed.orderedMutations[index]);
    expect(presentation != nullptr && presentation->eventId == 900 &&
               presentation->argumentCount == 1 &&
               presentation->arguments[0] == expectedArguments[index],
           "successful callback mutations preserve clamped authored order");
  }
  expect(bridge.commitFrame().frameSerial == 0,
         "writer transaction cannot be committed twice");
  bridge.discardFrame();
  bridge.discardFrame();
}

} // namespace

int main() {
  testPinnedMutationTableMatchesFrozenFixtureExhaustively();
  testBridgeOwnsSnapshotAndClosesEachFrameExactlyOnce();
  testFramePropertiesUseAuthoritativeGaugeAndTimerRules();
  testGameplayModeAndLoadingBooleanProperties();
  testSelectedScuroMappingsUseOnlyAuthoritativeState();
  testEmptyCustomObjectsStayZeroCost();
  testCustomTimersPrecedeAutomaticEventsInAuthoredOrder();
  testCustomEventsAcceptManualAritiesAndRollbackCriticalFrames();
  testAutomaticCustomEventUsesTheCapturedFrameClockForMinimumInterval();
  testCustomObjectBudgetStopsLaterEventsAndRollsBackWrites();
  testFloatWritersResolveLocallyAndRollbackCallbackMutations();
  return failures == 0 ? 0 : 1;
}
