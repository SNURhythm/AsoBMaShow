// Exercise the export session's production ownership, selection and clock logic
// without opening a GPU or encoder. Real Lua capture behavior is covered by
// play_skin_session_tests.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
struct RenderContext {};
struct ResultSkinData {
  bool showControls = true;
  void **outGraphPlaceholder = nullptr;
  int previousBest = 0;
  int previousLampBest = 0;
  std::vector<int> graph;
};
class ReplayVideoExportLog {};
void replayExportLog(ReplayVideoExportLog *, const char *, ...) {}
namespace archive_file { void readFileBounded() {} }
namespace skin {
struct SkinDiagnostic { std::string message; };
enum class GameplaySkinAcquisitionDisposition { BuiltIn, Ready, Failed };
struct Request { int activation = 0; int safetyLevel = 0; };
struct Failure { SkinDiagnostic diagnostic; };
struct Acquisition {
  GameplaySkinAcquisitionDisposition disposition = GameplaySkinAcquisitionDisposition::BuiltIn;
  std::optional<Request> request;
  std::optional<Failure> failure;
};
struct Lifecycle {
  Acquisition next;
  int target = 0;
  bool boundary = true;
  Acquisition acquireForSkinType(int type, bool chartBoundary) {
    target = type; boundary = chartBoundary; return next;
  }
};
std::optional<int> makeSkinProfileId(int id) {
  return id ? std::optional<int>(id) : std::nullopt;
}
struct BgfxSkinTextureDevice {};
struct SkinSafetyPolicy { explicit SkinSafetyPolicy(int) {} };
int createLuaSkinNoOutputAudioBackend(int *) { return 42; }
struct SessionContext {
  int expectedSkinType;
  int profileId;
  int storageRoots;
  int &resourcePreparation;
  ResultSkinData initialData;
  std::shared_ptr<BgfxSkinTextureDevice> textureDevice;
  void (*builtinImageReader)();
  int audioBackend;
  int *liveResourceCounters;
  SkinSafetyPolicy safetyPolicy;
  std::stop_token stop;
};
struct ResultSkinSession;
struct Created {
  std::unique_ptr<ResultSkinSession> session;
  std::vector<SkinDiagnostic> diagnostics;
};
struct ResultSkinSession {
  static inline ResultSkinData initial;
  static inline ResultSkinData rendered;
  static inline std::vector<long long> times;
  static inline std::vector<std::uint64_t> serials;
  static inline int skinType = 0, audio = 0, alive = 0;
  static inline bool failCreate = false, failRender = false;
  static inline std::stop_token stop;
  ResultSkinSession() { ++alive; }
  ~ResultSkinSession() { --alive; }
  static Created create(int, SessionContext context) {
    initial = context.initialData;
    skinType = context.expectedSkinType;
    audio = context.audioBackend;
    stop = context.stop;
    if (failCreate) return {nullptr, {{"decode failed"}}};
    return {std::make_unique<ResultSkinSession>(), {}};
  }
  bool renderForVideoExport(RenderContext &, const ResultSkinData &data,
                       std::uint64_t serial, long long time) {
    rendered = data; serials.push_back(serial); times.push_back(time);
    return !failRender;
  }
  std::vector<SkinDiagnostic> takeLastDiagnostics() { return {}; }
};
}
struct ApplicationContext {
  std::unique_ptr<skin::Lifecycle> gameplaySkinLifecycle = std::make_unique<skin::Lifecycle>();
  std::optional<int> skinStorageRoots = 1;
  std::unique_ptr<int> skinResourcePreparationService = std::make_unique<int>(1);
  int *skinLiveResourceCounters = skinResourcePreparationService.get();
  struct ProfileManager {
    struct Profile { int id = 1; } profile;
    Profile activeProfile() const { return profile; }
  } profileManager;
};

namespace bms_parser {
struct ChartMeta {
  int TotalNotes = 0, Rank = 0, LnMode = 0;
  int TotalLongNotes = 0, TotalBackSpinNotes = 0;
  long long PlayLength = 0;
  std::string BmsPath, Folder, StageFile, BackBmp, Banner;
};
struct Chart { ChartMeta Meta; };
}
struct CourseReplayData { std::string courseName, courseGroupName; };
struct CourseReplayVideoStage { std::shared_ptr<bms_parser::Chart> chart; };
namespace result_presentation {
bms_parser::ChartMeta courseResultMeta(const std::string &, const std::string &,
                                       std::size_t, int notes, long long length) {
  return {.TotalNotes = notes, .PlayLength = length};
}
}
ASOBMS_COURSE_META

ASOBMS_RESULT_PRESENTATION

int main() {
  int failures = 0;
  const auto check = [&](bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
  };
  auto chart = std::make_shared<bms_parser::Chart>();
  chart->Meta = {.TotalNotes = 100, .Rank = 3, .LnMode = 2,
                .TotalLongNotes = 5, .TotalBackSpinNotes = 1, .PlayLength = 123,
                .BmsPath = "/charts/test.bms", .Folder = "/charts",
                .StageFile = "stage.png", .BackBmp = "back.png", .Banner = "banner.png"};
  const auto courseMeta = courseResultMetaForReplayVideo({}, {{chart}, {chart}});
  check(courseMeta.TotalNotes == 200 && courseMeta.PlayLength == 246 &&
            courseMeta.StageFile == "stage.png" && courseMeta.BackBmp == "back.png" &&
            courseMeta.Banner == "banner.png" && courseMeta.BmsPath == "/charts/test.bms" &&
            courseMeta.Rank == 3 && courseMeta.LnMode == 2,
        "course results retain aggregated totals and resolve last-stage artwork");
  ApplicationContext app;
  RenderContext render;
  PreparedReplayResultPresentation presentation;
  std::stop_source stop;
  std::string error;
  ResultSkinData data{.previousBest = 1234, .previousLampBest = 5, .graph = {2, 7}};
  check(presentation.prepare(app, data, 7, stop.get_token(), error, nullptr) &&
            !presentation.active(), "explicit built-in selection keeps native layout");
  app.gameplaySkinLifecycle->next = {skin::GameplaySkinAcquisitionDisposition::Ready, skin::Request{}};
  using Session = skin::ResultSkinSession;
  for (int type : {7, 7, 15}) {
    check(presentation.prepare(app, data, type, stop.get_token(), error, nullptr) &&
              presentation.active(), "selected result skin activates for chart/stage/course");
    check(app.gameplaySkinLifecycle->target == type && !app.gameplaySkinLifecycle->boundary &&
              Session::skinType == type, "acquisition uses exact target without a chart boundary");
    check(!Session::initial.showControls && Session::initial.outGraphPlaceholder == nullptr &&
              Session::audio == 42, "capture hides native controls and uses silent skin audio");
    const auto index = Session::times.size();
    check(presentation.render(render, 0, error, nullptr) &&
              presentation.render(render, 1'234'567, error, nullptr), "video frames render");
    check(Session::times[index] == 0 && Session::times[index + 1] == 1234 &&
              Session::serials[index] == 1 && Session::serials[index + 1] == 2,
          "each result starts a fresh serial and uses export time in milliseconds");
    check(Session::rendered.previousBest == 1234 && Session::rendered.previousLampBest == 5 &&
              Session::rendered.graph == std::vector<int>({2, 7}), "historical scores and graphs survive owned data handoff");
    presentation.reset();
    check(Session::alive == 0 && !presentation.active(), "reset releases resources before renderer handoff");
  }
  check(presentation.prepare(app, data, 7, stop.get_token(), error, nullptr), "skin prepares for failure test");
  Session::failRender = true;
  check(!presentation.render(render, 0, error, nullptr) && !error.empty(), "render failure aborts export");
  stop.request_stop();
  check(Session::stop.stop_requested(), "external cancellation reaches skin resources");
  Session::failCreate = true;
  check(!presentation.prepare(app, data, 7, stop.get_token(), error, nullptr) &&
            !presentation.active() && error.find("decode failed") != std::string::npos,
        "failed skin creation reports diagnostic instead of substituting native UI");
  app.gameplaySkinLifecycle->next = {skin::GameplaySkinAcquisitionDisposition::Failed,
                                     std::nullopt, skin::Failure{{"selection missing"}}};
  check(!presentation.prepare(app, data, 7, stop.get_token(), error, nullptr) &&
            error.find("selection missing") != std::string::npos,
        "failed selection aborts instead of silently falling back");
  return failures == 0 ? 0 : 1;
}
