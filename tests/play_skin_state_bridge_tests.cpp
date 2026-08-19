#include "skin/beatoraja/PlaySkinStateBridge.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/GameplaySkinEndAnimation.h"
#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
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
  projection.builtInTraversal =
      BuiltInRendererTraversal{.configuredHispeed = 2.0F, .hispeed = 2.0F};
  projection.notes.push_back({.noteId = 1,
                              .lane = 0,
                              .kind = ChartVisualNoteKind::Mine,
                              .scrollDelta = 12.0,
                              .submissionOrdinal = 1});
  return projection;
}

bool hasDiagnostic(const PlaySkinStateBridge &bridge, std::string_view code);

void testDurationBindingsUsePinnedLaneRendererFormula() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.staticMetadata = {.minimumBpm = 150.0,
                          .maximumBpm = 240.0,
                          .mainBpm = 200.0};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(202);
  state.authority.currentBpm = 180.0;
  state.authority.laneCoverPercent = 25;
  state.authority.laneCoverEnabled = true;
  const auto projection = projectionAt(202);
  bridge.beginFrame(state, projection);

  for (const auto [id, expected] : std::array{
           std::pair{312, 500LL}, std::pair{313, 300LL},
           std::pair{1312, 500LL}, std::pair{1313, 300LL},
           std::pair{1314, 667LL}, std::pair{1315, 400LL},
           std::pair{1316, 450LL}, std::pair{1317, 270LL},
           std::pair{1318, 600LL}, std::pair{1319, 360LL},
           std::pair{1320, 600LL}, std::pair{1321, 360LL},
           std::pair{1322, 800LL}, std::pair{1323, 480LL},
           std::pair{1324, 375LL}, std::pair{1325, 225LL},
           std::pair{1326, 500LL}, std::pair{1327, 300LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "duration and lane-cover selector follows IntegerPropertyFactory: " +
               std::to_string(id));
  }
  expect(!hasDiagnostic(bridge, "skin.play_state.unsupported"),
         "pinned duration selectors do not become app-specific unsupported "
         "state errors");
}

void testZeroHispeedDurationBindingsFollowJavaCurrentDuration() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(202);
  state.authority.currentBpm = 120.0;
  state.authority.laneCoverPercent = 100;
  state.authority.laneCoverEnabled = true;
  state.configuration.configuredHispeed = 0.0F;
  auto projection = projectionAt(202);
  projection.builtInTraversal->configuredHispeed = 0.0F;
  bridge.beginFrame(state, projection);

  const auto raw = bridge.integerProperty({10});
  const auto duration = bridge.integerProperty({312});
  const auto green = bridge.integerProperty({313});
  expect(raw.supported && raw.value == 0 && duration.supported &&
             duration.value == 0 && green.supported && green.value == 0,
         "zero Hi-Speed retains Java currentduration rather than a duration "
         "fallback or unsupported property");
}

void testDurationBindingsUseFrameLocalSpeedObjectMultiplier() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(202);
  state.authority.currentBpm = 180.0;
  state.authority.laneCoverPercent = 25;
  state.authority.laneCoverEnabled = true;
  state.authority.currentSpeedMultiplier = 0.5;
  bridge.beginFrame(state, projectionAt(202));

  const auto duration = bridge.integerProperty({312});
  const auto green = bridge.integerProperty({313});
  expect(duration.supported && duration.value == 1'000 && green.supported &&
             green.value == 600,
         "duration selectors divide by the frame-local SPEED multiplier");
}

void testHispeedBindingsUsePinnedIntegerAndFloatProperties() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(203);
  // Pinned from IntegerPropertyFactory.createHispeedProperty and
  // FloatPropertyFactory.FloatType.hispeed: each selector reads the raw
  // PlayConfig/LaneRenderer hispeed, not the skin's note traversal speed.
  state.configuration.configuredHispeed = 1.3125F;
  auto projection = projectionAt(203);
  // LaneRenderer::getHispeed() is the live cover-compensated PlayConfig
  // value, not the preference and not the playback-scaled skin traversal.
  projection.builtInTraversal->configuredHispeed = 1.3125F;
  bridge.beginFrame(state, projection);

  for (const auto [id, expected] : std::array{
           std::pair{10, 131LL}, std::pair{310, 1LL},
           std::pair{311, 31LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "hispeed selector follows IntegerPropertyFactory: " +
               std::to_string(id));
  }
  const auto raw = bridge.floatProperty(
      {310}, SkinFloatPropertyDomain::FloatValue);
  expect(raw.supported && std::abs(raw.value - 1.3125) < 0.0001,
         "float hispeed selector follows FloatPropertyFactory");
  expect(!hasDiagnostic(bridge, "skin.play_state.unsupported"),
         "pinned hispeed selectors do not become app-specific unsupported "
         "state errors");
}

bool hasDiagnostic(const PlaySkinStateBridge &bridge, std::string_view code) {
  return std::ranges::any_of(bridge.diagnostics(),
                             [code](const SkinDiagnostic &diagnostic) {
                               return diagnostic.code == code;
                             });
}

void testIntegerPropertyFactoryDomainNeverRejectsGameplaySkins() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(231);
  state.authority.pacemakerTarget = {.enabled = true, .finalScore = 500};
  state.authority.loadingState = PlayfieldLoadingState::Loaded;
  bridge.beginFrame(state, projectionAt(231));

  for (const auto [id, expected] : std::array{
           std::pair{12, 0LL}, std::pair{100, 456LL},
           std::pair{121, 500LL}, std::pair{165, 100LL},
           std::pair{0, static_cast<long long>(INT32_MIN)},
           std::pair{65'535, static_cast<long long>(INT32_MIN)}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "IntegerPropertyFactory selector is accepted without a live-state "
           "failure: " + std::to_string(id));
  }
  const auto imageIndex = bridge.integerProperty(
      {65'535}, SkinIntegerPropertyDomain::ImageIndex);
  expect(imageIndex.supported && imageIndex.value == 0,
         "ImageIndexProperty cache upper bound selects frame zero instead of "
         "rejecting the skin");
  expect(!hasDiagnostic(bridge, "skin.play_state.unsupported"),
         "full IntegerPropertyFactory input domain avoids app-specific "
         "unsupported-state diagnostics");
  bridge.discardFrame();
}

void testMarkProcessedNoteImageIndexTracksPlayerConfiguration() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(232);
  bridge.beginFrame(state, projectionAt(232));
  const auto disabled = bridge.integerProperty(
      {305}, SkinIntegerPropertyDomain::ImageIndex);
  expect(disabled.supported && disabled.value == 0,
         "ImageIndex 305 defaults to Beatoraja's disabled processed marker");
  bridge.discardFrame();

  state = stateAt(233);
  state.configuration.markProcessedNotes = true;
  bridge.beginFrame(state, projectionAt(233));
  const auto enabled = bridge.integerProperty(
      {305}, SkinIntegerPropertyDomain::ImageIndex);
  expect(enabled.supported && enabled.value == 1,
         "ImageIndex 305 reflects the enabled processed-note marker");
  bridge.discardFrame();
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
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(71);
  state.playStartMicros = 0;
    state.clock.playTimer = {.active = true,
                             .startMicros = 0,
                             .elapsedMillisExact = true};
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
  expect(bridge.projectedNotes().size() == 1 &&
             bridge.projectedNotes().front().kind ==
                 SkinProjectedNoteKind::Mine,
         "bridge preserves Mine note kind when adapting the projection");
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

  // KeyInputProccessor turns the key-on timer off as it starts key-off on a
  // physical release, and does the inverse on a subsequent press. The skin
  // may render both destinations simultaneously, so publishing both stale
  // timestamps makes the key-on visual mask the release animation.
  state = stateAt(72);
  state.lanes.front().pressed = false;
  bridge.beginFrame(state, projectionAt(72));
  expect(bridge.timerProperty({100}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({120}) == 2'000,
         "a released lane exposes only the pinned key-off timer");
  bridge.discardFrame();

  state = stateAt(73);
  state.lanes.front().pressed = true;
  bridge.beginFrame(state, projectionAt(73));
  expect(bridge.timerProperty({100}) == 1'000 &&
             bridge.timerProperty({120}) == kPlayfieldTimestampOff,
         "a pressed lane clears its previous key-off timer");
  bridge.discardFrame();

  bridge.beginFrame(stateAt(71), projectionAt(71));
  expect(
      bridge.frameSerial() == 0 &&
          hasDiagnostic(bridge, "skin.play_state.frame_serial_not_increasing"),
      "a reused serial cannot reopen the runtime frame");
  bridge.beginFrame(stateAt(74), projectionAt(74));
  expect(bridge.frameSerial() == 74, "a strictly increasing serial begins");
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
                              .model = &model,
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
  const auto normalInverse = bridge.booleanProperty({-42});
  const auto constant = bridge.booleanProperty({400});
  const auto constantInverse = bridge.booleanProperty({-400});
  expect(normalInverse.supported && !normalInverse.value &&
             constant.supported && !constant.value &&
             constantInverse.supported && constantInverse.value,
         "negative boolean IDs use Beatoraja BooleanPropertyFactory negation, "
         "including OPTION_CONSTANT");
  const auto autoplayOff = bridge.booleanProperty({32});
  const auto autoplayOn = bridge.booleanProperty({33});
  expect(autoplayOff.supported && autoplayOff.value && autoplayOn.supported &&
             !autoplayOn.value,
         "the full pinned BooleanPropertyFactory domain includes the "
         "autoplay pair with AsoBMaShow's non-autoplay gameplay state");
  for (const int id : {603, 1002, 1177, 2246, 3000, 3015, 3020, 3035}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && !value.value,
           "unimplemented but upstream-supported boolean selectors are "
           "inactive rather than skin-invalidating");
  }
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
                              .model = &model,
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
  expect(bridge.booleanProperty({32}).supported &&
             bridge.booleanProperty({32}).value &&
             bridge.booleanProperty({33}).supported &&
             !bridge.booleanProperty({33}).value,
         "autoplay booleans expose AsoBMaShow's explicit non-autoplay state");
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
             !bridge.booleanProperty({1080}).value,
         "practice gameplay does not fabricate Beatoraja's separate "
         "STATE_PRACTICE menu boolean");
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

void testExistingGameplayStatePropertyWiring() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.keyCount = 14;
  chart.staticMetadata = {.judgeRank = 42, .hasBga = true};
  ValidatedBeatorajaSkinModel model;
  model.model.header.name = "Pinned gameplay skin";
  model.model.header.author = "Skin author";
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(205);
  state.authority.gameplayMode = PlayfieldGameplayMode::Practice;
  state.authority.gaugeType = GaugeType::ExHard;
  state.authority.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.authority.gaugeAutoShiftLowerBound = GaugeType::ExHard;
  state.authority.laneCoverEnabled = true;
  state.authority.liftEnabled = true;
  state.authority.liftRatio = 0.25F;
  state.authority.hiddenEnabled = true;
  state.authority.hiddenRatio = 0.75F;
  state.configuration.hispeedFixMode = AppSettings::HiSpeedFixMode::Max;
  state.configuration.hispeedAutoAdjust = true;
  state.configuration.bpmGuideEnabled = true;
  state.configuration.customJudge = true;
  state.configuration.showJudgeArea = true;
  state.configuration.notesDisplayTimingMilliseconds = -37;
  state.configuration.notesDisplayTimingAutoAdjust = true;
  state.configuration.autoSaveReplay = {1, 2, 3, 10};
  state.configuration.guideSoundEffects = true;
  state.configuration.extraNoteDepth = 23;
  state.configuration.mineMode = 3;
  state.configuration.scrollMode = 1;
  state.configuration.longNoteModifierMode = 4;
  state.configuration.sevenToNinePattern = 6;
  state.configuration.sevenToNineType = 2;
  state.configuration.constantScroll = true;
  practice::SkinMenuState practiceMenu;
  practiceMenu.items[0] = {.available = true,
                           .selected = true,
                           .label = "START TIME",
                           .value = " 0:00.0",
                           .text = "START TIME :  0:00.0"};
  practiceMenu.items[1] = {.available = true,
                           .selected = false,
                           .label = "END TIME",
                           .value = " 1:30.0",
                           .text = "END TIME :  1:30.0"};
  state.authority.practiceMenu = std::move(practiceMenu);
  state.clock.playTimer = {.active = true,
                           .startMicros = 0,
                           .elapsedMillisExact = true,
                           .playtimeMillis = 5'000};
  state.authority.judgementCounters = {
      {PGreat, 99}, {Great, 3}, {Good, 4},
      {Bad, 5},     {Poor, 6},  {Kpoor, 7}};
  state.authority.judgementFastSlowCounters = {
      {PGreat, {.fast = 11, .slow = 12}},
      {Great, {.fast = 1, .slow = 2}},
      {Good, {.fast = 3, .slow = 4}},
      {Bad, {.fast = 5, .slow = 6}},
      {Poor, {.fast = 7, .slow = 8}},
      {Kpoor, {.fast = 9, .slow = 10}}};
  bridge.beginFrame(state, projectionAt(205));

  expect(bridge.stringProperty({1010}).supported &&
             bridge.stringProperty({1010}).value == "0.0.1",
         "version string reads the declared application version");
  expect(bridge.booleanProperty({3000}).supported &&
             !bridge.booleanProperty({3000}).value &&
             bridge.booleanProperty({3001}).supported &&
             !bridge.booleanProperty({3001}).value &&
             bridge.booleanProperty({3002}).supported &&
             !bridge.booleanProperty({3002}).value &&
             bridge.booleanProperty({3020}).supported &&
             !bridge.booleanProperty({3020}).value &&
             bridge.booleanProperty({3021}).supported &&
             !bridge.booleanProperty({3021}).value &&
             bridge.stringProperty({1040}).value == "START TIME :  0:00.0" &&
             bridge.stringProperty({1061}).value == "END TIME" &&
             bridge.stringProperty({1081}).value == " 1:30.0" &&
             bridge.stringProperty({std::string{"practice_item2"}}).value ==
                 "END TIME :  1:30.0",
         "practice text remains readable during play while availability and "
         "selection require the unavailable STATE_PRACTICE UI");

  for (const auto [id, expected] : std::array{
           std::pair{40, false}, std::pair{41, true}, std::pair{82, true},
           std::pair{272, true}, std::pair{273, true},
           std::pair{160, false}, std::pair{161, false},
           std::pair{162, true}, std::pair{163, false},
           std::pair{164, false}, std::pair{1160, false},
           std::pair{1161, false}, std::pair{1046, true}}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && value.value == expected,
           "existing gameplay boolean property uses the pinned source: " +
               std::to_string(id));
  }
  for (const auto [id, expected] : std::array{
           std::pair{40, 4LL}, std::pair{55, 2LL}, std::pair{78, 4LL},
           std::pair{72, 0LL}, std::pair{306, 1LL}, std::pair{330, 1LL},
           std::pair{331, 1LL}, std::pair{332, 1LL}, std::pair{341, 4LL},
           std::pair{342, 1LL}, std::pair{301, 1LL}, std::pair{303, 1LL},
           std::pair{75, 1LL}, std::pair{321, 1LL}, std::pair{322, 2LL},
           std::pair{323, 3LL}, std::pair{324, 10LL}, std::pair{343, 1LL},
           std::pair{350, 23LL}, std::pair{351, 3LL}, std::pair{352, 1LL},
           std::pair{353, 4LL}, std::pair{360, 6LL}, std::pair{361, 2LL},
           std::pair{400, 1LL}}) {
    const auto value = bridge.integerProperty(
        {id}, SkinIntegerPropertyDomain::ImageIndex);
    expect(value.supported && value.value == expected,
           "existing gameplay image index uses the pinned source: " +
               std::to_string(id));
  }
  const auto displayTiming = bridge.integerProperty({12});
  expect(displayTiming.supported && displayTiming.value == -37,
         "notes-display timing uses the captured PlayerConfig value");
  for (const auto [id, expected] :
       std::array{std::pair{314, 250LL}, std::pair{315, 750LL},
                  std::pair{316, 337LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "Lift/HIDDEN numeric property uses the captured PlayConfig state: " +
               std::to_string(id));
  }
  expect(bridge.booleanProperty({400}).supported &&
             bridge.booleanProperty({400}).value,
         "OPTION_CONSTANT shares PlayConfig's captured enabled value");
  const auto progress = bridge.floatProperty({101});
  expect(progress.supported && progress.value == bridge.floatProperty({6}).value,
         "music progress bar is the same pinned source as music progress");
  expect(bridge.integerProperty({400}).supported &&
             bridge.integerProperty({400}).value == 42 &&
             bridge.integerProperty({423}).supported &&
             bridge.integerProperty({423}).value == 25 &&
             bridge.integerProperty({424}).supported &&
             bridge.integerProperty({424}).value == 30 &&
             bridge.integerProperty({426}).supported &&
             bridge.integerProperty({426}).value == 13,
         "judge-rank and aggregate judge counters use captured gameplay state");
  expect(bridge.stringProperty({50}).supported &&
             bridge.stringProperty({50}).value == "Pinned gameplay skin" &&
             bridge.stringProperty({51}).supported &&
             bridge.stringProperty({51}).value == "Skin author",
         "skin name and author use the decoded active skin header");
  bridge.discardFrame();

  state.clock.serial = 206;
  state.configuration.bgaEnabled = false;
  bridge.beginFrame(state, projectionAt(206));
  const auto bgaOff = bridge.booleanProperty({40});
  const auto bgaOn = bridge.booleanProperty({41});
  const auto bgaIndex = bridge.integerProperty(
      {72}, SkinIntegerPropertyDomain::ImageIndex);
  expect(bgaOff.supported && !bgaOff.value && bgaOn.supported && bgaOn.value &&
             bgaIndex.supported && bgaIndex.value == 2,
         "BGA resource selectors and the global BGA mode use their separate "
         "pinned sources");
  bridge.discardFrame();
}

void testPracticeMenuSelectorsAndEventsRequireCapturedMenuState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(206);
  state.authority.gameplayMode = PlayfieldGameplayMode::Practice;
  state.authority.practiceMenuActive = true;
  practice::SkinMenuState menu;
  menu.items[0] = {.available = true, .selected = false};
  menu.items[1] = {.available = true, .selected = true};
  state.authority.practiceMenu = std::move(menu);
  bridge.beginFrame(state, projectionAt(206));

  expect(bridge.booleanProperty({1080}).supported &&
             bridge.booleanProperty({1080}).value &&
             bridge.booleanProperty({3000}).supported &&
             bridge.booleanProperty({3000}).value &&
             bridge.booleanProperty({3001}).supported &&
             bridge.booleanProperty({3001}).value &&
             bridge.booleanProperty({3020}).supported &&
             !bridge.booleanProperty({3020}).value &&
             bridge.booleanProperty({3021}).supported &&
             bridge.booleanProperty({3021}).value,
         "captured STATE_PRACTICE menu authority drives source row booleans");
  expect(bridge.executeEvent(370, {}).status == SkinHostCallStatus::Completed &&
             bridge.executeEvent(371, std::array{-1}).status ==
                 SkinHostCallStatus::Completed,
         "practice item events preserve Java's default increment and negative "
         "decrement direction");
  const auto committed = bridge.commitFrame();
  expect(committed.orderedMutations.size() == 2,
         "active practice events stage one mutation per source event");
  if (committed.orderedMutations.size() == 2) {
    const auto *first = std::get_if<SetPracticeMenuItem>(
        &committed.orderedMutations[0]);
    const auto *second = std::get_if<SetPracticeMenuItem>(
        &committed.orderedMutations[1]);
    expect(first != nullptr && first->visibleIndex == 0 && first->increment &&
               second != nullptr && second->visibleIndex == 1 &&
               !second->increment,
           "practice event mutations retain visible row and source direction");
  }

  state.clock.serial = 207;
  state.authority.practiceMenuActive = false;
  bridge.beginFrame(state, projectionAt(207));
  expect(bridge.booleanProperty({1080}).supported &&
             !bridge.booleanProperty({1080}).value &&
             bridge.booleanProperty({3000}).supported &&
             !bridge.booleanProperty({3000}).value &&
             bridge.executeEvent(370, {}).status == SkinHostCallStatus::Completed &&
             bridge.commitFrame().orderedMutations.empty(),
         "practice item events remain source no-ops outside STATE_PRACTICE");
}

void testLiftHiddenOffsetsFollowPinnedLaneRenderer() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  SkinNoteObject note;
  note.lanes.push_back({.authoredLane = 0,
                        .laneDestination = {.x = 100.0,
                                            .y = 200.0,
                                            .width = 40.0,
                                            .height = 200.0}});
  model.model.objects.push_back(
      {.id = 1, .payload = std::move(note), .authoredOrdinal = 1});
  BeatorajaSkinConfiguration configuration;
  configuration.offsetsById.emplace(3, ConfigOffset{.x = 7});
  configuration.offsetsById.emplace(4, ConfigOffset{.x = 8});
  configuration.offsetsById.emplace(5, ConfigOffset{.x = 9});
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(208);
  state.authority.laneCoverEnabled = true;
  state.authority.laneCoverPercent = 40;
  state.authority.liftEnabled = true;
  state.authority.liftRatio = 0.333F;
  state.authority.hiddenEnabled = true;
  state.authority.hiddenRatio = 0.25F;
  bridge.beginFrame(state, projectionAt(208));
  const auto lift = bridge.offsetProperty(3);
  const auto laneCover = bridge.offsetProperty(4);
  const auto hidden = bridge.offsetProperty(5);
  expect(lift.supported && laneCover.supported && hidden.supported &&
             lift.value.x == 7.0 && laneCover.value.x == 8.0 &&
             hidden.value.x == 9.0 &&
             std::abs(lift.value.y - 66.6) < 0.0001 &&
             std::abs(laneCover.value.y + 53.36) < 0.0001 &&
             std::abs(hidden.value.y - 33.35) < 0.0001 &&
             hidden.value.a == 0.0,
         "LaneRenderer writes fractional Lift, lane-cover, and HIDDEN "
         "offsets while preserving the loaded reserved offset fields");
  bridge.discardFrame();

  state = stateAt(209);
  state.authority.laneCoverEnabled = true;
  state.authority.laneCoverPercent = 40;
  state.authority.liftEnabled = false;
  state.authority.hiddenEnabled = false;
  bridge.beginFrame(state, projectionAt(209));
  const auto disabledLift = bridge.offsetProperty(3);
  const auto disabledLaneCover = bridge.offsetProperty(4);
  const auto disabledHidden = bridge.offsetProperty(5);
  expect(disabledLift.supported && disabledLaneCover.supported &&
             disabledHidden.supported && disabledLift.value.y == 0.0 &&
             disabledLaneCover.value.y == -80.0 &&
             std::abs(disabledHidden.value.y - 33.35) < 0.0001 &&
             disabledHidden.value.a == -255.0,
         "disabled HIDDEN keeps LaneRenderer's prior y while suppressing "
         "draws through alpha");
  bridge.discardFrame();
}

void testRemainingDirectGameplayStatePropertyWiring() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.chartMd5 = "captured-md5";
  chart.chartSha256 = "captured-sha256";
  chart.staticMetadata = {.totalNotes = 100,
                          .playLevel = 12,
                          .hasRandomSequence = true,
                          .hasBpmStop = true};

  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(207);
  state.score = 120;
  state.combo = 12;
  state.authority.maximumCombo = 44;
  state.authority.stagePassedNotes = 80;
  // Intentionally leave the optional pacemaker playback snapshot empty: the
  // source score-rate/current-rank family uses JudgeManager.pastNotes, not a
  // selected pacemaker's progress.
  state.authority.pacemakerStatus.playedNotes = 0;
  state.authority.bestScore = 100;
  state.authority.bestScoreTarget = {.enabled = true,
                                     .finalScore = 100,
                                     .maxScore = 200,
                                     .totalNotes = 100};
  state.authority.pacemakerTarget = {.enabled = true,
                                     .finalScore = 150,
                                     .maxScore = 200,
                                     .totalNotes = 100};
  state.authority.judgementCounters = {{PGreat, 40}, {Great, 40}};
  state.authority.loadingState = PlayfieldLoadingState::Loaded;
  state.configuration.masterVolume = 0.25F;
  state.configuration.keysoundVolume = 0.5F;
  state.configuration.bgmVolume = 0.75F;
  bridge.beginFrame(state, projectionAt(207));

  for (const auto [id, expected] : std::array{
           std::pair{178, false}, std::pair{179, true},
           std::pair{1177, true},
           std::pair{192, true}, std::pair{193, false},
           // score / (passed notes * 2) = 120 / 160 = 75% (A).
           std::pair{200, false}, std::pair{201, false},
           std::pair{202, true}, std::pair{300, false},
           std::pair{302, true}, std::pair{340, false},
           std::pair{342, true},
           // Final score rate is 60%, so B and all lower inclusive rank
           // options are active. The persisted 50% score is exclusively C.
           std::pair{220, false}, std::pair{221, false},
           std::pair{222, false}, std::pair{223, true},
           std::pair{320, false}, std::pair{324, true}}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && value.value == expected,
           "direct chart and score-rank boolean property uses the pinned "
           "source: " +
               std::to_string(id));
  }

  for (const auto [id, expected] : std::array{
           std::pair{45, 12LL}, std::pair{46, 12LL},
           std::pair{47, 12LL}, std::pair{48, 12LL},
           std::pair{49, 12LL}, std::pair{96, 12LL},
           std::pair{72, 200LL}, std::pair{102, 75LL},
           std::pair{103, 0LL}, std::pair{104, 12LL},
           std::pair{115, 60LL}, std::pair{116, 0LL},
           std::pair{122, 75LL}, std::pair{123, 0LL},
           std::pair{135, 75LL}, std::pair{136, 0LL},
           std::pair{154, 11LL}, std::pair{155, 60LL},
           std::pair{156, 0LL}, std::pair{157, 75LL},
           std::pair{158, 0LL}, std::pair{170, 100LL},
           std::pair{171, 120LL}, std::pair{172, 40LL},
           std::pair{174, 44LL}, std::pair{183, 50LL},
           std::pair{184, 0LL}, std::pair{57, 25LL},
           std::pair{58, 50LL}, std::pair{59, 75LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "direct score property uses the pinned source: " +
               std::to_string(id));
  }
  for (const auto [id, expected] : std::array{
           std::pair{17, 0.25}, std::pair{18, 0.5}, std::pair{19, 0.75},
           std::pair{20, 0.0}}) {
    const auto value = bridge.floatProperty({id});
    expect(value.supported && value.value == expected,
           "direct audio float property uses the pinned source: " +
               std::to_string(id));
  }
  const auto currentRate = bridge.floatProperty({111});
  const auto loadingProgress =
      bridge.floatProperty({165}, SkinFloatPropertyDomain::FloatValue);
  const auto currentRateFloat = bridge.floatProperty(
      {1102}, SkinFloatPropertyDomain::FloatValue);
  expect(currentRate.supported && currentRateFloat.supported &&
             std::abs(currentRate.value - 0.75) < 0.000001 &&
             std::abs(currentRateFloat.value - 0.75) < 0.000001,
         "current score rates use JudgeManager past notes even without a "
         "pacemaker target");
  expect(loadingProgress.supported && loadingProgress.value == 1.0,
         "loaded gameplay exposes BMSResource's completed float progress");
  expect(bridge.stringProperty({1030}).supported &&
             bridge.stringProperty({1030}).value == "captured-md5" &&
             bridge.stringProperty({1031}).supported &&
             bridge.stringProperty({1031}).value == "captured-sha256",
         "chart hash strings use immutable parser metadata");
  bridge.discardFrame();

  state.clock.serial = 208;
  state.authority.courseMode = true;
  state.authority.courseStageIndex = 1;
  state.authority.courseStageCount = 4;
  state.authority.courseStageTitles = {"first", "second", "third", "fourth"};
  state.authority.playerName = "captured-player";
  state.authority.laneCoverAdjustmentHeld = true;
  bridge.beginFrame(state, projectionAt(208));
  expect(!bridge.booleanProperty({280}).value &&
             bridge.booleanProperty({281}).value &&
             !bridge.booleanProperty({282}).value &&
             !bridge.booleanProperty({283}).value &&
             !bridge.booleanProperty({289}).value &&
             bridge.booleanProperty({290}).value &&
             bridge.booleanProperty({270}).value,
         "course stage selectors use the captured course session position");
  expect(bridge.stringProperty({150}).value == "first" &&
             bridge.stringProperty({151}).value == "second" &&
             bridge.stringProperty({159}).value.empty(),
         "course title strings use the captured per-stage chart titles");
  expect(bridge.stringProperty({2}).value == "captured-player",
         "player string property uses the captured active player name");
  bridge.discardFrame();

  state.clock.serial = 209;
  state.authority.courseStageIndex = 3;
  bridge.beginFrame(state, projectionAt(209));
  expect(bridge.booleanProperty({289}).value,
         "course final-stage selector follows the final captured stage");
  for (const int id : {1002, 1003, 1004, 1005, 1006, 1007, 1010, 1011,
                       1012, 1013, 1014, 1015, 1016, 1017}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && !value.value,
           "course constraint option is inactive during Beatoraja gameplay: " +
               std::to_string(id));
  }
  bridge.discardFrame();

  state.clock.serial = 210;
  state.authority.courseStageIndex = 6;
  state.authority.courseStageCount = 10;
  bridge.beginFrame(state, projectionAt(210));
  const auto unsupportedCourseStage = bridge.booleanProperty({286});
  expect(!bridge.booleanProperty({280}).value &&
             !bridge.booleanProperty({289}).value &&
             !unsupportedCourseStage.supported,
         "only Beatoraja's four course stage selectors and final selector "
         "are defined");
  bridge.discardFrame();

  state.clock.serial = 211;
  state.authority.player1RandomOption = 3;
  state.authority.player2RandomOption = 6;
  state.authority.doublePlayOption = 1;
  state.configuration.judgeAlgorithmImageIndex = 1;
  bridge.beginFrame(state, projectionAt(211));
  expect(bridge.integerProperty({42}, SkinIntegerPropertyDomain::ImageIndex)
                 .value == 3 &&
             bridge.integerProperty({43}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 6 &&
             bridge.integerProperty({54}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 1,
         "random and double-play image indexes use captured play options");
  expect(bridge.integerProperty({340}, SkinIntegerPropertyDomain::ImageIndex)
                 .value == 1,
         "judge-algorithm image index preserves the pinned duration mode");
  for (const int id : {61, 62, 63}) {
    const auto targetOption =
        bridge.integerProperty({id}, SkinIntegerPropertyDomain::ImageIndex);
    expect(targetOption.supported &&
               targetOption.value == std::numeric_limits<int>::min(),
           "target option image indexes preserve the source null-target "
           "sentinel");
  }
  bridge.discardFrame();

  state.clock.serial = 212;
  state.configuration.judgeAlgorithmImageIndex =
      std::numeric_limits<std::int32_t>::min();
  state.authority.targetPlayOption = 123;
  bridge.beginFrame(state, projectionAt(212));
  const auto scorePriority =
      bridge.integerProperty({340}, SkinIntegerPropertyDomain::ImageIndex);
  expect(scorePriority.supported &&
             scorePriority.value == std::numeric_limits<std::int32_t>::min(),
         "judge-algorithm Score preserves Beatoraja's non-index sentinel");
  expect(bridge.integerProperty({61}, SkinIntegerPropertyDomain::ImageIndex)
                 .value == 3 &&
             bridge.integerProperty({62}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 2 &&
             bridge.integerProperty({63}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 1,
         "target option image indexes split ScoreData.option with the pinned "
         "decimal divisors");
  bridge.discardFrame();
}

void testLongNoteHoldTimersUseCapturedLaneState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.keyCount = 7;
  // Aso stores 7K in visible order (scratch, then keys); Beatoraja maps the
  // key at raw BMS lane 0 to skin offset 1 and the scratch to offset 0.
  chart.laneOrder = {7, 0};
  chart.notes = {
      {.id = 1,
       .timelineId = 1,
       .pairId = 2,
       .lane = 0,
       .kind = ChartVisualNoteKind::LongHead},
      {.id = 2,
       .timelineId = 2,
       .pairId = 1,
       .lane = 0,
       .kind = ChartVisualNoteKind::LongTail},
  };
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(212);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 6'000'000;
  state.lastJudgeVisualMicros = 1'000'000;
  state.notes = {{.id = 1, .longActive = true},
                 {.id = 2, .longActive = true}};
  bridge.beginFrame(state, projectionAt(212));
  expect(bridge.timerProperty({71}) == 6'000'000 &&
             bridge.timerProperty({70}) == kPlayfieldTimestampOff,
         "Beatoraja 1P hold timers use captured long-note state and the "
         "source lane offset, not a stale judge timestamp");
  bridge.discardFrame();

  state.clock.serial = 213;
  state.clock.visualTimeMicros = 7'000'000;
  state.notes = {{.id = 1, .longActive = false},
                 {.id = 2, .longActive = false}};
  bridge.beginFrame(state, projectionAt(213));
  expect(bridge.timerProperty({71}) == kPlayfieldTimestampOff,
         "Beatoraja 1P hold timer turns off when its captured long note ends");
  bridge.discardFrame();

  state.clock.serial = 214;
  state.notes = {{.id = 1, .longActive = true, .longReactive = true},
                 {.id = 2, .longActive = true, .longReactive = true}};
  bridge.beginFrame(state, projectionAt(214));
  expect(bridge.timerProperty({251}) == 7'000'000 &&
             bridge.timerProperty({271}) == kPlayfieldTimestampOff,
         "normal-range HCN active timer uses the captured increase state, "
         "not the generic long-note hold state");
  bridge.discardFrame();

  state.clock.serial = 215;
  state.notes = {{.id = 1, .longDamaged = true},
                 {.id = 2, .longDamaged = true}};
  bridge.beginFrame(state, projectionAt(215));
  expect(bridge.timerProperty({251}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({271}) == 7'000'000,
         "normal-range HCN damage timer stays independent from active HCN");
  bridge.discardFrame();
}

void testExtendedPlayerOneLaneTimersUsePinnedSkinOffsets() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  // Beatoraja LaneProperty.KEYBOARD_24K maps the physical lane at index 9
  // to skin offset 10. SkinPropertyMapper must therefore select the extended
  // 1P timer ranges rather than treating this as an absent normal-key lane.
  PlayfieldChartVisualModel chart;
  chart.keyCount = 24;
  chart.laneOrder = {9};
  chart.notes = {{.id = 1,
                  .timelineId = 1,
                  .lane = 9,
                  .kind = ChartVisualNoteKind::LongHead}};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(214);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 8'000'000;
  state.lanes = {{.pressed = true,
                  .pressMicros = 7'000'000,
                  .bombMicros = 7'500'000}};
  state.notes = {{.id = 1, .longActive = true, .longReactive = true}};
  bridge.beginFrame(state, projectionAt(214));
  expect(bridge.timerProperty({1010}) == 7'500'000 &&
             bridge.timerProperty({1210}) == 8'000'000 &&
             bridge.timerProperty({1410}) == 7'000'000 &&
             bridge.timerProperty({1810}) == 8'000'000 &&
             bridge.timerProperty({2010}) == kPlayfieldTimestampOff,
         "extended 1P bomb, hold, key-on, and HCN timers use LaneProperty's "
         "24K offset and JudgeManager's active HCN state");
  bridge.discardFrame();

  state.clock.serial = 215;
  state.lanes = {{.pressed = false, .releaseMicros = 8'100'000}};
  state.notes = {{.id = 1, .longDamaged = true}};
  bridge.beginFrame(state, projectionAt(215));
  expect(bridge.timerProperty({1610}) == 8'100'000 &&
             bridge.timerProperty({1810}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({2010}) == 8'000'000,
         "extended 1P key-off and HCN-damage timers retain the pinned "
         "independent inactive and damage states");
  bridge.discardFrame();
}

void testPomyuTimersFollowPinnedDefaultProcessorCycles() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(215);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 0;
  state.clock.gameplayTimeMicros = 0;
  state.clock.playTimer = {.active = true,
                           .startMicros = 0,
                           .elapsedMillisExact = true};
  state.authority.stagePassedNotes = 0;
  state.lastJudge = JudgeResult(None, 0);
  bridge.beginFrame(state, projectionAt(215));
  expect(bridge.timerProperty({900}) == 0 &&
             bridge.timerProperty({905}) == 0 &&
             bridge.timerProperty({909}) == 0 &&
             bridge.timerProperty({901}) == kPlayfieldTimestampOff,
         "PomyuCharaProcessor initializes both neutral motions and dance "
         "with its pinned unconfigured state");
  bridge.discardFrame();

  state.clock.serial = 216;
  state.clock.visualTimeMicros = 2'000;
  state.clock.gameplayTimeMicros = 2'000;
  state.authority.stagePassedNotes = 1;
  state.lastJudge = JudgeResult(PGreat, 0);
  bridge.beginFrame(state, projectionAt(216));
  expect(bridge.timerProperty({900}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({902}) == 2'000 &&
             bridge.timerProperty({905}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({907}) == 2'000 &&
             bridge.timerProperty({909}) == 0,
         "PomyuCharaProcessor maps a PGREAT to 1P great and 2P bad after "
         "the default neutral-cycle boundary");
  bridge.discardFrame();

  state.clock.serial = 217;
  state.clock.visualTimeMicros = 4'000;
  state.clock.gameplayTimeMicros = 4'000;
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.timerProperty({900}) == 4'000 &&
             bridge.timerProperty({902}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({905}) == 4'000 &&
             bridge.timerProperty({907}) == kPlayfieldTimestampOff,
         "PomyuCharaProcessor returns completed default motions to their "
         "neutral timers and captures the processed-note count");
  bridge.discardFrame();
}

void testPomyuTimersUseAuthoredMotionCycles() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({
      .chartModel = chart,
      .model = &model,
      .configuration = configuration,
      .runtime = runtime.runtime(),
      .mutationTable = mutations,
      .pomyuMotionCyclesMillis = {100, 250, 250, 250, 250, 100, 250, 250},
  });

  auto state = stateAt(215);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 0;
  state.clock.gameplayTimeMicros = 0;
  state.clock.playTimer = {.active = true,
                           .startMicros = 0,
                           .elapsedMillisExact = true};
  state.authority.stagePassedNotes = 0;
  state.lastJudge = JudgeResult(None, 0);
  bridge.beginFrame(state, projectionAt(215));
  bridge.discardFrame();

  state.clock.serial = 216;
  state.clock.visualTimeMicros = 2'000;
  state.clock.gameplayTimeMicros = 2'000;
  state.authority.stagePassedNotes = 1;
  state.lastJudge = JudgeResult(PGreat, 0);
  bridge.beginFrame(state, projectionAt(216));
  expect(bridge.timerProperty({900}) == 0 &&
             bridge.timerProperty({902}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({905}) == 0 &&
             bridge.timerProperty({907}) == kPlayfieldTimestampOff,
         "PomyuCharaProcessor leaves authored neutral motions active until "
         "their source-defined cycle boundary");
  bridge.discardFrame();

  state.clock.serial = 217;
  state.clock.visualTimeMicros = 100'000;
  state.clock.gameplayTimeMicros = 100'000;
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.timerProperty({900}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({902}) == 100'000 &&
             bridge.timerProperty({905}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({907}) == 100'000,
         "PomyuCharaProcessor enters judgement motions at the authored "
         "neutral-cycle boundary");
  bridge.discardFrame();

  state.clock.serial = 218;
  state.clock.visualTimeMicros = 350'000;
  state.clock.gameplayTimeMicros = 350'000;
  bridge.beginFrame(state, projectionAt(218));
  expect(bridge.timerProperty({900}) == 350'000 &&
             bridge.timerProperty({902}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({905}) == 350'000 &&
             bridge.timerProperty({907}) == kPlayfieldTimestampOff,
         "PomyuCharaProcessor holds authored judgement motions for their "
         "own configured cycle before returning to neutral");
  bridge.discardFrame();
}

void testIrProviderStringUsesCapturedProfileConfiguration() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(215);
  state.authority.irProviderName = "tachi";
  bridge.beginFrame(state, projectionAt(215));
  expect(bridge.stringProperty({1020}).value == "tachi",
         "StringPropertyFactory.irname exposes the captured first configured "
         "IR provider during gameplay");
  bridge.discardFrame();
}

void testIrAccountStringUsesCapturedConnectedAccount() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(216);
  state.authority.irAccountName = "source-account";
  bridge.beginFrame(state, projectionAt(216));
  expect(bridge.stringProperty({1021}).value == "source-account",
         "StringPropertyFactory.irUserName exposes the first connected IR "
         "account rather than a local or provider name");
  bridge.discardFrame();

  state.clock.serial = 217;
  state.authority.irAccountName.clear();
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.stringProperty({1021}).value.empty(),
         "StringPropertyFactory.irUserName remains empty without a connected "
         "account");
  bridge.discardFrame();
}

void testSongInformationPropertiesUseImmutableSourceAnalysis() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.staticMetadata.songInformation = {
      .density = 0.75, .peakDensity = 1.0, .endDensity = 1.0, .total = 100.0};
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(216);
  bridge.beginFrame(state, projectionAt(216));
  expect(bridge.integerProperty({360}).value == 1 &&
             bridge.integerProperty({361}).value == 0 &&
             bridge.integerProperty({362}).value == 1 &&
             bridge.integerProperty({363}).value == 0 &&
             bridge.integerProperty({364}).value == 0 &&
             bridge.integerProperty({365}).value == 75 &&
             bridge.integerProperty({368}).value == 100,
         "SongInformation integer properties preserve Beatoraja's whole-"
         "second density and decimal-part conversions");
  expect(std::abs(bridge.floatProperty({360}, SkinFloatPropertyDomain::FloatValue)
                      .value -
                  1.0) < 0.000001 &&
             std::abs(bridge.floatProperty({362},
                                             SkinFloatPropertyDomain::FloatValue)
                          .value -
                      1.0) < 0.000001 &&
             std::abs(bridge.floatProperty({367},
                                             SkinFloatPropertyDomain::FloatValue)
                          .value -
                      0.75) < 0.000001 &&
             std::abs(bridge.floatProperty({368},
                                             SkinFloatPropertyDomain::FloatValue)
                          .value -
                      100.0) < 0.000001,
         "SongInformation float properties retain Beatoraja's unrounded "
         "analysis values");
  bridge.discardFrame();
}

void testPersistedScorePropertiesUseScoreDataRatherThanLiveJudgements() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(217);
  state.authority.judgementCounters = {{PGreat, 1}, {Great, 2}};
  state.authority.persistedScore = PlayfieldPersistedScoreState{
      .score = 165,
      .maxScore = 200,
      .totalNotes = 100,
      .judgementCounts = {80, 10, 5, 3, 2},
      .lastPlayedUnixSeconds = 1'700'000'000};
  state.authority.playerScoreHistory = {
      .playCount = 17,
      .clearCount = 11,
      .judgementCounts = {900, 80, 7, 6, 5},
      .playDurationSeconds = 7'384};
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.integerProperty({80}).value == 80 &&
             bridge.integerProperty({81}).value == 10 &&
             bridge.integerProperty({84}).value == 2 &&
             bridge.integerProperty({85}).value == 80 &&
             bridge.integerProperty({86}).value == 10 &&
             bridge.integerProperty({89}).value == 2 &&
             bridge.integerProperty({243}).value == 1'700'000'000,
         "ScoreData integer counts, rates, and date use the persisted high "
         "score record rather than current JudgeManager counters");
  expect(bridge.integerProperty({30}).value == 17 &&
             bridge.integerProperty({31}).value == 11 &&
             bridge.integerProperty({32}).value == 6 &&
             bridge.integerProperty({33}).value == 900 &&
             bridge.integerProperty({37}).value == 5 &&
             bridge.integerProperty({333}).value == 993 &&
             bridge.integerProperty({17}).value == 2 &&
             bridge.integerProperty({18}).value == 3 &&
             bridge.integerProperty({19}).value == 4,
         "PlayerData properties use the immutable local player-history "
         "aggregate rather than the active chart");
  expect(std::abs(bridge.floatProperty({85}, SkinFloatPropertyDomain::FloatValue)
                      .value -
                  0.8) < 0.000001 &&
             std::abs(bridge.floatProperty({89}, SkinFloatPropertyDomain::FloatValue)
                          .value -
                      0.02) < 0.000001,
         "ScoreData float rates use the persisted record denominator");
  bridge.discardFrame();
}

void testRivalScorePropertiesRequireCapturedTargetScoreData() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(225);
  bridge.beginFrame(state, projectionAt(225));
  expect(bridge.integerProperty({271}).value == 0 &&
             bridge.integerProperty({280}).value ==
                 std::numeric_limits<int>::min() &&
             bridge.floatProperty({285}, SkinFloatPropertyDomain::Rate).value ==
                 std::numeric_limits<float>::min(),
         "ScoreDataProperty preserves zero rival score but sentinels for an "
         "absent rival ScoreData record");
  bridge.discardFrame();

  state = stateAt(226);
  state.authority.rivalScore = PlayfieldRivalScoreState{
      .score = 170,
      .totalNotes = 100,
      .judgementCounts = {80, 10, 5, 3, 2},
  };
  bridge.beginFrame(state, projectionAt(226));
  expect(bridge.integerProperty({271}).value == 170 &&
             bridge.integerProperty({280}).value == 80 &&
             bridge.integerProperty({284}).value == 2 &&
             bridge.integerProperty({285}).value == 80 &&
             bridge.integerProperty({289}).value == 2 &&
             std::abs(bridge.floatProperty({285}, SkinFloatPropertyDomain::Rate)
                          .value -
                      0.8) < 0.000001 &&
             std::abs(bridge.floatProperty({289}, SkinFloatPropertyDomain::Rate)
                          .value -
                      0.02) < 0.000001,
         "rival score/count/rate selectors use the captured target ScoreData "
         "record and its own note denominator");
  bridge.discardFrame();
}

void testWallClockPropertiesUseTheLocalCalendar() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(218);
  bridge.beginFrame(state, projectionAt(218));
  const auto year = bridge.integerProperty({21});
  const auto month = bridge.integerProperty({22});
  const auto day = bridge.integerProperty({23});
  const auto hour = bridge.integerProperty({24});
  const auto minute = bridge.integerProperty({25});
  const auto second = bridge.integerProperty({26});
  expect(year.supported && month.supported && day.supported && hour.supported &&
             minute.supported && second.supported && year.value > 1970 &&
             month.value >= 1 && month.value <= 12 && day.value >= 1 &&
             day.value <= 31 && hour.value >= 0 && hour.value <= 23 &&
             minute.value >= 0 && minute.value <= 59 && second.value >= 0 &&
             second.value <= 59,
         "wall-clock properties expose the source local Calendar fields");
  bridge.discardFrame();
}

void testRuntimeFpsAndUptimePropertiesUseCapturedApplicationAuthority() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(224);
  state.authority.currentFramesPerSecond = 144;
  state.authority.applicationUptimeMillis = 3'661'999;
  bridge.beginFrame(state, projectionAt(224));
  expect(bridge.integerProperty({20}).supported &&
             bridge.integerProperty({20}).value == 144 &&
             bridge.integerProperty({27}).value == 1 &&
             bridge.integerProperty({28}).value == 1 &&
             bridge.integerProperty({29}).value == 1,
         "current FPS and boot-time fields use the captured live application "
         "authority with Beatoraja's integer divisions");
  bridge.discardFrame();
}

void testStartInputTimerUsesPinnedSkinTiming() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.timing.inputMillis = 150;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(217);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 150'000;
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.timerProperty({1}) == kPlayfieldTimestampOff,
         "TIMER_STARTINPUT stays off at the authored input boundary because "
         "BMSPlayer uses a strict greater-than comparison");
  bridge.discardFrame();

  state.clock.serial = 218;
  state.clock.visualTimeMicros = 150'001;
  bridge.beginFrame(state, projectionAt(218));
  expect(bridge.timerProperty({1}) == 150'001,
         "TIMER_STARTINPUT preserves the first post-delay gameplay-clock "
         "timestamp rather than the authored delay");
  bridge.discardFrame();
}

void testFailureTimerUsesCapturedSurvivalFailureEvent() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(219);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 2'000'000;
  bridge.beginFrame(state, projectionAt(219));
  expect(bridge.timerProperty({3}) == kPlayfieldTimestampOff,
         "TIMER_FAILED stays off when no captured survival failure exists");
  bridge.discardFrame();

  state.clock.serial = 220;
  state.authority.failureAnimationActive = true;
  bridge.beginFrame(state, projectionAt(220));
  expect(bridge.timerProperty({3}) == 2'000'000,
         "TIMER_FAILED starts from the captured active-gauge failure frame, "
         "not an inferred display gauge threshold");
  bridge.discardFrame();
}

void testRhythmTimerUsesPinnedSectionAndBpmAccumulator() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.timelines = {
      {.timeMicros = 1'000'000, .bpm = 60.0, .sectionLine = true},
      {.timeMicros = 2'000'000, .bpm = 60.0, .sectionLine = true},
  };
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(230);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 0;
  state.clock.gameplayTimeMicros = 0;
  state.clock.playTimer = {.active = true,
                           .startMicros = 0,
                           .elapsedMillisExact = true};
  state.authority.currentBpm = 60.0;
  bridge.beginFrame(state, projectionAt(230));
  bridge.discardFrame();

  state.clock.serial = 231;
  state.clock.visualTimeMicros = 1'000'000;
  state.clock.gameplayTimeMicros = 1'000'000;
  bridge.beginFrame(state, projectionAt(231));
  expect(bridge.timerProperty({140}) == 1'000'000,
         "TIMER_RHYTHM resets to the current skin clock at a section line");
  bridge.discardFrame();

  state.clock.serial = 232;
  state.clock.visualTimeMicros = 1'250'000;
  state.clock.gameplayTimeMicros = 1'250'000;
  state.authority.currentBpm = 120.0;
  bridge.beginFrame(state, projectionAt(232));
  expect(bridge.timerProperty({140}) == 750'000,
         "TIMER_RHYTHM advances with RhythmTimerProcessor's integer BPM accumulator");
  bridge.discardFrame();

  state.clock.serial = 233;
  state.clock.visualTimeMicros = 2'000'000;
  state.clock.gameplayTimeMicros = 2'000'000;
  bridge.beginFrame(state, projectionAt(233));
  expect(bridge.timerProperty({140}) == 2'000'000,
         "TIMER_RHYTHM applies its section reset after writing the accumulated timer");
  bridge.discardFrame();
}

void testFavoriteChartImageIndexUsesCapturedRepositoryState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(221);
  state.authority.songReviewFavorite = 2;
  bridge.beginFrame(state, projectionAt(221));
  expect(bridge.integerProperty({90}, SkinIntegerPropertyDomain::ImageIndex)
                 .supported &&
             bridge.integerProperty({90}, SkinIntegerPropertyDomain::ImageIndex)
                     .value ==
                 1,
         "favorite_chart uses the captured chart repository state rather than "
         "the generic image-index fallback");
  bridge.discardFrame();
}

void testSongReviewImageIndexesUsePinnedBitmaskStates() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(222);
  state.authority.songReviewFavorite = 3;
  bridge.beginFrame(state, projectionAt(222));
  expect(bridge.integerProperty({89}, SkinIntegerPropertyDomain::ImageIndex)
                 .value == 1 &&
             bridge.integerProperty({90}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 1,
         "SongReview favourite bits select state one for both image indexes");
  bridge.discardFrame();

  state.clock.serial = 223;
  state.authority.songReviewFavorite = 12;
  bridge.beginFrame(state, projectionAt(223));
  expect(bridge.integerProperty({89}, SkinIntegerPropertyDomain::ImageIndex)
                 .value == 2 &&
             bridge.integerProperty({90}, SkinIntegerPropertyDomain::ImageIndex)
                     .value == 2,
         "SongReview invisible bits take precedence for both image indexes");
  bridge.discardFrame();
}

void testDifficultyTableStringsUseCapturedSelectionContext() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(224);
  state.authority.tableName = "Example Difficulty Table";
  state.authority.tableLevel = "★12";
  state.authority.tableFullName = "★12Example Difficulty Table";
  bridge.beginFrame(state, projectionAt(224));
  expect(bridge.stringProperty({1001}).value == "Example Difficulty Table" &&
             bridge.stringProperty({1002}).value == "★12" &&
             bridge.stringProperty({1003}).value ==
                 "★12Example Difficulty Table",
         "table string selectors preserve PlayerResource's level-before-name "
         "concatenation");
  bridge.discardFrame();
}

void testPlayerConfigurationStringsUseCapturedSourceValues() {
  // StringPropertyFactory reads these four PlayerConfig values in every
  // gameplay state.  The display strings below are the pinned enum outputs;
  // sort and replication stay the exact configured identifiers.
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(233);
  state.authority.modeFilterName = "14KEY";
  state.authority.sortId = "MISSCOUNT";
  state.authority.difficultyFilterName = "SPEED CHANGE CHART";
  state.authority.chartReplicationMode = "RIVALCHART";
  bridge.beginFrame(state, projectionAt(233));

  expect(bridge.stringProperty({60}).value == "14KEY" &&
             bridge.stringProperty({"sort"}).value == "MISSCOUNT" &&
             bridge.stringProperty({62}).value == "SPEED CHANGE CHART" &&
             bridge.stringProperty({"chartreplication"}).value ==
                 "RIVALCHART",
         "source PlayerConfig strings survive immutable gameplay capture");
}

void testConfiguredTargetNameNeighborsFollowPinnedTargetRing() {
  // StringPropertyFactory resolves 200-209 as the ten preceding target-list
  // positions and 210-219 as the following ten positions around targetid.
  // These literals exercise TargetProperty's static, IR, missing-rival, and
  // unknown-id-to-MAX name branches without using Aso's pacemaker labels.
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(234);
  state.authority.skinTargetId = "RANK_NEXT";
  state.authority.skinTargetList = {
      "RATE_A", "RANK_NEXT", "IR_NEXT_3", "RIVAL_1", "unknown-target"};
  bridge.beginFrame(state, projectionAt(234));

  expect(bridge.stringProperty({200}).value == "NEXT RANK" &&
             bridge.stringProperty({"targetnamep1"}).value == "RANK A" &&
             bridge.stringProperty({210}).value == "IR NEXT 3RANK" &&
             bridge.stringProperty({"targetnamen2"}).value == "NO RIVAL" &&
             bridge.stringProperty({212}).value == "MAX" &&
             bridge.stringProperty({213}).value == "RANK A",
         "configured target neighbours preserve the pinned target-list ring "
         "and TargetProperty display branches");
  bridge.discardFrame();

  state = stateAt(235);
  state.authority.skinTargetId = "RANK_NEXT";
  state.authority.skinTargetList = {"RATE_ 50 ", "RANK_NEXT"};
  bridge.beginFrame(state, projectionAt(235));
  expect(bridge.stringProperty({210}).value == "SCORE RATE 50.0%",
         "custom RATE target labels retain Java Float.parseFloat whitespace "
         "semantics");
}

void testTargetScoreStringsFollowPinnedTargetSource() {
  // During BMSPlayer gameplay, StringPropertyFactory.rival and .target both
  // read targetScoreData.player. These cases cover the source's static,
  // local-only rival, no-ranking IR, and practice branches without treating
  // Aso's pacemaker label as a player name.
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(236);
  state.authority.gameplayMode = PlayfieldGameplayMode::Play;
  state.authority.skinTargetId = "RATE_AA";
  bridge.beginFrame(state, projectionAt(236));
  expect(bridge.stringProperty({1}).value == "RANK AA" &&
             bridge.stringProperty({"target"}).value == "RANK AA",
         "static target ScoreData player labels drive both gameplay strings");
  bridge.discardFrame();

  state = stateAt(237);
  state.authority.gameplayMode = PlayfieldGameplayMode::Play;
  state.authority.skinTargetId = "RIVAL_RANK_1";
  state.authority.persistedScore = PlayfieldPersistedScoreState{.score = 42};
  bridge.beginFrame(state, projectionAt(237));
  expect(bridge.stringProperty({1}).value.empty() &&
             bridge.stringProperty({3}).value.empty(),
         "local-only rival rank preserves the pinned empty self-player name");
  bridge.discardFrame();

  state = stateAt(238);
  state.authority.gameplayMode = PlayfieldGameplayMode::Play;
  state.authority.skinTargetId = "IR_NEXT_1";
  bridge.beginFrame(state, projectionAt(238));
  expect(bridge.stringProperty({1}).value == "NO DATA" &&
             bridge.stringProperty({3}).value == "NO DATA",
         "IR targets retain TargetProperty's no-RankingData player label");
  bridge.discardFrame();

  state = stateAt(239);
  state.authority.gameplayMode = PlayfieldGameplayMode::Practice;
  state.authority.skinTargetId = "MAX";
  bridge.beginFrame(state, projectionAt(239));
  expect(bridge.stringProperty({1}).value.empty() &&
             bridge.stringProperty({3}).value.empty(),
         "practice keeps BMSPlayer's absent target-score strings empty");
}

void testChartDocumentBooleansUseCapturedLibraryMetadata() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(222);
  state.authority.chartHasDocument = true;
  bridge.beginFrame(state, projectionAt(222));
  expect(bridge.booleanProperty({174}).supported &&
             !bridge.booleanProperty({174}).value &&
             bridge.booleanProperty({175}).supported &&
             bridge.booleanProperty({175}).value,
         "song_text and song_no_text mirror SongData.CONTENT_TEXT");
  bridge.discardFrame();

  state = stateAt(223);
  bridge.beginFrame(state, projectionAt(223));
  expect(bridge.booleanProperty({174}).supported &&
             bridge.booleanProperty({174}).value &&
             bridge.booleanProperty({175}).supported &&
             !bridge.booleanProperty({175}).value,
         "document booleans remain complementary when the library flag is off");
  bridge.discardFrame();
}

void testScoreAndComboTimersUseCapturedGameplayState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.staticMetadata.totalNotes = 100;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(208);
  state.sceneStartMicros = 0;
  state.clock.visualTimeMicros = 5'000'000;
  state.clock.gameplayTimeMicros = 5'000'000;
  state.lastJudgeVisualMicros = 4'000'000;
  state.score = 120;
  state.authority.stagePassedNotes = 80;
  state.authority.bestScore = 100;
  state.authority.pacemakerTarget = {.enabled = true,
                                     .finalScore = 110,
                                     .maxScore = 200,
                                     .totalNotes = 100};
  bridge.beginFrame(state, projectionAt(208));
  expect(bridge.timerProperty({446}) == 4'000'000 &&
             bridge.timerProperty({44}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({348}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({349}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({350}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({351}) == 4'000'000 &&
             bridge.timerProperty({352}) == 4'000'000,
         "combo and score timers follow their first captured qualifying "
         "judgement");
  bridge.discardFrame();

  state.clock.serial = 209;
  state.clock.visualTimeMicros = 6'000'000;
  state.clock.gameplayTimeMicros = 6'000'000;
  state.lastJudgeVisualMicros = 6'000'000;
  state.score = 140;
  state.authority.currentGauge = 100.0F;
  bridge.beginFrame(state, projectionAt(209));
  expect(bridge.timerProperty({446}) == 6'000'000 &&
             bridge.timerProperty({44}) == 6'000'000 &&
             bridge.timerProperty({348}) == 6'000'000 &&
             bridge.timerProperty({351}) == 4'000'000 &&
             bridge.timerProperty({352}) == 4'000'000,
         "rank timers start only on their qualifying transition while score "
         "best and target timers retain their original start");
  bridge.discardFrame();

  state.clock.serial = 210;
  state.clock.visualTimeMicros = 7'000'000;
  state.clock.gameplayTimeMicros = 7'000'000;
  state.lastJudgeVisualMicros = 7'000'000;
  state.score = 120;
  state.authority.currentGauge = 99.0F;
  bridge.beginFrame(state, projectionAt(210));
  expect(bridge.timerProperty({446}) == 7'000'000 &&
             bridge.timerProperty({44}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({348}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({351}) == 4'000'000 &&
             bridge.timerProperty({352}) == 4'000'000,
         "rank timers turn off when BMSPlayer's score condition no longer "
         "qualifies");
  bridge.discardFrame();
}

void testPlayTimerPropertiesMatchPinnedJavaConversions() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.staticMetadata.durationMicros = 125'789'000;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(211);
  state.clock.gameplayTimeMicros = 63'499'999;
  state.clock.playTimer = {.active = false,
                           .startMicros = 1'500'000,
                           .elapsedMillisExact = true,
                           .playtimeMillis = 130'789};
  bridge.beginFrame(state, projectionAt(211));
  expect(bridge.timerProperty({41}) == INT64_MIN,
         "an initialized play start remains off without explicit activation");
  expect(bridge.floatProperty({6}).supported &&
             bridge.floatProperty({6}).value == 0.0,
         "inactive Float 6 is exactly zero");
  for (const auto [id, expected] : std::array{
           std::pair{161, 0LL}, std::pair{162, 0LL},
           std::pair{163, 2LL}, std::pair{164, 11LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "inactive play elapsed is zero while time-left retains the Java "
           "one-second bias");
  }
  bridge.discardFrame();

  const auto expectCapturedPlaytime =
      [&](std::uint64_t serial, std::int32_t playtimeMillis,
          double expectedProgress, long long remainingMinutes,
          long long remainingSeconds, std::string_view message) {
        state = stateAt(serial);
        state.clock.gameplayTimeMicros = 63'499'999;
        state.clock.playTimer = {.active = true,
                                 .startMicros = 1'500'000,
                                 .elapsedMillisExact = true,
                                 .playtimeMillis = playtimeMillis};
        bridge.beginFrame(state, projectionAt(serial));
        const auto progress = bridge.floatProperty({6});
        expect(bridge.timerProperty({41}) == 1'500'000,
               "active timer 41 publishes its exact gameplay-clock start");
        expect(progress.supported &&
                   std::abs(progress.value - expectedProgress) <
                       0.000000000001,
               message);
        expect(bridge.integerProperty({161}).supported &&
                   bridge.integerProperty({161}).value == 1 &&
                   bridge.integerProperty({162}).supported &&
                   bridge.integerProperty({162}).value == 1,
               "elapsed play-time fields retain Java long-to-int narrowing");
        expect(bridge.integerProperty({163}).supported &&
                   bridge.integerProperty({163}).value == remainingMinutes &&
                   bridge.integerProperty({164}).supported &&
                   bridge.integerProperty({164}).value == remainingSeconds,
               "time-left consumes the immutable captured playtime authority");
        bridge.discardFrame();
      };

  // Pinned BMSPlayer normal play uses the last playable-note time plus its
  // 5,000ms margin; this intentionally differs from the static chart length.
  expectCapturedPlaytime(212, 130'789, 0.47403833270072937, 1, 9,
                         "Float 6 consumes manual last-playable plus margin");
  // Autoplay instead uses the last timeline (including BGA/background) plus
  // the same margin.
  expectCapturedPlaytime(213, 145'000, 0.4275793135166168, 1, 24,
                         "Float 6 consumes autoplay last-timeline plus margin");
  // Pinned practice computes ((endtime + 1000) * 100 / freq) + 5000.
  // For end=120,000ms and freq=150 this is 85,666ms.
  expectCapturedPlaytime(214, 85'666, 0.7237293720245361, 0, 24,
                         "Float 6 consumes captured practice range/rate time");

  state = stateAt(215);
  state.clock.gameplayTimeMicros = 63'499'999;
  state.clock.playTimer = {.active = true,
                           .startMicros = 1'500'000,
                           .elapsedMillisExact = false};
  bridge.beginFrame(state, projectionAt(215));
  expect(bridge.timerProperty({41}) == INT64_MIN,
         "Timer 41 fails closed without an exact practice elapsed authority");
  expect(!bridge.floatProperty({6}).supported,
         "Float 6 fails closed without an exact practice elapsed authority");
  for (const int id : {161, 162, 163, 164}) {
    expect(!bridge.integerProperty({id}).supported,
           "practice time fields fail closed without the paired TimerManager "
           "clock authority");
  }
  bridge.discardFrame();

  state = stateAt(216);
  state.clock.gameplayTimeMicros = 133'289'000;
  state.clock.playTimer = {.active = true,
                           .startMicros = 1'500'000,
                           .elapsedMillisExact = true,
                           .playtimeMillis = 130'789};
  bridge.beginFrame(state, projectionAt(216));
  expect(bridge.floatProperty({6}).supported &&
             bridge.floatProperty({6}).value == 1.0,
         "Float 6 caps at one after playtime");
  expect(bridge.integerProperty({163}).supported &&
             bridge.integerProperty({163}).value == 0 &&
             bridge.integerProperty({164}).supported &&
             bridge.integerProperty({164}).value == 0,
         "time-left reaches zero exactly at playtime plus the Java bias");
  bridge.discardFrame();

  state = stateAt(217);
  state.clock.gameplayTimeMicros = 2'147'485'148'000;
  state.clock.playTimer = {.active = true,
                           .startMicros = 1'500'000,
                           .elapsedMillisExact = true,
                           .playtimeMillis = 130'789};
  bridge.beginFrame(state, projectionAt(217));
  expect(bridge.floatProperty({6}).supported &&
             bridge.floatProperty({6}).value == 1.0,
         "Float 6 retains the long elapsed value before its upper clamp");
  for (const auto [id, expected] : std::array{
           std::pair{161, -35'791LL}, std::pair{162, -23LL},
           std::pair{163, 0LL}, std::pair{164, 0LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "integer play-time fields reproduce Java long-to-int narrowing "
           "and signed remainder boundaries");
  }
  bridge.discardFrame();

  const auto catalog = gameplaySkinBuiltinCatalog();
  for (const int id : {32,   -32,  42,   -42,  400,
                       -400, 603,  -603, 1002, -1002, 1177, -1177,
                       2246, -2246, 3000, -3000, 3015, -3015, 3020,
                       -3020, 3035, -3035}) {
    expect(catalog.contains({.kind = SkinBindingKind::BooleanProperty},
                            SkinBuiltinPropertySelector{id}),
           "gameplay catalog admits each executable signed BooleanProperty "
           "selector");
  }
  for (const int id : {161, 162, 163, 164}) {
    expect(catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                             .integerDomain =
                                 SkinIntegerPropertyDomain::IntegerValue},
                            SkinBuiltinPropertySelector{id}),
           "gameplay catalog admits each implemented play-time value");
    expect(catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                             .integerDomain =
                                 SkinIntegerPropertyDomain::ImageIndex},
                            SkinBuiltinPropertySelector{id}),
           "compatibility catalog accepts every integer cache selector as an "
           "image index too");
  }
  expect(catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                           .integerDomain =
                               SkinIntegerPropertyDomain::IntegerValue},
                          SkinBuiltinPropertySelector{106}) &&
             catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                               .integerDomain =
                                   SkinIntegerPropertyDomain::ImageIndex},
                              SkinBuiltinPropertySelector{308}) &&
             catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                               .integerDomain =
                                   SkinIntegerPropertyDomain::IntegerValue},
                              SkinBuiltinPropertySelector{500}),
         "catalog accepts every selector supplied through either integer "
         "factory cache");
  expect(catalog.contains({.kind = SkinBindingKind::FloatProperty,
                           .floatDomain = SkinFloatPropertyDomain::Rate},
                          SkinBuiltinPropertySelector{6}),
         "gameplay catalog admits implemented music progress Float 6");
  for (const int id : {16, 1003}) {
    expect(catalog.contains({.kind = SkinBindingKind::StringProperty},
                            SkinBuiltinPropertySelector{id}),
           "gameplay catalog admits every implemented pinned string state");
  }
  for (const int id : {360, 361, 362, 363, 364, 365, 368}) {
    expect(catalog.contains({.kind = SkinBindingKind::IntegerProperty,
                             .integerDomain =
                                 SkinIntegerPropertyDomain::IntegerValue},
                            SkinBuiltinPropertySelector{id}),
           "gameplay catalog admits each pinned chart-information number");
  }
}

void testReadyAndLiveTimersUseTheSharedSkinStateClock() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto beforeReady = stateAt(218);
  beforeReady.clock.visualTimeMicros = 1'499'999;
  beforeReady.sceneStartMicros = 1'500'000;
  bridge.beginFrame(beforeReady, projectionAt(218));
  expect(bridge.timerProperty({40}) == INT64_MIN,
         "TIMER_READY remains off until the selected skin-state origin");
  bridge.discardFrame();

  auto state = stateAt(219);
  state.clock.visualTimeMicros = 2'500'000;
  state.clock.gameplayTimeMicros = 2'400'000;
  state.sceneStartMicros = 1'500'000;
  state.lanes.front().pressMicros = 1'600'000;
  state.lanes.front().bombMicros = 1'700'000;
  state.lastJudgeVisualMicros = 1'800'000;
  state.clock.playTimer = {.active = true,
                           .startMicros = 1'800'000,
                           .elapsedMillisExact = true};
  bridge.beginFrame(state, projectionAt(219));
  expect(skinStateClockMicros(state) == 1'000'000 &&
             bridge.timerProperty({40}) == 0 &&
             bridge.timerProperty({100}) == 100'000 &&
             bridge.timerProperty({50}) == 200'000 &&
             bridge.timerProperty({46}) == 300'000 &&
             bridge.timerProperty({41}) == 400'000,
         "ready, lane, judgement, and play timers share the renderer's "
         "Beatoraja-style skin-state clock");
  bridge.discardFrame();
}

void testClearAndFullComboTimersFollowPinnedBmsPlayerState() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  chart.staticMetadata.totalNotes = 3;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  // BMSPlayer switches TIMER_ENDOFNOTE_1P only after its integer play clock
  // moves past (playtime - TIME_MARGIN).  At this frame the terminal chart
  // time is exactly 5,000 ms, so the clear phase is still off.
  auto beforeClear = stateAt(224);
  beforeClear.sceneStartMicros = 1'000'000;
  beforeClear.clock.visualTimeMicros = 6'000'000;
  beforeClear.clock.gameplayTimeMicros = 5'000'000;
  beforeClear.clock.playTimer = {.active = true,
                                 .startMicros = 0,
                                 .elapsedMillisExact = true,
                                 .playtimeMillis = 10'000};
  bridge.beginFrame(beforeClear, projectionAt(224));
  expect(bridge.timerProperty({143}) == kPlayfieldTimestampOff &&
             bridge.timerProperty({48}) == kPlayfieldTimestampOff,
         "clear and full-combo timers are off before their pinned conditions");
  bridge.discardFrame();

  auto clearedFullCombo = stateAt(225);
  clearedFullCombo.sceneStartMicros = 1'000'000;
  clearedFullCombo.clock.visualTimeMicros = 6'001'000;
  clearedFullCombo.clock.gameplayTimeMicros = 5'001'000;
  clearedFullCombo.clock.playTimer = {.active = true,
                                      .startMicros = 0,
                                      .elapsedMillisExact = true,
                                      .playtimeMillis = 10'000};
  clearedFullCombo.lastJudgeVisualMicros = 5'000'700;
  clearedFullCombo.authority.judgementCounters = {{PGreat, 3}};
  clearedFullCombo.authority.stagePassedNotes = 3;
  clearedFullCombo.authority.stageCombo = 3;
  bridge.beginFrame(clearedFullCombo, projectionAt(225));
  expect(bridge.timerProperty({143}) == 5'001'000,
         "end-of-notes clear timer starts on the first post-margin play frame");
  expect(bridge.timerProperty({48}) == 4'000'700,
         "full-combo timer starts at the final stage judgement on the skin clock");
  bridge.discardFrame();

  auto broken = clearedFullCombo;
  broken.clock.serial = 226;
  broken.authority.stageCombo = 0;
  bridge.beginFrame(broken, projectionAt(226));
  expect(bridge.timerProperty({143}) == 5'001'000 &&
             bridge.timerProperty({48}) == kPlayfieldTimestampOff,
         "clear remains latched while full-combo turns off when stage combo breaks");
  bridge.discardFrame();

  // Once the exact play clock moves past BMSPlayer's complete playtime, it
  // enters STATE_FINISHED and starts TIMER_MUSIC_END. The following frame
  // starts TIMER_FADEOUT because this fixture's authored finish margin is
  // the default zero milliseconds.
  auto musicEnded = broken;
  musicEnded.clock.serial = 227;
  musicEnded.clock.visualTimeMicros = 11'001'000;
  musicEnded.clock.gameplayTimeMicros = 10'001'000;
  bridge.beginFrame(musicEnded, projectionAt(227));
  expect(bridge.timerProperty({908}) == 10'001'000 &&
             bridge.timerProperty({2}) == kPlayfieldTimestampOff,
         "music-end starts after the exact complete playtime before fadeout");
  bridge.discardFrame();

  auto fadingOut = musicEnded;
  fadingOut.clock.serial = 228;
  fadingOut.clock.visualTimeMicros = 12'001'000;
  fadingOut.clock.gameplayTimeMicros = 11'001'000;
  bridge.beginFrame(fadingOut, projectionAt(228));
  expect(bridge.timerProperty({908}) == 10'001'000 &&
             bridge.timerProperty({2}) == 11'001'000,
         "authored zero finish margin starts the fadeout on the next frame");
  bridge.discardFrame();
}

void testLiveGameplayClockKeepsTheFinalNoteInStatePlayUntilPinnedDeadline() {
  // Pinned Beatoraja BMSPlayer keeps STATE_PLAY through lastNoteTime +
  // TIME_MARGIN, then changes state only when the integer play timer is
  // strictly greater than that deadline. The active Jukebox callback clock
  // remains the single source for both gameplay and skin presentation.
  expect(!beatorajaGameplayStateFinished(15'000'000, 15'000),
         "the exact BMSPlayer playtime remains in STATE_PLAY");
  expect(beatorajaGameplayStateFinished(15'001'000, 15'000),
         "the first post-playtime millisecond enters STATE_FINISHED");
}

void testPlayTimerVisualRebaseSaturatesWithoutLosingCancellation() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  const auto timer = [&](std::uint64_t serial, std::int64_t start,
                         std::int64_t visual, std::int64_t gameplay) {
    auto state = stateAt(serial);
    state.sceneStartMicros = 0;
    state.clock.visualTimeMicros = visual;
    state.clock.gameplayTimeMicros = gameplay;
    state.clock.playTimer = {
        .active = true, .startMicros = start, .elapsedMillisExact = true};
    bridge.beginFrame(state, projectionAt(serial));
    const auto result = bridge.timerProperty({41});
    bridge.discardFrame();
    return result;
  };

  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  expect(timer(220, -1, maximum, -1) == maximum,
         "play-timer rebasing retains negative cancellation at the upper bound");
  expect(timer(221, 1, minimum + 1, 1) == minimum + 1 &&
             timer(222, 0, minimum + 1, 1) == minimum + 1,
         "play-timer rebasing retains lower-bound cancellation and preserves "
         "the off sentinel");
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
                .fullArtist = "artist subartist",
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
      {3, {.x = 3}}, {4, {.x = 4}},
      {30, {.x = 30}}, {32, {.x = 32}}};
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});

  auto state = stateAt(101);
  state.playStartMicros = 6'001;
    state.clock.playTimer = {.active = true,
                             .startMicros = 6'001,
                             .elapsedMillisExact = true};
  state.lastJudgeVisualMicros = 6'002;
  state.fastSlowMicros = -34'000;
  state.authority.liftEnabled = false;
  state.authority.liftRatio = 0.3759F;
  state.authority.hiddenEnabled = false;
  state.authority.hiddenRatio = 0.2867F;
  state.authority.currentGauge = 62.3F;
  state.authority.maximumCombo = 321;
  // ScoreDataProperty.update receives JudgeManager.getPastNotes(); this
  // captured stage value is independent from the optional pacemaker snapshot.
  state.authority.stagePassedNotes = 200;
  state.authority.bestScore = 300;
  state.authority.bestScoreTarget = {.enabled = true,
                                     .label = "BEST",
                                     .finalScore = 300,
                                     .totalNotes = 417};
  state.authority.pacemakerTarget = {.enabled = true, .finalScore = 500};
  state.authority.pacemakerStatus = {.enabled = true,
                                     .currentScore = 456,
                                     .targetScore = 240,
                                     .finalTargetScore = 500,
                                     .maxScore = 834,
                                     .playedNotes = 200,
                                     .totalNotes = 417};
  state.authority.judgementCounters = {
      {PGreat, 10}, {Great, 9}, {Good, 8},
      {Bad, 7},    {Kpoor, 6}, {Poor, 5}};
  state.authority.judgementFastSlowCounters = {
      {PGreat, {.fast = 4, .slow = 3}}, {Great, {.fast = 2, .slow = 1}},
      {Good, {.fast = 6, .slow = 5}},   {Bad, {.fast = 8, .slow = 7}},
      {Kpoor, {.fast = 10, .slow = 9}}, {Poor, {.fast = 12, .slow = 11}}};
  state.authority.loadingState = PlayfieldLoadingState::Loaded;
  state.authority.stageFileAvailable = true;
  state.authority.backBmpAvailable = true;
  state.authority.tableName = "table";
  state.authority.tableLevel = "level";
  state.authority.tableFullName = "leveltable";
  state.configuration.visibleTimeDurationMilliseconds = 667;
  state.lanes.resize(8);
  for (std::size_t index = 0; index < state.lanes.size(); ++index) {
    state.lanes[index].pressed = true;
    state.lanes[index].pressMicros = 1'000 + static_cast<long long>(index);
    state.lanes[index].releaseMicros =
        2'000 + static_cast<long long>(index);
    state.lanes[index].bombMicros = 3'000 + static_cast<long long>(index);
    state.lanes[index].beatorajaJudgeValue =
        2 + static_cast<int>(index);
  }
  bridge.beginFrame(state, projectionAt(101));

  expect(bridge.booleanProperty({32}).supported &&
             bridge.booleanProperty({32}).value &&
             bridge.booleanProperty({33}).supported &&
             !bridge.booleanProperty({33}).value,
         "autoplay options expose AsoBMaShow's explicit non-autoplay state");
  expect(bridge.booleanProperty({43}).supported &&
             !bridge.booleanProperty({43}).value,
         "gauge-hard option reads the authoritative gauge type");
  for (const auto [id, expected] : std::array{
           std::pair{271, false}, std::pair{272, false},
           std::pair{273, false}}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && value.value == expected,
           "lane-cover family enabled option reads its exact authority flag");
  }
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
           // LITONE12's Ghost target display uses NUMBER_DIFF_EXSCORE.
           // IntegerPropertyFactory maps 108, 128, and 153 to the live score
           // delta against ScoreDataProperty's projected pacemaker target.
           // This must stay distinct from 121/151, which expose the target's
           // final score.
           std::pair{108, 216LL}, std::pair{128, 216LL},
           std::pair{101, 456LL}, std::pair{107, 62LL},
           std::pair{110, 10LL}, std::pair{111, 9LL},
           std::pair{112, 8LL}, std::pair{113, 7LL},
           std::pair{114, 5LL}, std::pair{171, 456LL},
           std::pair{314, 375LL}, std::pair{315, 286LL},
           std::pair{316, 280LL},
           std::pair{407, 3LL}, std::pair{427, 18LL},
           // Judge.Diff is negative for an early input, whereas Beatoraja's
           // JudgeManager.getRecentJudgeTiming() is positive for that same
           // input.  IntegerPropertyFactory.ValueType 525 exposes the latter.
           std::pair{525, 34LL}, std::pair{75, 321LL},
           std::pair{102, 114LL}, std::pair{103, 0LL},
           // NUMBER_DIFF_HIGHSCORE compares with ScoreDataProperty's
           // projected current best score, rather than its final score.
           // NUMBER_HIGHSCORE instead exposes the persisted final best score.
           std::pair{105, 321LL}, std::pair{150, 300LL},
           std::pair{152, 428LL},
           std::pair{153, 216LL}, std::pair{313, 417LL},
           std::pair{410, 4LL}, std::pair{411, 3LL},
           std::pair{412, 2LL}, std::pair{413, 1LL},
           std::pair{414, 6LL}, std::pair{415, 5LL},
           std::pair{416, 8LL}, std::pair{417, 7LL},
           std::pair{418, 12LL}, std::pair{419, 11LL},
           std::pair{420, 6LL}, std::pair{421, 10LL},
           std::pair{422, 9LL}, std::pair{425, 0LL},
           std::pair{526, 0LL}, std::pair{527, 0LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "selected live score, gauge, judgement, and lift number is exact: " +
               std::to_string(id));
  }
  const auto currentBestRate =
      bridge.floatProperty({112}, SkinFloatPropertyDomain::Rate);
  expect(currentBestRate.supported &&
             std::abs(currentBestRate.value - 28.0 / 834.0) < 0.000001,
         "current-best rate uses the same passed-note projection as "
         "ScoreDataProperty.getNowBestScore");
  for (const auto [id, expected] :
       std::array{std::pair{500, 2LL}, std::pair{501, 3LL},
                  std::pair{507, 9LL}, std::pair{510, -1LL}}) {
    const auto value = bridge.integerProperty(
        {id}, SkinIntegerPropertyDomain::ImageIndex);
    expect(value.supported && value.value == expected,
           "selected image-index judgement value is exact: " +
               std::to_string(id));
  }
  chart.staticMetadata.hasAnyLongNote = true;
  chart.staticMetadata.hasUndefinedLongNote = false;
  chart.staticMetadata.hasHellChargeNote = true;
  const auto lnMode = bridge.integerProperty(
      {308}, SkinIntegerPropertyDomain::ImageIndex);
  expect(lnMode.supported && lnMode.value == 2,
         "ImageIndex 308 returns Beatoraja's HCN long-note mode");
  for (const auto [id, expected] : std::array{
           std::pair{102, 1.0},
           std::pair{110, 456.0 / 834.0},
           std::pair{111, 456.0 / 400.0},
           std::pair{112, 28.0 / 834.0},
           std::pair{113, 300.0 / 834.0},
           std::pair{114, 240.0 / 834.0},
           std::pair{115, 500.0 / 834.0}}) {
    const auto value = bridge.floatProperty({id});
    expect(value.supported && std::abs(value.value - expected) < 0.000001,
           "selected pinned score and loading Float is exact");
  }
  const auto floatValue = bridge.floatProperty(
      {1102}, SkinFloatPropertyDomain::FloatValue);
  expect(floatValue.supported &&
             std::abs(floatValue.value - 456.0 / 400.0) < 0.000001,
         "FloatType.score_rate uses getFloatProperty rather than RateType");
  const auto namedFloatValue = bridge.floatProperty(
      {std::string{"score_rate"}}, SkinFloatPropertyDomain::FloatValue);
  expect(namedFloatValue.supported &&
             std::abs(namedFloatValue.value - floatValue.value) < 0.000001,
         "named FloatProperty resolves through the same pinned factory");
  const auto nonSelectJudgeRate =
      bridge.floatProperty({std::string{"rate_pgreat"}});
  expect(nonSelectJudgeRate.supported && nonSelectJudgeRate.value == 0.0,
         "RateType's selection-only judge rate remains its exact gameplay "
         "zero fallback");
  const double floatMinimum =
      static_cast<double>(std::numeric_limits<float>::min());
  for (const std::string_view selector :
       {"duration_average", "timing_average", "timign_stddev",
        "ir_player_failed_rate"}) {
    const auto value = bridge.floatProperty(
        {std::string(selector)}, SkinFloatPropertyDomain::FloatValue);
    expect(value.supported && value.value == floatMinimum,
           "FloatPropertyFactory's non-gameplay value keeps its exact "
           "Float.MIN_VALUE fallback");
  }
  for (const auto [id, expected] : std::array{
           std::pair{74, 417LL}, std::pair{106, 417LL}, std::pair{90, 240LL},
           std::pair{91, 120LL}, std::pair{92, 178LL},
           std::pair{96, 12LL}, std::pair{350, 321LL},
           std::pair{351, 54LL}, std::pair{352, 32LL},
           std::pair{353, 10LL},
           std::pair{1163, 2LL}, std::pair{1164, 5LL}}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == expected,
           "selected static chart number uses its exact immutable source");
  }
  const auto densityDiagnosticCount = bridge.diagnostics().size();
  for (const int id : {360, 361, 362, 363, 364, 365, 368}) {
    const auto value = bridge.integerProperty({id});
    expect(value.supported && value.value == INT32_MIN,
           "absent Beatoraja SongInformation uses its Integer.MIN_VALUE "
           "sentinel: " + std::to_string(id));
  }
  expect(bridge.diagnostics().size() == densityDiagnosticCount,
         "recognized density properties do not become app-specific unsupported "
         "state errors");
  expect(bridge.stringProperty({12}).supported &&
             bridge.stringProperty({12}).value == "full title" &&
             bridge.stringProperty({13}).value == "genre" &&
             bridge.stringProperty({14}).value == "artist" &&
             bridge.stringProperty({15}).value == "subartist" &&
             bridge.stringProperty({16}).value == "artist subartist" &&
             bridge.stringProperty({1003}).value == "leveltable" &&
             bridge.stringProperty({std::string{"fullartist"}}).value ==
                 "artist subartist" &&
             bridge.stringProperty({std::string{"tablefull"}}).value ==
                 "leveltable",
         "pinned full artist and table text use their authoritative states");
  const auto practiceText =
      bridge.stringProperty({std::string{"practice_item1"}});
  expect(practiceText.supported && practiceText.value.empty(),
         "non-gameplay pinned StringProperty values retain Beatoraja's empty "
         "fallback");
  for (const int id : {3, 4, 30, 32}) {
    const auto offset = bridge.offsetProperty(id);
    expect(offset.supported && offset.value.x == id,
           "configured selected offsets are returned by their exact IDs");
  }
  for (const int id : {1, 2, 199}) {
    const auto offset = bridge.offsetProperty(id);
    expect(offset.supported && offset.value.x == 0.0 && offset.value.y == 0.0 &&
               offset.value.w == 0.0 && offset.value.h == 0.0 &&
               offset.value.r == 0.0 && offset.value.a == 0.0,
           "every unconfigured pinned SkinOffset ID returns Beatoraja's "
           "zero-initialized offset");
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
           std::pair{120, kPlayfieldTimestampOff},
           std::pair{121, kPlayfieldTimestampOff},
           std::pair{122, kPlayfieldTimestampOff},
           std::pair{123, kPlayfieldTimestampOff},
           std::pair{124, kPlayfieldTimestampOff},
           std::pair{125, kPlayfieldTimestampOff},
           std::pair{126, kPlayfieldTimestampOff},
           std::pair{127, kPlayfieldTimestampOff}}) {
    expect(bridge.timerProperty({id}) == expected,
           "selected live timer reads its authoritative presentation clock");
  }
  const auto diagnosticCount = bridge.diagnostics().size();
  for (const int id : {2, 3, 11, 40, 42, 44, 48, 70, 71, 72, 73, 74,
                       75, 76, 77, 143, 172, 173}) {
    expect(bridge.timerProperty({id}) == INT64_MIN,
           "selected timer without an authoritative source is off");
  }
  expect(bridge.diagnostics().size() == diagnosticCount,
         "selected off timers do not report unsupported diagnostics");
  expect(bridge.floatProperty({4}).supported &&
             bridge.floatProperty({4}).value == 0.0,
         "disabled lane cover reports zero despite a retained amount");
  expect(bridge.floatProperty({5}).supported &&
             bridge.floatProperty({5}).value == bridge.floatProperty({4}).value,
         "second lane-cover rate matches the disabled primary rate");
  expect(bridge.floatProperty({"lanecover2"}).supported &&
             bridge.floatProperty({"lanecover2"}).value ==
                 bridge.floatProperty({4}).value,
         "second lane-cover rate alias matches the disabled primary rate");
  bridge.discardFrame();

  state = stateAt(102);
  state.authority.laneCoverEnabled = true;
  state.authority.laneCoverPercent = 0;
  state.authority.stageFileAvailable = false;
  state.authority.backBmpAvailable = false;
  state.lastJudge = JudgeResult(Great, 20);
  state.fastSlowMicros = -20;
  bridge.beginFrame(state, projectionAt(102));
  expect(bridge.booleanProperty({190}).supported &&
             bridge.booleanProperty({190}).value &&
             bridge.booleanProperty({191}).supported &&
             !bridge.booleanProperty({191}).value &&
             bridge.booleanProperty({194}).supported &&
             bridge.booleanProperty({194}).value &&
             bridge.booleanProperty({195}).supported &&
             !bridge.booleanProperty({195}).value,
         "stagefile and backbmp options require a decoded gameplay resource, "
         "not merely a declared chart path");
  expect(bridge.floatProperty({4}).supported &&
             bridge.floatProperty({4}).value == 0.0,
         "enabled zero lane cover remains zero");
  expect(bridge.floatProperty({5}).supported &&
             bridge.floatProperty({5}).value == bridge.floatProperty({4}).value,
         "second lane-cover rate matches the enabled zero primary rate");
  expect(bridge.booleanProperty({1242}).supported &&
             bridge.booleanProperty({1242}).value &&
             bridge.booleanProperty({1243}).supported &&
             !bridge.booleanProperty({1243}).value,
         "negative Aso timing selects Beatoraja's positive early option");
  bridge.discardFrame();

  state = stateAt(103);
  state.authority.laneCoverEnabled = true;
  state.authority.liftEnabled = true;
  state.authority.liftRatio = 0.2F;
  state.authority.hiddenEnabled = true;
  state.lastJudge = JudgeResult(Great, -20);
  state.fastSlowMicros = 20;
  bridge.beginFrame(state, projectionAt(103));
  expect(bridge.floatProperty({4}).supported &&
             std::abs(bridge.floatProperty({4}).value - 0.36) < 0.000001,
         "enabled lift scales the lane-cover amount by one minus lift");
  expect(bridge.floatProperty({5}).supported &&
             bridge.floatProperty({5}).value == bridge.floatProperty({4}).value,
         "second lane-cover rate exactly matches the lifted primary rate");
  for (const auto [id, expected] : std::array{
           std::pair{271, true}, std::pair{272, true},
           std::pair{273, true}}) {
    const auto value = bridge.booleanProperty({id});
    expect(value.supported && value.value == expected,
           "lane-cover family enabled option tracks the active authority flag");
  }
  expect(bridge.booleanProperty({1242}).supported &&
             !bridge.booleanProperty({1242}).value &&
             bridge.booleanProperty({1243}).supported &&
             bridge.booleanProperty({1243}).value,
         "positive Aso timing selects Beatoraja's negative late option");
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
                                 .model = &model,
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
                              .model = &model,
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
                              .model = &model,
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
                              .model = &model,
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
                              .model = &model,
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
                              .model = &model,
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
      {.id = SkinFloatWriterId{8},
       .source = SkinBuiltinPropertySelector{.value = 17}},
      {.id = SkinFloatWriterId{9},
       .source = SkinBuiltinPropertySelector{.value = 18}},
      {.id = SkinFloatWriterId{10},
       .source = SkinBuiltinPropertySelector{.value = 19}},
      {.id = SkinFloatWriterId{11},
       .source = SkinBuiltinPropertySelector{.value = 20}},
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
                              .model = &model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  auto state = stateAt(121);
  state.authority.practiceMenu = practice::SkinMenuState{};
  bridge.beginFrame(state, projectionAt(121));
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
  for (const auto [writer, input] :
       std::array{std::pair{SkinFloatWriterId{8}, -0.5},
                  std::pair{SkinFloatWriterId{9}, 0.5},
                  std::pair{SkinFloatWriterId{10}, 1.5}}) {
    const auto result = bridge.invokeWriter(writer, input);
    expect(result.status == SkinHostCallStatus::Completed,
           "pinned audio writer is staged as a built-in gameplay mutation");
  }
  expect(bridge.invokeWriter(SkinFloatWriterId{11}, 0.5).status ==
             SkinHostCallStatus::Completed,
         "practice-position writer stages the source viewport mutation");

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
             committed.orderedMutations.size() == 7,
         "failed callbacks roll back only their savepoint before one commit");
  const std::array expectedAudioWrites{
      SetSkinAudioVolume{.target = SkinAudioVolumeWriterTarget::Master,
                         .value = 0.0F},
      SetSkinAudioVolume{.target = SkinAudioVolumeWriterTarget::Keysound,
                         .value = 0.5F},
      SetSkinAudioVolume{.target = SkinAudioVolumeWriterTarget::Bgm,
                         .value = 1.0F},
  };
  for (std::size_t index = 0; index < expectedAudioWrites.size(); ++index) {
    const auto *audio = std::get_if<SetSkinAudioVolume>(
        &committed.orderedMutations[index]);
    expect(audio != nullptr && audio->target == expectedAudioWrites[index].target &&
               audio->value == expectedAudioWrites[index].value,
           "successful built-in audio writers preserve pinned target and "
           "clamped value order");
  }
  const auto *practiceScroll = std::get_if<SetPracticeItemScroll>(
      &committed.orderedMutations[expectedAudioWrites.size()]);
  expect(practiceScroll != nullptr && practiceScroll->position == 0.5F,
         "practice-position preserves its normalized authored value");
  const std::array<int, 3> expectedArguments{0, 100, 25};
  for (std::size_t index = 0; index < expectedArguments.size(); ++index) {
    const auto *presentation = std::get_if<SessionPresentationWrite>(
        &committed.orderedMutations[index + expectedAudioWrites.size() + 1]);
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
  testDurationBindingsUsePinnedLaneRendererFormula();
  testZeroHispeedDurationBindingsFollowJavaCurrentDuration();
  testDurationBindingsUseFrameLocalSpeedObjectMultiplier();
  testHispeedBindingsUsePinnedIntegerAndFloatProperties();
  testIntegerPropertyFactoryDomainNeverRejectsGameplaySkins();
  testMarkProcessedNoteImageIndexTracksPlayerConfiguration();
  testBridgeOwnsSnapshotAndClosesEachFrameExactlyOnce();
  testFramePropertiesUseAuthoritativeGaugeAndTimerRules();
  testGameplayModeAndLoadingBooleanProperties();
  testExistingGameplayStatePropertyWiring();
  testPracticeMenuSelectorsAndEventsRequireCapturedMenuState();
  testLiftHiddenOffsetsFollowPinnedLaneRenderer();
  testRemainingDirectGameplayStatePropertyWiring();
  testLongNoteHoldTimersUseCapturedLaneState();
  testExtendedPlayerOneLaneTimersUsePinnedSkinOffsets();
  testPomyuTimersFollowPinnedDefaultProcessorCycles();
  testPomyuTimersUseAuthoredMotionCycles();
  testIrProviderStringUsesCapturedProfileConfiguration();
  testIrAccountStringUsesCapturedConnectedAccount();
  testSongInformationPropertiesUseImmutableSourceAnalysis();
  testPersistedScorePropertiesUseScoreDataRatherThanLiveJudgements();
  testRivalScorePropertiesRequireCapturedTargetScoreData();
  testWallClockPropertiesUseTheLocalCalendar();
  testRuntimeFpsAndUptimePropertiesUseCapturedApplicationAuthority();
  testStartInputTimerUsesPinnedSkinTiming();
  testFailureTimerUsesCapturedSurvivalFailureEvent();
  testRhythmTimerUsesPinnedSectionAndBpmAccumulator();
  testFavoriteChartImageIndexUsesCapturedRepositoryState();
  testSongReviewImageIndexesUsePinnedBitmaskStates();
  testDifficultyTableStringsUseCapturedSelectionContext();
  testPlayerConfigurationStringsUseCapturedSourceValues();
  testConfiguredTargetNameNeighborsFollowPinnedTargetRing();
  testTargetScoreStringsFollowPinnedTargetSource();
  testChartDocumentBooleansUseCapturedLibraryMetadata();
  testScoreAndComboTimersUseCapturedGameplayState();
  testPlayTimerPropertiesMatchPinnedJavaConversions();
  testReadyAndLiveTimersUseTheSharedSkinStateClock();
  testClearAndFullComboTimersFollowPinnedBmsPlayerState();
  testLiveGameplayClockKeepsTheFinalNoteInStatePlayUntilPinnedDeadline();
  testPlayTimerVisualRebaseSaturatesWithoutLosingCancellation();
  testSelectedScuroMappingsUseOnlyAuthoritativeState();
  testEmptyCustomObjectsStayZeroCost();
  testCustomTimersPrecedeAutomaticEventsInAuthoredOrder();
  testCustomEventsAcceptManualAritiesAndRollbackCriticalFrames();
  testAutomaticCustomEventUsesTheCapturedFrameClockForMinimumInterval();
  testCustomObjectBudgetStopsLaterEventsAndRollsBackWrites();
  testFloatWritersResolveLocallyAndRollbackCallbackMutations();
  return failures == 0 ? 0 : 1;
}
