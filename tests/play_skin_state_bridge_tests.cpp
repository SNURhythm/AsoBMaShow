#include "skin/beatoraja/PlaySkinStateBridge.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
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
if not skin_config then return {type=0} end
return {type=0, probe_timer=function() return main_state.timer(41) end}
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
    header.value.reset();
    auto configured = runtime_->loadConfigured({});
    expect(configured.value.has_value(), "runtime configured phase executes");
    if (configured.value) {
      probeTimer_ = configured.value->callbackNamed("probe_timer");
    }
    expect(probeTimer_.has_value(), "runtime bridge callback is retained");
    configured.value.reset();
    expect(runtime_->enterRenderPhase().ok, "runtime enters render phase");
  }

  [[nodiscard]] bool ready() const noexcept { return runtime_ != nullptr; }
  LuaSkinRuntime &runtime() { return *runtime_; }
  [[nodiscard]] LuaCallbackId probeTimer() const { return *probeTimer_; }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> probeTimer_;
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

void testCustomObjectsRemainExplicitlyPendingSharedFrameOwnership() {
  RuntimeHarness runtime;
  if (!runtime.ready()) {
    return;
  }
  PlayfieldChartVisualModel chart;
  ValidatedBeatorajaSkinModel model;
  model.model.customTimers = {{.id = 10'001, .timer = std::nullopt}};
  model.model.customEvents = {{.id = 1'001,
                               .action = SkinEventBindingId{1},
                               .condition = std::nullopt}};
  BeatorajaSkinConfiguration configuration;
  const auto mutations = makePinnedSkinEventMutationTableV1();
  PlaySkinStateBridge bridge({.chartModel = chart,
                              .model = model,
                              .configuration = configuration,
                              .runtime = runtime.runtime(),
                              .mutationTable = mutations});
  bridge.beginFrame(stateAt(91), projectionAt(91));
  const auto update = bridge.updateCustomObjects();
  expect(
      update.status == SkinHostCallStatus::Unsupported &&
          update.callbacksInvoked == 0 &&
          hasDiagnostic(bridge, "skin.play_state.custom_objects_unavailable"),
      "custom maps never claim false support while renderer owns Lua frame "
      "begin");
  expect(bridge.executeEvent(1'001, {}).status ==
             SkinHostCallStatus::Unsupported,
         "manual custom events stay pending under the same frame-owner gap");
  (void)bridge.commitFrame();
}

} // namespace

int main() {
  testPinnedMutationTableMatchesFrozenFixtureExhaustively();
  testBridgeOwnsSnapshotAndClosesEachFrameExactlyOnce();
  testFramePropertiesUseAuthoritativeGaugeAndTimerRules();
  testCustomObjectsRemainExplicitlyPendingSharedFrameOwnership();
  return failures == 0 ? 0 : 1;
}
