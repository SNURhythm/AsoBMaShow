#include "RAII.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

namespace perf {
struct StartupTrace { explicit StartupTrace(const char *) {} void mark(const char *) {} };
}
// Execute the real menu start method; replace UI, parsing, and device boundaries.
namespace bms_parser {
struct Chart {
  struct { std::filesystem::path BmsPath = "chart.bms"; } Meta;
  int modifierApplications = 0;
};
}
struct ChartMetaRecord {
  struct { std::filesystem::path BmsPath = "chart.bms"; int LnMode = 0; } meta;
  bool solidArchive = false, unavailable = false;
};
namespace audio { struct PlaybackRate { int percent, mode; }; }
namespace i18n { std::string message(const char *text) { return text; } }
namespace archive_file { void appendDebugLogLine(std::string) {} }
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
void SDL_Log(const char *, ...) {}
namespace long_note_mode { int valueFromId(int mode) { return mode; } }
int normalizeChartLongNoteModeValue(int mode) { return mode; }
void applyEffectiveLongNoteModeToChart(bms_parser::Chart &, int) {}
namespace pacemaker { std::string normalizeTargetId(std::string id) { return id; } }
namespace play_options {
int parses = 0;
std::string normalizePlayOption(std::string option) { return option; }
bool isNormalPlayOption(const std::string &option) { return option == "NORMAL"; }
struct PlayOptionReplayInfo {
  std::optional<std::string> option;
  std::optional<long long> seed;
  std::optional<std::string> option2;
  std::optional<long long> seed2;
};
PlayOptionReplayInfo applySelectedPlayOptions(bms_parser::Chart &chart, const std::string &option) {
  if (option == "NORMAL") return {};
  ++chart.modifierApplications;
  return {.option = option, .seed = option == "MIRROR" ? std::nullopt : std::optional<long long>(123),
          .option2 = "MIRROR"};
}
std::unique_ptr<bms_parser::Chart> parseChart(const auto &, const auto &, const auto &,
                                            const auto &, std::atomic_bool &) {
  ++parses;
  return std::make_unique<bms_parser::Chart>();
}
}
using GaugeType = int;
using GaugeAutoShiftMode = int;
using GameplayRuleset = int;
struct StartOptions {
  int startPosition = 0;
  bool autoKeySound = false, autoPlay = false;
  int gaugeType = 0, gaugeAutoShift = 0, gaugeAutoShiftLowerBound = 0;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  int longNoteMode = 0;
  std::string assistOption, pacemakerTarget, tableName, tableLevel;
  audio::PlaybackRate playback{};
  int ruleset = 0;
};
struct LibraryFolderItem {
  enum class Type { Folder, DifficultyTable, DifficultyLevel, DifficultyClearMark };
  Type type = Type::Folder;
  int tableId = 0;
  std::string tableLevel;
};
struct DifficultyTableInfo { int id; std::string name, symbol; };
struct ImageView { static void dropAllCache() {} };
struct Label { void setLocalizedText(std::string) {} };
struct Overlay { void setChart(const ChartMetaRecord &) {} void setVisible(bool) {} };
struct Worker { void cancel() {} void stop() {} };
struct Scene {
  std::atomic_bool willStart = false, selectedChartMediaReady = true, selectedChartReusableForStart = true;
  Label *startButtonText = nullptr;
  Overlay *decideOverlay_ = nullptr;
  struct {
    int gaugeType = 0, gaugeAutoShift = 0, gaugeAutoShiftLowerBound = 0, ruleset = 0;
    std::string playOption = "NORMAL";
    int longNoteMode = 1;
    std::string assistOption, pacemakerTarget;
  } profileSelections;
  struct {
    struct { bool inputKeysoundEnabled = true; int selectedPlaybackRatePercent = 100, selectedPlaybackMode = 0; } settings;
    struct {
      int loads = 0;
      bool failLoad = false;
      void setDecodedAssetCache(std::shared_ptr<int>) {}
      struct Result { bool success; std::string diagnostic; };
      Result loadChart(bms_parser::Chart &, bool, std::atomic_bool &) { ++loads; return {!failLoad, "injected"}; }
      void stop() {}
    } jukebox;
  } context;
  LibraryFolderItem activeFolder;
  struct { std::vector<DifficultyTableInfo> tables; } folderMetadataCache;
  struct SelectedChartRandomInfo { int seed = 42; std::string prng; std::vector<int> values{2}; };
  SelectedChartRandomInfo selectedChartRandomInfoForPath(const auto &) { return {}; }
  std::unique_ptr<Worker> previewWorker_ = std::make_unique<Worker>();
  std::shared_ptr<int> previewDecodedAssets_ = std::make_shared<int>(1);
  std::shared_ptr<bms_parser::Chart> selectedChart = std::make_shared<bms_parser::Chart>();
  bms_parser::Chart *launched = nullptr;
  StartOptions launchedOptions;
  bms_parser::Chart *parsedSelectedChartForPath(const auto &path) {
    return selectedChartReusableForStart && selectedChart && selectedChart->Meta.BmsPath == path ? selectedChart.get() : nullptr;
  }
  bms_parser::Chart *loadedSelectedChartForPath(const auto &path) {
    return selectedChartMediaReady ? parsedSelectedChartForPath(path) : nullptr;
  }
  bms_parser::Chart *setSelectedChart(std::unique_ptr<bms_parser::Chart> chart, bool ready, bool reusable) {
    selectedChart = std::move(chart); selectedChartMediaReady = ready; selectedChartReusableForStart = reusable;
    return selectedChart.get();
  }
  void changeToGameplayScene(bms_parser::Chart *chart, StartOptions options) {
    launched = chart; launchedOptions = std::move(options); selectedChartReusableForStart = false;
  }
  void resetStartLoadingUi() { willStart = false; }
  template<typename F> void defer(F callback, int, bool) { callback(); }
  void startChartDirect(const ChartMetaRecord &record) START_METHOD
};
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int main() {
  try {
    for (const std::string option : {"NORMAL", "MIRROR", "RANDOM", "S-RANDOM"}) {
      Scene scene;
      scene.profileSelections.playOption = option;
      auto *previewChart = scene.selectedChart.get();
      play_options::parses = 0;
      scene.startChartDirect({});
      require(scene.launched == previewChart && play_options::parses == 0,
              "Play must reuse the preview chart for lane modifiers without reparsing");
      require(scene.context.jukebox.loads == 0,
              "completed preview must not reload resources for lane modifiers");
      require(scene.launched->modifierApplications == (option == "NORMAL" ? 0 : 1),
              "Play must apply the currently selected modifier exactly once");
      require(!scene.selectedChartReusableForStart, "a modified chart cannot be reused for another attempt");
      if (option != "NORMAL") {
        require(scene.launchedOptions.playOption == option && scene.launchedOptions.playOption2 == "MIRROR",
                "both applied play options must reach gameplay/replay capture");
        require(scene.launchedOptions.playOptionSeed == (option == "MIRROR" ? std::nullopt : std::optional<long long>(123)),
                "the applied random seed must reach gameplay/replay capture");
      }
    }
    Scene partial;
    partial.profileSelections.playOption = "RANDOM";
    partial.selectedChartMediaReady = false;
    auto *partialChart = partial.selectedChart.get();
    play_options::parses = 0;
    partial.startChartDirect({});
    require(partial.launched == partialChart && partial.context.jukebox.loads == 1 && play_options::parses == 0,
            "early Play must finish preparation on the same parsed chart");

    Scene failed;
    failed.context.jukebox.failLoad = true;
    failed.selectedChartMediaReady = false;
    failed.profileSelections.playOption = "RANDOM";
    failed.startChartDirect({});
    require(!failed.launched && failed.selectedChart->modifierApplications == 0,
            "failed resource preparation must not mutate the reusable chart");

    Scene consumed;
    consumed.profileSelections.playOption = "RANDOM";
    consumed.selectedChartReusableForStart = false;
    play_options::parses = 0;
    consumed.startChartDirect({});
    require(consumed.launched && play_options::parses == 1 && consumed.launched->modifierApplications == 1,
            "an already consumed chart must be freshly parsed before modifiers are applied again");
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
