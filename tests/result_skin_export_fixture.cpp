// Exercise the production scene handoff without a GPU or platform Photos UI.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

ASOBMS_SHARE_TEXT_HELPER
struct Presentation {
  std::string title = "Title";
  struct Comparison { struct { std::string value = "HARD CLEAR"; } current; };
  std::optional<Comparison> lampComparison = Comparison{};
};
struct Grade { std::string grade = "AAA"; };
std::optional<Grade> gradeCard(const Presentation &) { return Grade{}; }
namespace i18n { const char *tr(const char *) { return "Share"; } }
constexpr int SDL_MESSAGEBOX_ERROR = 1;
template<class... Args> void SDL_ShowSimpleMessageBox(Args &&...) {}
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
struct RenderContext {
  struct UiBatchScope { explicit UiBatchScope(RenderContext &) {} };
};
struct ResultSkinData { bool showControls = true; int graph = 73; };
struct ResultPacemakerData {};
struct ResultImageExportResult {
  bool success = true;
  std::filesystem::path outputPath;
  std::string message;
};
struct ApplicationContext { std::uint64_t currentFrame = 17; };
struct ResultSkinSession {
  int renders = 0;
  int captures = 0;
  bool succeeds = true;
  bool controls = true;
  int graph = 0;
  long long elapsed = 0;
  std::uint64_t lastSerial = 0;
  bool needsRefresh = false;
  bool refreshSucceeds = true;
  int refreshes = 0;
  bool requiresRuntimeStringRefresh(const ResultSkinData &) const { return needsRefresh; }
  bool refreshRuntimeStrings(const ResultSkinData &) { ++refreshes; return refreshSucceeds; }
  bool renderForExport(RenderContext &context, const ResultSkinData &data,
                       std::uint64_t serial, long long time) {
    ++captures;
    return render(context, data, serial, time);
  }
  bool render(RenderContext &, const ResultSkinData &data,
              std::uint64_t serial, long long time) {
    if (serial <= lastSerial) return false;
    lastSerial = serial;
    ++renders;
    controls = data.showControls;
    graph = data.graph;
    elapsed = time;
    return succeeds;
  }
};
namespace result_image_export {
struct ExportOptions {
  std::filesystem::path outputDirectory;
  bool saveToPhotos = true;
};
}
struct ChartMeta {
  std::string Title = "Local chart", SubTitle = "Subtitle";
  std::string StageFile = "jacket.png", Banner = "banner.png";
};
struct ResultImageExporter {
  static inline int nativeExports = 0;
  static inline int shares = 0;
  static inline std::string sharedText;
  static inline const ChartMeta *exportedMeta = nullptr;
  static inline int exportedAnalytics = -1;
  static inline result_image_export::ExportOptions exportedOptions;
  static ResultImageExportResult Share(
      const std::string &text,
      const std::function<ResultImageExportResult(
          const result_image_export::ExportOptions &)> &exportImage) {
    ++shares;
    sharedText = text;
    return exportImage({.outputDirectory = "share-cache", .saveToPhotos = false});
  }
  static inline int skinExports = 0;
  static ResultImageExportResult Export(
      ApplicationContext &, const ChartMeta &meta, int,
      const std::string &, const std::string &, const std::string &,
      int, int, int, int, int, const std::optional<ResultPacemakerData> &,
      int analytics, const result_image_export::ExportOptions &options) {
    ++nativeExports;
    exportedMeta = &meta;
    exportedAnalytics = analytics;
    exportedOptions = options;
    return {};
  }
  static ResultImageExportResult Export(
      ApplicationContext &, const Presentation &,
      const result_image_export::ExportOptions &options) {
    ++nativeExports;
    exportedMeta = nullptr;
    exportedOptions = options;
    return {};
  }
  static ResultImageExportResult ExportSkin(
      ApplicationContext &, const std::string &,
      const std::function<bool(RenderContext &)> &draw,
      const result_image_export::ExportOptions &options) {
    ++skinExports;
    exportedOptions = options;
    RenderContext context;
    return {.success = draw(context)};
  }
};
struct LocalSource {
  bool autoPlayResult = false;
  ChartMeta meta;
  Presentation presentation;
  int resultState = 0;
  std::string playModeLabel, laneOrderLabel, difficultyLabel;
  int previousBest = 0, previousLampBest = 0;
  int currentClearLabelOverride = 0, currentClearRankOverride = 0;
  int headerDifficultyLabelOverride = 0;
};
struct RemoteSource { Presentation presentation; };
struct View {
  int renders = 0;
  void applyYogaLayout() {}
  void render(RenderContext &) { ++renders; }
};
enum class ResultPhotoExportPresentation { Ready, Saving, Saved, Failed };
long long nowMicros() { return 6'000'000; }
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
template<class... Args> void SDL_Log(Args &&...) {}
struct ResultScene {
  LocalSource local;
  RemoteSource remote;
  bool isRemote = false;
  ApplicationContext context;
  std::unique_ptr<ResultSkinSession> resultSkinSession;
  std::uint64_t resultSkinFrameSerial = 0;
  long long resultSkinStartedMicros = 1'000'000;
  bool resultPhotoExportInProgress = false;
  int diagnosticPublications = 0;
  ResultPhotoExportPresentation status = ResultPhotoExportPresentation::Ready;
  View *rootLayout = nullptr;
  const LocalSource *localSource() const { return isRemote ? nullptr : &local; }
  const RemoteSource *remoteSource() const { return isRemote ? &remote : nullptr; }
  ResultSkinData makeResultSkinData() const { return {}; }
  std::optional<ResultPacemakerData> pacemakerDataForCurrentResult() const { return {}; }
  int makeTimingAnalyticsModel() const { return 42; }
  void setResultPhotoExportPresentation(ResultPhotoExportPresentation value) { status = value; }
  void appendResultSkinRenderDiagnostics() { ++diagnosticPublications; }
  template<class Callback> void defer(Callback, int, bool) {}
  void exportPhoto(bool share = false);
  bool drawOnscreen() {
    RenderContext renderContext;
    auto skinData = makeResultSkinData();
    long long elapsedMillis = 5000;
    ASOBMS_NORMAL_SKIN_RENDER
    return rendered;
  }
};
ASOBMS_EXPORT_PHOTO_METHOD

int nativeGaugeDraws = 0;
namespace bgfx {
int submittedFrames = 0;
void frame() { ++submittedFrames; }
}
void drawResultGaugeGraph(int, int, View *) { ++nativeGaugeDraws; }
ResultImageExportResult drawExportFrame(
    View *resultRoot, const std::function<bool(RenderContext &)> &renderSkin,
    bool attachGaugeAsView = false) {
  RenderContext renderContext;
  std::filesystem::path path = "result.png";
  int graphBatch = 0, gaugeGraph = 0;
  View *graphPlaceHolder = nullptr;
  ASOBMS_EXPORT_DRAW
  return {.success = true};
}

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
  };
  ResultScene onscreen;
  onscreen.resultSkinSession = std::make_unique<ResultSkinSession>();
  expect(onscreen.drawOnscreen(), "initial onscreen skin render succeeds");
  onscreen.exportPhoto();
  expect(onscreen.status == ResultPhotoExportPresentation::Saved && onscreen.drawOnscreen(),
         "onscreen render, export, and next onscreen render use increasing Lua frame serials");
  expect(onscreen.diagnosticPublications == 3,
         "normal and export frames publish diagnostics even on successful renders");
  onscreen.resultSkinSession->succeeds = false;
  expect(!onscreen.drawOnscreen() && onscreen.diagnosticPublications == 4,
         "failed normal frames also publish skin diagnostics");
  for (bool remote : {false, true}) {
    ResultImageExporter::nativeExports = ResultImageExporter::skinExports = 0;
    ResultScene scene;
    scene.isRemote = remote;
    scene.resultSkinSession = std::make_unique<ResultSkinSession>();
    scene.exportPhoto();
    expect(ResultImageExporter::skinExports == 1 && ResultImageExporter::nativeExports == 0,
           "selected result skin must replace native export for local and remote results");
    expect(scene.resultSkinSession->renders == 1 && scene.resultSkinSession->captures == 1 &&
               !scene.resultSkinSession->controls &&
               scene.resultSkinSession->graph == 73 && scene.resultSkinSession->elapsed == 5000,
           "export draws the current skin data and animation without native controls");
    expect(!scene.resultPhotoExportInProgress && scene.status == ResultPhotoExportPresentation::Saved,
           "successful skin export restores the scene export state");
    scene.exportPhoto();
    expect(scene.status == ResultPhotoExportPresentation::Saved &&
               scene.resultSkinSession->lastSerial == 2,
           "multiple exports in one application frame use distinct Lua frame serials");
    scene.resultSkinSession->needsRefresh = true;
    scene.exportPhoto();
    expect(scene.status == ResultPhotoExportPresentation::Saved &&
               scene.resultSkinSession->refreshes == 1,
           "updated result strings are prepared before drawing the export");
    scene.resultSkinSession->refreshSucceeds = false;
    scene.exportPhoto();
    expect(scene.status == ResultPhotoExportPresentation::Failed &&
               scene.resultSkinSession->renders == 3 && !scene.resultPhotoExportInProgress,
           "failed string preparation does not render stale resources or leave export busy");
    scene.resultSkinSession->needsRefresh = false;
    scene.resultSkinSession->succeeds = false;
    scene.exportPhoto();
    expect(scene.status == ResultPhotoExportPresentation::Failed &&
               !scene.resultPhotoExportInProgress && ResultImageExporter::nativeExports == 0,
           "skin render failure is reported without exporting a different layout");
    scene.resultSkinSession.reset();
    scene.exportPhoto();
    expect(ResultImageExporter::nativeExports == 1,
           "results without an active skin retain native image export");
  }
  ResultScene sharing;
  sharing.resultSkinSession = std::make_unique<ResultSkinSession>();
  sharing.exportPhoto(true);
  expect(ResultImageExporter::shares == 1 &&
             ResultImageExporter::sharedText == "Title Subtitle HARD CLEAR AAA\n#AsoBMaShow" &&
             sharing.resultSkinSession->captures == 1 &&
             !sharing.resultSkinSession->controls && !sharing.resultPhotoExportInProgress,
         "sharing hands off requested text and the current skin without controls");
  sharing.local.autoPlayResult = true;
  sharing.exportPhoto(true);
  expect(ResultImageExporter::shares == 1, "autoplay cannot share results");
  sharing.local.autoPlayResult = false;
  sharing.resultSkinSession.reset();
  ResultImageExporter::nativeExports = 0;
  sharing.exportPhoto(true);
  expect(ResultImageExporter::shares == 2, "default result layout can be shared");
  expect(ResultImageExporter::nativeExports == 1,
         "default sharing must reuse photo export with chart metadata and analytics");
  expect(ResultImageExporter::exportedMeta == &sharing.local.meta &&
             ResultImageExporter::exportedMeta->StageFile == "jacket.png" &&
             ResultImageExporter::exportedMeta->Banner == "banner.png" &&
             ResultImageExporter::exportedAnalytics == 42,
         "sharing preserves jacket sources and uses the photo analytics preparation");
  expect(!ResultImageExporter::exportedOptions.saveToPhotos &&
             ResultImageExporter::exportedOptions.outputDirectory == "share-cache",
         "sharing changes only the destination and disables saving to Photos");
  sharing.exportPhoto();
  expect(ResultImageExporter::exportedMeta == &sharing.local.meta &&
             ResultImageExporter::exportedAnalytics == 42 &&
             ResultImageExporter::exportedOptions.saveToPhotos &&
             ResultImageExporter::exportedOptions.outputDirectory.empty(),
         "photo export retains the same chart and analytics with its normal destination");
  sharing.isRemote = true;
  sharing.exportPhoto(true);
  expect(ResultImageExporter::exportedMeta == nullptr &&
             !ResultImageExporter::exportedOptions.saveToPhotos,
         "remote sharing retains the existing presentation export path");
  View nativeRoot;
  int skinDraws = 0;
  auto image = drawExportFrame(&nativeRoot, [&](RenderContext &) {
    ++skinDraws;
    return true;
  });
  expect(image.success && skinDraws == 1 && nativeRoot.renders == 0 && nativeGaugeDraws == 0,
         "offscreen skin frame excludes the native root and gauge overlays");
  image = drawExportFrame(&nativeRoot, [](RenderContext &) { return false; });
  expect(!image.success && nativeRoot.renders == 0 && nativeGaugeDraws == 0,
         "failed skin frame does not save a native fallback image");
  expect(bgfx::submittedFrames == 1,
         "failed export drains partial submissions before restoring the window target");
  image = drawExportFrame(&nativeRoot, {});
  expect(image.success && nativeRoot.renders == 1 && nativeGaugeDraws == 1,
         "native export still draws its result and gauge");
  image = drawExportFrame(&nativeRoot, {}, true);
  expect(image.success && nativeRoot.renders == 2 && nativeGaugeDraws == 1,
         "native presentation gauge is not drawn twice");
  return failures == 0 ? 0 : 1;
}
