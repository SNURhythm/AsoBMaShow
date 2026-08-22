#include "scene/play/PlayfieldChartVisualModel.h"
#include "rendering/common.h"
#include "scene/play/PlayfieldProjection.h"
#include "scene/play/PlayfieldVisualState.h"
#include "skin/SkinConfigurationWriteQueue.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#if __has_include("skin/beatoraja/GameplaySkinSourceFormat.h")
#include "skin/beatoraja/GameplaySkinSourceFormat.h"
#define ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS 1
#else
#define ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS 0
#endif
#include "skin/beatoraja/PlaySkinSession.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rendering {
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

// ASOBMASHOW_LOADING_WORKLOAD_V2: isolated Lua/JSON/LR2 session creation.

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

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-skin-loading-benchmark-" +
               std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }
  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class BenchmarkTextureDevice final : public SkinTextureDevice {
public:
  bgfx::TextureHandle
  create(const image_decode::DecodedImageData &) override {
    ++created;
    return bgfx::TextureHandle{next_++};
  }
  void destroy(bgfx::TextureHandle) noexcept override { ++destroyed; }
  bool ownsCurrentThread() const noexcept override { return true; }

  std::size_t created = 0;
  std::size_t destroyed = 0;

private:
  std::uint16_t next_ = 1;
};

class BenchmarkMovieDevice final : public SkinMovieDevice {
public:
  std::optional<SkinMovieLoadResult>
  load(const fs::path &path, const SkinMovieLoadLimits &limits,
       std::stop_token stop) override {
    if (stop.stop_requested() || !fs::is_regular_file(path)) {
      materializedPathsValid = false;
      return std::nullopt;
    }
    const auto layout = skinMovieDecodedLayout(80, 40, limits);
    if (!layout) return std::nullopt;
    ++loaded;
    return SkinMovieLoadResult{
        .handle = SkinMoviePlayerHandle{loaded},
        .width = 80,
        .height = 40,
        .durationMillis = 1'000,
        .decodedBytes = layout->residentBytes};
  }

  void destroy(SkinMoviePlayerHandle) noexcept override { ++destroyed; }
  bool ownsCurrentThread() const noexcept override { return true; }
  void beginFrame() noexcept override {}
  SkinMovieFramePreparationResult
  prepareFrame(SkinMoviePlayerHandle, const SkinMovieCommand &,
               const PlaySkinViewport &) override {
    return {.ready = true, .drawable = false};
  }
  void discardFrame() noexcept override {}
  void commitFrame() noexcept override {}
  void submitPrepared(std::size_t) noexcept override {}

  std::uint64_t loaded = 0;
  std::uint64_t destroyed = 0;
  bool materializedPathsValid = true;
};

struct BenchmarkOptions {
  bool benchmark = false;
  bool acceptanceReport = false;
  bool acceptanceMatrix = false;
  bool warm = false;
  std::size_t samples = 1;
  std::optional<fs::path> skin;
  std::optional<std::string> entry;
  std::optional<std::string> entryIdentity;
  std::optional<GameplaySkinSourceFormat> format;
};

std::optional<BenchmarkOptions> parseOptions(int argc, char **argv) {
  BenchmarkOptions result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--benchmark") {
      result.benchmark = true;
    } else if (argument == "--acceptance-report") {
      result.acceptanceReport = true;
    } else if (argument == "--acceptance-matrix") {
      result.acceptanceMatrix = true;
    } else if (argument == "--mode" && index + 1 < argc) {
      const std::string_view mode(argv[++index]);
      if (mode != "cold" && mode != "warm") return std::nullopt;
      result.warm = mode == "warm";
    } else if (argument == "--samples" && index + 1 < argc) {
      try {
        result.samples = std::stoull(argv[++index]);
      } catch (...) {
        return std::nullopt;
      }
      if (result.samples == 0 || result.samples > 100) return std::nullopt;
    } else if (argument == "--skin" && index + 1 < argc) {
      result.skin = fs::path(argv[++index]);
    } else if (argument == "--entry" && index + 1 < argc) {
      result.entry = argv[++index];
    } else if (argument == "--entry-identity" && index + 1 < argc) {
      result.entryIdentity = argv[++index];
    } else if (argument == "--format" && index + 1 < argc) {
      const std::string_view format(argv[++index]);
      if (format == "lua") {
        result.format = GameplaySkinSourceFormat::Lua;
      } else if (format == "json") {
        result.format = GameplaySkinSourceFormat::Json;
      } else if (format == "lr2") {
        result.format = GameplaySkinSourceFormat::Lr2;
      } else {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if (static_cast<int>(result.benchmark) +
          static_cast<int>(result.acceptanceReport) +
          static_cast<int>(result.acceptanceMatrix) >
      1) {
    return std::nullopt;
  }
  if (result.acceptanceReport &&
      (!result.skin || !result.entry || !result.entryIdentity)) {
    return std::nullopt;
  }
  if (result.acceptanceMatrix && !result.skin) return std::nullopt;
  return result;
}

class BenchmarkFixture final {
public:
  BenchmarkFixture(const std::optional<fs::path> &localSkin,
                   std::optional<GameplaySkinSourceFormat> format,
                   std::optional<std::string> selectedEntry = std::nullopt)
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("LoadingBenchmark").package),
        profile_(*makeSkinProfileId(
            "99999999-9999-4999-8999-999999999999")) {
    chart_.keyCount = 7;
    chart_.text.title = "loading benchmark";
    chart_.text.artist = "AsoBMaShow tests";
    chart_.text.fullArtist = chart_.text.artist;
    chart_.text.auditedStringProperties = {{12, chart_.text.title}};
    initialState_.clock.serial = 1;
    initialState_.clock.visualTimeMicros = 0;
    initialState_.clock.gameplayTimeMicros = 0;
    initialState_.authority.loadingState = PlayfieldLoadingState::Loaded;
    initialState_.authority.currentGauge = 50.0F;
    initialState_.authority.gaugeType = GaugeType::Normal;
    initialState_.authority.gaugeRules.compiled = true;
    initialState_.authority.gaugeRules
        .gauges[gaugeTypeIndex(GaugeType::Normal)] = {
        .initial = 20.0F,
        .minimum = 2.0F,
        .maximum = 100.0F,
        .clearBorder = 80.0F};
    initialState_.lastJudge = JudgeResult(PGreat, 0);
    initialState_.lastJudgeVisualMicros = 0;
    initialState_.combo = 1;
    initialProjection_.frameSerial = 1;
    prepareAcceptanceState();

    fs::path source = temp_.root() / "source";
    if (localSkin) {
      if (!prepareLocalSkin(*localSkin, source)) return;
    } else {
      prepareSyntheticFormats(source);
    }
    if (format) {
      std::erase_if(entryPaths_, [&](const std::string &relative) {
        return gameplaySkinSourceFormatForPath(relative) != format;
      });
    }
    if (selectedEntry) entryPaths_ = {std::move(*selectedEntry)};
    fs::create_directories(roots_.visiblePackages);
    fs::copy(source, roots_.visiblePackages / package_.directoryName,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    if (!snapshot.prepared) return;
    std::string error;
    lease_ = std::move(*snapshot.prepared).publish(error);
    if (!lease_ || !error.empty()) {
      lease_.reset();
      return;
    }
    for (const auto &relative : entryPaths_) {
      const auto normalized = normalizeEntryPath(package_, relative).entry;
      if (!normalized) continue;
      GameplaySkinValidator validator(validationResources_);
      auto validated = validator.validate(lease_->readView(), *normalized,
                                          nullptr, {});
      if (validated.disposition !=
              SkinValidationDisposition::SelectableGameplay ||
          !validated.reconciledSettings ||
          validated.configurationDigest.empty()) {
        continue;
      }
      entries_.push_back({.entry = *normalized,
                          .relativePath = relative,
                          .settings = *validated.reconciledSettings,
                          .digest = std::move(validated.configurationDigest)});
    }
  }

  bool ready() const noexcept {
    return lease_.has_value() && !entries_.empty();
  }

  std::optional<std::uint64_t>
  runSample(SkinResourcePreparationService &preparation,
            std::uint64_t &sessionSerial) {
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t total = 0;
    for (const auto &entry : entries_) {
      auto device = std::make_shared<BenchmarkTextureDevice>();
      auto movieDevice = std::make_shared<BenchmarkMovieDevice>();
      auto counters = std::make_shared<SkinLiveResourceCounters>();
      auto created = PlaySkinSession::create(
          {.revision = lease_->clone(),
           .entry = entry.entry,
           .reconciledSettings = entry.settings,
           .configurationDigest = entry.digest},
          {.sessionSerial = ++sessionSerial,
           .profileId = profile_,
           .chartModel = chart_,
           .initialState = &initialState_,
           .initialProjection = &initialProjection_,
           .safeUiBounds = {.x = 0.0,
                            .y = 0.0,
                            .width = 1280.0,
                            .height = 720.0},
           .storageRoots = roots_,
           .resourcePreparation = preparation,
           .textureDevice = device,
           .movieDevice = movieDevice,
           .liveResourceCounters = counters,
           .configurationWrites = configurationWrites_});
      if (!created.session) {
        return std::nullopt;
      }
#if defined(ASOBMASHOW_SKIN_LOADING_TELEMETRY)
      if (!isCompleteSkinLoadingTelemetry(created.loadingTelemetry)) {
        return std::nullopt;
      }
#endif
      created.session.reset();
      if (device->created != device->destroyed ||
          !movieDevice->materializedPathsValid ||
          movieDevice->loaded != movieDevice->destroyed ||
          counters->snapshot() != SkinLiveResourceSnapshot{}) {
        return std::nullopt;
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    total = elapsed.count() <= 0 ? 0U
                                 : static_cast<std::uint64_t>(elapsed.count());
    return total;
  }

  std::optional<nlohmann::json>
  acceptanceReport(std::string_view entryIdentity) {
    if (!ready() || entries_.size() != 1 || entryIdentity.empty()) {
      return std::nullopt;
    }
    constexpr std::array<std::string_view, 4> familyNames{
        "noteDistribution", "timingVisualizer", "bpmGraph",
        "hitErrorVisualizer"};
    std::map<std::string, std::size_t, std::less<>> declared;
    std::map<std::string, std::size_t, std::less<>> commands;
    for (const auto family : familyNames) {
      declared.emplace(family, 0U);
      commands.emplace(family, 0U);
    }
    std::set<std::string, std::less<>> diagnosticCodes;
    std::set<std::string, std::less<>> unsupportedDiagnostics;
    std::set<std::string, std::less<>> unsupportedSubjects;
    std::set<std::string, std::less<>> budgetDiagnostics;
    const auto rememberDiagnostic = [&](const SkinDiagnostic &diagnostic) {
      diagnosticCodes.insert(diagnostic.code);
      if (diagnostic.code.find("unsupported") != std::string::npos) {
        unsupportedDiagnostics.insert(diagnostic.code);
      }
      if (diagnostic.code == "skin.play_state.unsupported" &&
          diagnostic.virtualPath.find('/') == std::string::npos &&
          diagnostic.virtualPath.find('\\') == std::string::npos) {
        unsupportedSubjects.insert(diagnostic.virtualPath);
      }
      if (diagnostic.code == "skin_lua_instruction_limit_exceeded" ||
          diagnostic.code == "skin_lua_wall_time_limit_exceeded" ||
          diagnostic.code == "skin_lua_frame_budget_exceeded") {
        budgetDiagnostics.insert(diagnostic.code);
      }
    };

    SkinResourcePreparationService preparation;
    auto device = std::make_shared<BenchmarkTextureDevice>();
    auto movieDevice = std::make_shared<BenchmarkMovieDevice>();
    auto counters = std::make_shared<SkinLiveResourceCounters>();
    const auto started = std::chrono::steady_clock::now();
    auto created = PlaySkinSession::create(
        {.revision = lease_->clone(),
         .entry = entries_.front().entry,
         .reconciledSettings = entries_.front().settings,
         .configurationDigest = entries_.front().digest},
        {.sessionSerial = 700,
         .profileId = profile_,
         .chartModel = chart_,
         .initialState = &initialState_,
         .initialProjection = &initialProjection_,
         .safeUiBounds = {.x = 0.0,
                          .y = 0.0,
                          .width = 1280.0,
                          .height = 720.0},
         .storageRoots = roots_,
         .resourcePreparation = preparation,
         .textureDevice = device,
         .movieDevice = movieDevice,
         .liveResourceCounters = counters,
         .configurationWrites = configurationWrites_});
    const auto measuredLoadMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
    for (const auto &diagnostic : created.diagnostics) {
      rememberDiagnostic(diagnostic);
    }

    nlohmann::json report = {
        {"schemaVersion", 1},
        {"entryIdentity", entryIdentity},
        {"sessionPublished", created.session != nullptr},
        {"graphFamilies", nlohmann::json::object()},
        {"resourcePreparation",
         {{"complete", isCompleteSkinLoadingTelemetry(created.loadingTelemetry)},
          {"allReferencedResourcesPrepared", false},
          {"imageDecodes", created.loadingTelemetry.resources.imageDecodes},
          {"fontDecodes", created.loadingTelemetry.resources.fontDecodes},
          {"movieDecodes", created.loadingTelemetry.resources.movieDecodes},
          {"audioDecodes", created.loadingTelemetry.resources.audioDecodes},
          {"textureUploads",
           created.loadingTelemetry.resources.textureUploads}}},
        {"callbackBudget",
         {{"frameBudgetMicros",
           static_cast<std::uint64_t>(
               LuaRuntimePolicy::gameplayFrame.maxWallTime.count()) *
               1'000U},
          {"framesEvaluated", 0},
          {"maximumFrameWallMicros", 0},
          {"violationDiagnostics", nlohmann::json::array()}}},
        {"unsupportedDiagnostics", nlohmann::json::array()},
        {"unsupportedSubjects", nlohmann::json::array()},
        {"diagnosticCodes", nlohmann::json::array()},
        {"loading",
         {{"complete", isCompleteSkinLoadingTelemetry(created.loadingTelemetry)},
          {"totalMicros", created.loadingTelemetry.totalMicros},
          {"measuredMicros",
           measuredLoadMicros.count() <= 0 ? 0 : measuredLoadMicros.count()}}}};

    if (created.session) {
      std::map<SkinObjectId, std::string> familyByObject;
      for (const auto &object : created.session->modelForTesting().model.objects) {
        std::string family;
        if (std::holds_alternative<SkinNoteDistributionGraphObject>(
                object.payload)) {
          family = "noteDistribution";
        } else if (std::holds_alternative<SkinTimingVisualizerObject>(
                       object.payload)) {
          family = "timingVisualizer";
        } else if (std::holds_alternative<SkinBpmGraphObject>(object.payload)) {
          family = "bpmGraph";
        } else if (std::holds_alternative<SkinHitErrorVisualizerObject>(
                       object.payload)) {
          family = "hitErrorVisualizer";
        }
        if (!family.empty()) {
          ++declared[family];
          familyByObject.emplace(object.id, std::move(family));
        }
      }

      std::uint64_t maximumCallbackMicros = 0;
      int framesEvaluated = 0;
      for (std::uint64_t serial = 2; serial <= 4; ++serial) {
        auto state = initialState_;
        auto projection = initialProjection_;
        state.clock.serial = serial;
        state.clock.visualTimeMicros = static_cast<long long>(serial) * 750'000;
        state.clock.gameplayTimeMicros = state.clock.visualTimeMicros;
        state.clock.playTimer = {.active = true,
                                 .startMicros = 0,
                                 .elapsedMillisExact = true,
                                 .playtimeMillis = 12'000};
        projection.frameSerial = serial;
        const auto frame = created.session->prepareFrame(state, projection, {});
        for (const auto &diagnostic : frame.diagnostics) {
          rememberDiagnostic(diagnostic);
        }
        for (const auto &diagnostic : frame.evaluation.diagnostics) {
          rememberDiagnostic(diagnostic);
        }
        maximumCallbackMicros = std::max(
            maximumCallbackMicros,
            created.session->callbackWallMicrosForTesting());
        if (!frame.ready() || !frame.evaluation.submitReady) continue;
        ++framesEvaluated;
        for (const auto &command : frame.evaluation.submitReady->commands) {
          const auto found = familyByObject.find(command.sourceObject);
          if (found != familyByObject.end()) ++commands[found->second];
        }
      }
      report["callbackBudget"]["framesEvaluated"] = framesEvaluated;
      report["callbackBudget"]["maximumFrameWallMicros"] =
          maximumCallbackMicros;
      report["resourcePreparation"]["allReferencedResourcesPrepared"] =
          created.session->preparedTextureCountForTesting() ==
              created.loadingTelemetry.resources.textureUploads &&
          movieDevice->materializedPathsValid &&
          movieDevice->loaded == created.loadingTelemetry.resources.movieDecodes;
    }
    for (const auto family : familyNames) {
      report["graphFamilies"][family] = {
          {"declared", declared[std::string(family)]},
          {"commands", commands[std::string(family)]}};
    }
    for (const auto &code : budgetDiagnostics) {
      report["callbackBudget"]["violationDiagnostics"].push_back(code);
    }
    for (const auto &code : unsupportedDiagnostics) {
      report["unsupportedDiagnostics"].push_back(code);
    }
    for (const auto &subject : unsupportedSubjects) {
      report["unsupportedSubjects"].push_back(subject);
    }
    for (const auto &code : diagnosticCodes) {
      report["diagnosticCodes"].push_back(code);
    }
    created.session.reset();
    return report;
  }

  std::size_t formatCount() const noexcept { return entries_.size(); }

  nlohmann::json acceptanceMatrix() {
    nlohmann::json matrix = nlohmann::json::array();
    SkinResourcePreparationService preparation;
    std::uint64_t sessionSerial = 900;
    for (const auto &entry : entries_) {
      int keys = 0;
      for (const int candidate : {5, 7, 10, 14}) {
        if (entry.relativePath ==
            "play" + std::to_string(candidate) + "_hw.luaskin") {
          keys = candidate;
        }
      }
      if (keys == 0) continue;
      for (const int mode : {1, 2, 3}) {
        auto chart = chart_;
        chart.keyCount = keys;
        chart.staticMetadata.selectedLongNoteMode = mode;
        auto state = initialState_;
        auto projection = initialProjection_;
        auto device = std::make_shared<BenchmarkTextureDevice>();
        auto movieDevice = std::make_shared<BenchmarkMovieDevice>();
        auto counters = std::make_shared<SkinLiveResourceCounters>();
        auto created = PlaySkinSession::create(
            {.revision = lease_->clone(),
             .entry = entry.entry,
             .reconciledSettings = entry.settings,
             .configurationDigest = entry.digest},
            {.sessionSerial = ++sessionSerial,
             .profileId = profile_,
             .chartModel = chart,
             .initialState = &state,
             .initialProjection = &projection,
             .safeUiBounds = {.x = 0.0, .y = 0.0,
                              .width = 1280.0, .height = 720.0},
             .storageRoots = roots_,
             .resourcePreparation = preparation,
             .textureDevice = device,
             .movieDevice = movieDevice,
             .liveResourceCounters = counters,
             .configurationWrites = configurationWrites_});
        nlohmann::json unsupported = nlohmann::json::array();
        for (const auto &diagnostic : created.diagnostics) {
          if (diagnostic.code.find("unsupported") != std::string::npos) {
            unsupported.push_back(diagnostic.code);
          }
        }
        nlohmann::json cell = {
            {"entryPath", entry.relativePath},
            {"keys", keys},
            {"lnMode", mode},
            {"sessionPublished", created.session != nullptr},
            {"selectorIndex", nullptr},
            {"drawSlots", nlohmann::json::object()},
            {"referencedImageResourceIds", nlohmann::json::array()},
            {"preparedImageResourceIds", nlohmann::json::array()},
            {"referencedTextObjectIds", nlohmann::json::array()},
            {"preparedTextObjectIds", nlohmann::json::array()},
            {"unsupportedDiagnostics", std::move(unsupported)}};
        if (created.session) {
          if (const auto selected =
                  created.session->selectedLongNoteImageIndexForTesting(
                      state, projection)) {
            cell["selectorIndex"] = *selected;
          }
          const auto resources =
              created.session->resourcePreparationEvidenceForTesting();
          cell["referencedImageResourceIds"] =
              resources.referencedImageResourceIds;
          cell["preparedImageResourceIds"] =
              resources.preparedImageResourceIds;
          cell["referencedTextObjectIds"] =
              resources.referencedTextObjectIds;
          cell["preparedTextObjectIds"] = resources.preparedTextObjectIds;

          struct DrawScenario {
            std::string_view name;
            bool active = false;
            bool damaged = false;
            bool reactive = false;
          };
          std::vector<DrawScenario> scenarios{{"active", true},
                                              {"inactive"}};
          if (mode == 3) {
            scenarios.push_back({"reactive", false, false, true});
            scenarios.push_back({"damaged", false, true, false});
          }
          std::uint64_t frameSerial = 10;
          for (const auto &scenario : scenarios) {
            ++frameSerial;
            state.clock.serial = frameSerial;
            state.clock.visualTimeMicros =
                static_cast<long long>(frameSerial) * 100'000;
            state.clock.gameplayTimeMicros = state.clock.visualTimeMicros;
            projection.frameSerial = frameSerial;
            projection.longNotes = {{
                .headId = 1,
                .tailId = 2,
                .lane = 0,
                .mode = mode == 1 ? ChartLongNoteMode::LN
                                  : mode == 2 ? ChartLongNoteMode::CN
                                              : ChartLongNoteMode::HCN,
                .headScrollDelta = 20.0,
                .tailScrollDelta = 80.0,
                .active = scenario.active,
                .damaged = scenario.damaged,
                .reactive = scenario.reactive,
                .submissionOrdinal = 1}};
            const auto frame =
                created.session->prepareFrame(state, projection, {});
            std::vector<int> slots;
            if (frame.evaluation.submitReady) {
              for (const auto &command :
                   frame.evaluation.submitReady->commands) {
                if (command.longNoteSlotForTesting >= 0) {
                  slots.push_back(command.longNoteSlotForTesting);
                }
              }
            }
            for (const auto &diagnostic : frame.diagnostics) {
              if (diagnostic.code.find("unsupported") != std::string::npos) {
                cell["unsupportedDiagnostics"].push_back(diagnostic.code);
              }
            }
            cell["drawSlots"][std::string(scenario.name)] = std::move(slots);
          }
        }
        created.session.reset();
        matrix.push_back(std::move(cell));
      }
    }
    return matrix;
  }

private:
  struct Entry {
    SkinEntryId entry;
    std::string relativePath;
    EntryProfileSettings settings;
    std::string digest;
  };

  void prepareAcceptanceState() {
    chart_.skinGameplayGraph.mainBpm = 120.0;
    chart_.skinGameplayGraph.minimumBpm = 90.0;
    chart_.skinGameplayGraph.maximumBpm = 180.0;
    chart_.skinGameplayGraph.normalDistribution.resize(12);
    for (std::size_t second = 0;
         second < chart_.skinGameplayGraph.normalDistribution.size();
         ++second) {
      chart_.skinGameplayGraph.normalDistribution[second] = {
          1 + static_cast<int>(second % 3U), 2, 1, 0, 0, 0, 0};
    }
    chart_.skinGameplayGraph.bpmSeries = {
        {.chartTimeMicros = 0,
         .sourceOrder = 0,
         .bpm = 120.0,
         .scroll = 1.0,
         .bpmTimesScroll = 120.0,
         .graphSpeed = 120.0,
         .emitsGraphPoint = true},
        {.chartTimeMicros = 4'000'000,
         .sourceOrder = 1,
         .bpm = 180.0,
         .scroll = 1.0,
         .bpmTimesScroll = 180.0,
         .graphSpeed = 180.0,
         .emitsGraphPoint = true},
        {.chartTimeMicros = 8'000'000,
         .sourceOrder = 2,
         .bpm = 90.0,
         .scroll = 1.0,
         .bpmTimesScroll = 90.0,
         .graphSpeed = 90.0,
         .emitsGraphPoint = true}};

    auto dynamic = std::make_shared<SkinGameplayDynamicGraphState>();
    dynamic->judgementDistribution.resize(12);
    dynamic->earlyLateDistribution.resize(12);
    for (std::size_t second = 0;
         second < dynamic->judgementDistribution.size(); ++second) {
      dynamic->judgementDistribution[second] = {1, 2, 1, 1, 0, 0};
      dynamic->earlyLateDistribution[second] = {1, 1, 1, 1, 1,
                                                 1, 1, 1, 0, 0};
    }
    dynamic->judgeWindows = {{{.minimumTimingMillis = -5,
                               .maximumTimingMillis = 5},
                              {.minimumTimingMillis = -12,
                               .maximumTimingMillis = 12},
                              {.minimumTimingMillis = -24,
                               .maximumTimingMillis = 24},
                              {.minimumTimingMillis = -42,
                               .maximumTimingMillis = 42},
                              {.minimumTimingMillis = -75,
                               .maximumTimingMillis = 75}}};
    dynamic->recentJudgeTimingsMillis[0] = -24;
    dynamic->recentJudgeTimingsMillis[1] = 11;
    dynamic->recentJudgeTimingsMillis[2] = -5;
    dynamic->recentJudgeTimingIndex = 2;
    dynamic->judgementRevision = 1;
    initialState_.skinGameplayGraph = {
        .chart = std::make_shared<SkinGameplayChartGraphState>(
            chart_.skinGameplayGraph),
        .dynamic = std::move(dynamic)};
  }

  void prepareSyntheticFormats(const fs::path &source) {
    constexpr int objectCount = 48;
    fs::create_directories(source / "skin/resources");
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/beatoraja_skin/resources/fixture.png",
                  source / "skin/resources/fixture.png");
    writeText(source / "skin/resources/fixture.mp4", "bounded movie fixture");
    writeText(source / "skin/parity.luaskin", R"lua(
local skin = {type=0, name="Lua loading", author="fixture", w=1280, h=720}
if skin_config then
  local benchmark_full_title = main_state.text(12)
  local benchmark_gauge_type = main_state.gauge_type()
  skin.source = {{id="atlas", path="resources/fixture.png"},
                 {id="movie-source", path="resources/fixture.mp4"}}
  skin.image = {}
  skin.destination = {}
  for index = 1, 48 do
    local id = "image" .. index
    skin.image[index] = {id=id, src="atlas", x=0, y=0, w=40, h=20}
    skin.destination[index] = {
      id=id, dst={{x=(index - 1) * 3, y=(index - 1) % 8 * 22,
                  w=40, h=20}}
    }
  end
  skin.image[#skin.image + 1] = {
    id="movie", src="movie-source", x=0, y=0, w=80, h=40
  }
  skin.destination[#skin.destination + 1] = {
    id="movie", dst={{x=0, y=0, w=80, h=40}}
  }
  skin.judgegraph = {{id="note-distribution", type=1, delay=0}}
  skin.bpmgraph = {{id="bpm-graph", delay=0, lineWidth=2}}
  skin.timingvisualizer = {{id="timing-visualizer", width=101,
                            judgeWidth=50, lineWidth=1}}
  skin.hiterrorvisualizer = {{id="hit-error-visualizer", width=101,
                              judgeWidth=50, lineWidth=1, windowLength=6}}
  skin.destination[#skin.destination + 1] = {
    id="note-distribution", dst={{x=20, y=100, w=220, h=100}}
  }
  skin.destination[#skin.destination + 1] = {
    id="bpm-graph", dst={{x=260, y=100, w=220, h=100}}
  }
  skin.destination[#skin.destination + 1] = {
    id="timing-visualizer", dst={{x=500, y=100, w=220, h=100}}
  }
  skin.destination[#skin.destination + 1] = {
    id="hit-error-visualizer", dst={{x=740, y=100, w=220, h=100}}
  }
end
return skin
)lua");
    entryPaths_ = {"skin/parity.luaskin"};
#if ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS
    std::string json =
        R"json({"type":0,"name":"JSON loading","author":"fixture","w":1280,"h":720,"source":[{"id":"atlas","path":"resources/fixture.png"}],"image":[)json";
    for (int index = 1; index <= objectCount; ++index) {
      if (index != 1) json += ',';
      json += "{\"id\":\"image" + std::to_string(index) +
              "\",\"src\":\"atlas\",\"x\":0,\"y\":0,\"w\":40,\"h\":20}";
    }
    json += R"json(],"destination":[)json";
    for (int index = 1; index <= objectCount; ++index) {
      if (index != 1) json += ',';
      json += "{\"id\":\"image" + std::to_string(index) +
              "\",\"dst\":[{\"x\":" + std::to_string((index - 1) * 3) +
              ",\"y\":" + std::to_string((index - 1) % 8 * 22) +
              ",\"w\":40,\"h\":20}]}";
    }
    json += "]}";
    writeText(source / "skin/parity.json", json);

    std::string lr2 = "#INFORMATION,0,LR2 loading,fixture\n#RESOLUTION,1\n"
                      "#IMAGE,resources/fixture.png\n";
    for (int index = 0; index < objectCount; ++index) {
      lr2 += "#SRC_IMAGE," + std::to_string(index) +
             ",0,0,0,40,20,1,1,0,0\n";
      lr2 += "#DST_IMAGE," + std::to_string(index) + ",0," +
             std::to_string(index * 3) + ',' +
             std::to_string(index % 8 * 22) +
             ",40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0\n";
    }
    writeText(source / "skin/parity.lr2skin", lr2);
    entryPaths_ = {"skin/parity.luaskin", "skin/parity.json",
                   "skin/parity.lr2skin"};
#else
#endif
  }

  bool prepareLocalSkin(const fs::path &input, const fs::path &destination) {
    std::error_code error;
    if (fs::is_regular_file(input, error)) {
      fs::copy(input.parent_path(), destination,
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing, error);
      if (error) return false;
      entryPaths_ = {input.filename().generic_string()};
      return true;
    }
    if (!fs::is_directory(input, error)) return false;
    fs::copy(input, destination,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing, error);
    if (error) return false;
    for (const auto &item : fs::recursive_directory_iterator(input, error)) {
      if (error) return false;
      if (!item.is_regular_file()) continue;
      const auto relative = item.path().lexically_relative(input);
#if ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS
      if (gameplaySkinSourceFormatForPath(relative.generic_string())) {
        entryPaths_.push_back(relative.generic_string());
      }
#else
      if (relative.extension() == ".luaskin") {
        entryPaths_.push_back(relative.generic_string());
      }
#endif
    }
    std::ranges::sort(entryPaths_);
    return !entryPaths_.empty();
  }

  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  PlayfieldVisualState initialState_;
  PlayfieldProjectionResult initialProjection_;
  SkinResourcePreparationService validationResources_;
  SkinConfigurationWriteQueue configurationWrites_;
  std::vector<std::string> entryPaths_;
  std::vector<Entry> entries_;
  std::optional<SkinRevisionLease> lease_;
};

std::optional<std::vector<std::uint64_t>>
runSamples(BenchmarkFixture &fixture, bool warm, std::size_t samples) {
  std::vector<std::uint64_t> values;
  values.reserve(samples);
  std::uint64_t serial = 100;
  SkinResourcePreparationService warmPreparation;
  for (std::size_t index = 0; index < samples; ++index) {
    if (warm) {
      const auto sample = fixture.runSample(warmPreparation, serial);
      if (!sample) return std::nullopt;
      values.push_back(*sample);
    } else {
      SkinResourcePreparationService coldPreparation;
      const auto sample = fixture.runSample(coldPreparation, serial);
      if (!sample) return std::nullopt;
      values.push_back(*sample);
    }
  }
  return values;
}

std::uint64_t median(std::vector<std::uint64_t> values) {
  std::ranges::sort(values);
  return values[values.size() / 2U];
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parseOptions(argc, argv);
  if (!options) {
    std::cerr << "usage: gameplay_skin_loading_benchmark_tests "
                 "[--benchmark --mode cold|warm --samples N] [--skin PATH] "
                 "[--acceptance-report --skin PATH --entry PATH "
                 "--entry-identity ID] [--acceptance-matrix --skin PATH]\n";
    return 2;
  }
  BenchmarkFixture fixture(options->skin, options->format, options->entry);
  if (!fixture.ready()) {
    std::cerr << "gameplay skin loading benchmark fixture is unavailable\n";
    return 1;
  }
  if (options->acceptanceReport) {
    const auto report = fixture.acceptanceReport(*options->entryIdentity);
    if (!report) return 1;
    std::cout << report->dump() << '\n';
    return 0;
  }
  if (options->acceptanceMatrix) {
    std::cout << fixture.acceptanceMatrix().dump() << '\n';
    return 0;
  }
  if (!options->benchmark) {
    const auto cold = runSamples(fixture, false, 1);
    const auto warm = runSamples(fixture, true, 2);
    expect(cold && warm && cold->size() == 1 && warm->size() == 2 &&
               fixture.formatCount() ==
                   (ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS ? 3U : 1U),
           "cold and warm production session creation covers Lua/JSON/LR2");
    BenchmarkFixture acceptanceFixture({}, GameplaySkinSourceFormat::Lua,
                                       "skin/parity.luaskin");
    const auto acceptance =
        acceptanceFixture.acceptanceReport("entry-synthetic-fixture");
    expect(acceptance && (*acceptance)["sessionPublished"] == true &&
               (*acceptance)["resourcePreparation"]["complete"] == true &&
               (*acceptance)["callbackBudget"]["framesEvaluated"] == 3 &&
               (*acceptance)["graphFamilies"]["noteDistribution"]["commands"] > 0 &&
               (*acceptance)["graphFamilies"]["timingVisualizer"]["commands"] > 0 &&
               (*acceptance)["graphFamilies"]["bpmGraph"]["commands"] > 0 &&
               (*acceptance)["graphFamilies"]["hitErrorVisualizer"]["commands"] > 0,
           "acceptance reporter exercises production loading and all four "
           "gameplay graph command families");
    if (failures == 0) {
      std::cout << "Gameplay skin loading benchmark tests passed\n";
    }
    return failures == 0 ? 0 : 1;
  }
  const auto samples = runSamples(fixture, options->warm, options->samples);
  if (!samples) return 1;
  std::cout << "{\"mode\":\"" << (options->warm ? "warm" : "cold")
            << "\",\"formats\":" << fixture.formatCount()
            << ",\"sampleCount\":" << samples->size()
            << ",\"medianMicros\":" << median(*samples)
            << ",\"samplesMicros\":[";
  for (std::size_t index = 0; index < samples->size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << (*samples)[index];
  }
  std::cout << "]}\n";
  return 0;
}
