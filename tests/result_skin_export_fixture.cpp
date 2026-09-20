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
struct ResultImageExporter {
  static inline int nativeExports = 0;
  static inline int skinExports = 0;
  template<class... Args>
  static ResultImageExportResult Export(Args &&...) {
    ++nativeExports;
    return {};
  }
  static ResultImageExportResult ExportSkin(
      ApplicationContext &, const std::string &,
      const std::function<bool(RenderContext &)> &draw) {
    ++skinExports;
    RenderContext context;
    return {.success = draw(context)};
  }
};
struct LocalSource {
  bool autoPlayResult = false;
  struct { std::string Title = "Local chart"; } meta;
  int resultState = 0;
  std::string playModeLabel, laneOrderLabel, difficultyLabel;
  int previousBest = 0, previousLampBest = 0;
  int currentClearLabelOverride = 0, currentClearRankOverride = 0;
  int headerDifficultyLabelOverride = 0;
};
struct RemoteSource { struct { std::string title = "Remote chart"; } presentation; };
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
  int makeTimingAnalyticsModel() const { return 0; }
  void setResultPhotoExportPresentation(ResultPhotoExportPresentation value) { status = value; }
  void appendResultSkinRenderDiagnostics() { ++diagnosticPublications; }
  template<class Callback> void defer(Callback, int, bool) {}
  void exportPhoto();
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
