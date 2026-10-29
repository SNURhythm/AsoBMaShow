#include "goals/FolderGoal.h"
#include "kombo/KomboModel.h"
#include "goals/ChartGoal.h"
#include "goals/GoalResultPresentation.h"
#include "kombo/KomboRoundMotion.h"
#include "kombo/KomboResultPresentation.h"
#include <cassert>
#include <atomic>
#include "input/InputTypes.h"
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <array>
#include <vector>
#include <cmath>
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
static std::uint64_t ticks = 1000;
std::uint64_t SDL_GetTicks() { return ticks; }
long long nowMicros() { return ticks * 1000; }
namespace ui { void invalidate() {} }
enum { SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_FINGER_DOWN,
       SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_CANCELED, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP };
constexpr int SDLK_ESCAPE = 27, SDL_TOUCH_MOUSEID = -1, SDL_MOUSE_TOUCHID = -1;
struct SDL_Event {
  int type;
  struct { int key = 0; } key;
  struct { float x = 0, y = 0; int which = 0; } button;
  struct { float x = 0, y = 0; int touchID = 0; } tfinger;
};
struct UiLogicalPoint { float x = 0, y = 0; };
namespace platform_open { bool openExternalUrl(std::string_view, std::string &) { return true; } }
namespace rendering {
constexpr int render_width=1000, render_height=1000;
constexpr float widthScale = 1, heightScale = 1;
constexpr int window_width=1000, window_height=1000;
constexpr float displayPixelScale=1, ui_scale_x=1;
struct Insets { float top=0,right=0,bottom=0,left=0; };
Insets uiSafeAreaInsets() { return {}; }
void screenToUi(float x, float y, float &ox, float &oy) { ox=x; oy=y; }
void normalizedToUi(float x, float y, float &ox, float &oy) { ox=x*1000; oy=y*1000; }
}
enum class Edge { Left, Top };
namespace kombo {
enum class Sound { Progress, Complete, RoundAdvance };
struct Sounds {
  std::vector<Sound> played;
  void play(int, Sound sound) { played.push_back(sound); }
};
}
struct View {
  void setSize(float, float) {}
  void setPosition(Edge, float) {}
  void applyYogaLayout() {}
  void refreshContentLayout() {}
  int cancelled = 0, events = 0;
  bool visible = true;
  std::function<void()> release;
  float getX() const { return 500; }
  float getY() const { return 0; }
  float getWidth() const { return 400; }
  float getHeight() const { return 700; }
  void cancelPointerInput() { ++cancelled; }
  void setVisible(bool value) { visible=value; }
  void handleEvents(SDL_Event &e) { ++events; if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && release) release(); }
};
struct Attempt {
  struct {
    std::string attemptId = "test";
    struct { struct { bool autoPlay = false; } provenance; } score;
    std::int64_t playedAtUnixMillis = std::time(nullptr) * 1000;
  } result;
};
struct Outcome { bool saved() const { return saved_; } bool saved_ = false; };
struct Local {
  bool autoPlayResult=false, replayResult=false;
  struct { bool enabled=false; } practiceOptions;
  struct { bool savedResultBrowsing=false; } courseOptions;
  struct {
    std::shared_ptr<Attempt> chartAttempt = std::make_shared<Attempt>();
    std::optional<Outcome> chartOutcome = Outcome{};
  } persistenceOptions;
};
struct ResultScene {
  struct {
    struct { int audioRuntime() { return 0; } } jukebox;
    struct { struct Profile { std::string id="one"; } profile;
      const Profile &activeProfile() const { return profile; } } profileManager;
    std::uint64_t currentFrame = 11;
    std::atomic_bool replayVideoExportActive{false};
    struct { bool exportRequested() const { return false; } } rendererAccess;
    struct {
      input::LegacyInputGeneration legacyInputGeneration(int w, int h) const {
        input::LegacyInputGeneration g;
        g.drawableWidth=w; g.drawableHeight=h; g.sequence=17;
        g.anyKeyPressed=true; g.justTouched=true; g.pressedGdxKeys.set(29); g.justPressedGdxKeys.set(29);
        g.pressedMouseButtons.set(0); g.controllerCount=1;
        return g;
      }
      bool hasCurrentLegacyPressEdge() const { return true; }
    } inputDeviceRegistry;
    struct {
      int loads=0;
      std::vector<chart_goals::Goal> achievements;
      std::vector<folder_goals::Goal> folderAchievements;
      std::vector<folder_goals::Goal> TakeFolderGoalAchievements(std::string_view) { return std::exchange(folderAchievements,{}); }
      std::vector<chart_goals::Goal> TakeChartGoalAchievements(std::string_view attempt) {
        assert(attempt == "test"); return std::exchange(achievements, {});
      }
      kombo::State state;
      std::optional<kombo::PlayProgress> progress;
      std::optional<kombo::PlayProgress> TakeKomboProgress(std::string_view attempt) {
        if (!progress || progress->attemptId != attempt) return {};
        return std::exchange(progress, {});
      }
      std::optional<kombo::State> LoadKombo(std::string &) { ++loads; return state; }
    } scoreRepository;
  } context;
  Local local;
  Local *localSource() { return &local; }
  bool isCourseStageResult() const { return false; }
  bool isCourseFinalResult() const { return false; }
  kombo::ResultPresentation komboPresentation_;
  kombo::RoundPromotionCue komboRoundCue_;
  kombo::Sounds komboSounds_;
  std::uint64_t komboProgressStartedAt_=0;
  float komboOverlayUnit_=0;
  std::array<int,4> komboLayoutSignature_{};
  View *komboScroll_=&viewport;
  void buildKomboProgress(bool) { komboPanel_=&panel; komboOverlay_=&overlay; }
  void updateKomboProgress();
  std::optional<kombo::State> komboSnapshot_;
  std::optional<kombo::PlayProgress> komboProgress_;
  View viewport, panel, overlay;
  View *viewportLayout=&viewport, *komboPanel_=nullptr, *komboOverlay_=nullptr;
  std::optional<int> resultSkinMouseCapture = 1;
  std::unordered_map<int,int> resultSkinTouchCaptures{{1,1}};
  std::optional<long long> resultSkinFadeoutStartedMillis = 900;
  long long resultSkinStartedMicros = 0, komboSkinResumeMillis_ = 0;
  bool komboDismissGesture_ = false;
  std::optional<std::uint64_t> komboInputBlockedThroughFrame_;
  bool komboOwnsInput() const;
  auto inputCallback() { return CAPTURE_INPUT; }
  auto browserCallback() { return OPEN_BROWSER; }
  void prepareGoalAchievement();
  GoalResultPresentation goalPresentation_;
  std::vector<chart_goals::Goal> goalAchievements_;
  std::vector<folder_goals::Goal> folderGoalAchievements_;
  std::string goalProfileId_;
  void prepareKomboProgress();
  void dismissKomboProgress(bool swallowRelease=false);
  bool handleKomboProgressEvent(SDL_Event &);
};
PRODUCTION_METHODS
int main() {
  ResultScene goal;
  goal.context.scoreRepository.achievements = {chart_goals::Goal{}};
  goal.prepareGoalAchievement();
  assert(!goal.goalPresentation_.active()); // A failed save may be retried.
  goal.local.persistenceOptions.chartOutcome->saved_ = true;
  goal.local.practiceOptions.enabled = true; goal.prepareGoalAchievement();
  assert(!goal.goalPresentation_.active());
  goal.local.practiceOptions.enabled = false; goal.local.replayResult = true;
  goal.prepareGoalAchievement(); assert(!goal.goalPresentation_.active());
  goal.local.replayResult = false; goal.local.autoPlayResult = true;
  goal.prepareGoalAchievement(); assert(!goal.goalPresentation_.active());
  goal.local.autoPlayResult = false; goal.prepareGoalAchievement();
  assert(goal.goalPresentation_.active() && goal.goalAchievements_.size() == 1);
  goal.goalPresentation_.dismiss(); goal.prepareGoalAchievement();
  assert(!goal.goalPresentation_.active()); // Rebuilding does not replay it.

  // Only the receipt which crosses the goal ignites, including on a completed day.
  for (bool reached : {false, true}) {
    ResultScene cue;
    cue.komboSnapshot_ = kombo::State{};
    cue.komboProgress_ = kombo::PlayProgress{.reachedDailyGoal=reached};
    cue.komboPresentation_.arm({.saved=true, .manual=true, .currentDay=true, .activeRoutine=true}, ticks);
    ticks += 500;
    cue.updateKomboProgress();
    assert(cue.komboSounds_.played.size() == 1);
    assert(cue.komboSounds_.played[0] == (reached ? kombo::Sound::Complete : kombo::Sound::Progress));
    const auto ignitionStart = cue.komboProgressStartedAt_;
    ticks += 100;
    cue.komboOverlayUnit_ = 123; // Resize rebuild must keep the same animation clock.
    cue.updateKomboProgress();
    assert(cue.komboProgressStartedAt_ == ignitionStart && cue.komboSounds_.played.size() == 1);
    cue.dismissKomboProgress();
    ticks += 3000;
    cue.updateKomboProgress();
    assert(cue.komboSounds_.played.size() == 1);
  }
  {
    ResultScene folder;
    folder.local.persistenceOptions.chartOutcome->saved_=true;
    folder.context.scoreRepository.folderAchievements.push_back({});
    folder.prepareGoalAchievement();
    assert(folder.goalPresentation_.active() && folder.folderGoalAchievements_.size()==1);
    folder.goalPresentation_.dismiss(); folder.prepareGoalAchievement();
    assert(!folder.goalPresentation_.active());
  }
  ResultScene scene;
  kombo::Day day;
  day.assignments.push_back({});
  scene.context.scoreRepository.state.days.emplace(kombo::today(), day);
  scene.context.scoreRepository.progress = kombo::PlayProgress{
      .day=kombo::today(), .attemptId="test",
      .assignments={{.id="a", .beforeCharts=0, .beforeQuota=3, .beforeRound=1,
                     .afterCharts=1, .afterQuota=3, .afterRound=1}}};
  scene.prepareKomboProgress();
  assert(scene.context.scoreRepository.loads == 0 && !scene.komboPresentation_.active());
  scene.local.persistenceOptions.chartOutcome->saved_ = true; // Explicit persistence retry.
  scene.prepareKomboProgress();
  assert(scene.context.scoreRepository.loads == 1 && scene.komboPresentation_.active());
  const auto blocked = scene.inputCallback()();
  assert(!blocked.anyKeyPressed && !blocked.justTouched && blocked.pressedGdxKeys.none() &&
         blocked.justPressedGdxKeys.none() && blocked.pressedMouseButtons.none() && blocked.controllerCount == 0);
  assert(blocked.drawableWidth == 1000 && blocked.sequence == 17);
  assert(!scene.browserCallback()("https://example.com"));
  assert(scene.viewport.cancelled == 1 && !scene.resultSkinMouseCapture &&
         scene.resultSkinTouchCaptures.empty() && !scene.resultSkinFadeoutStartedMillis);
  scene.prepareKomboProgress();
  assert(scene.context.scoreRepository.loads == 1);
  SDL_Event released{.type=SDL_EVENT_MOUSE_BUTTON_UP};
  assert(scene.handleKomboProgressEvent(released)); // Held pointer cannot reach the skin.
  ticks += 500;
  assert(scene.komboPresentation_.open(ticks));
  scene.komboPanel_=&scene.panel; scene.komboOverlay_=&scene.overlay;
  SDL_Event outside{.type=SDL_EVENT_MOUSE_BUTTON_DOWN};
  assert(scene.handleKomboProgressEvent(outside));
  assert(!scene.komboPresentation_.active() && !scene.overlay.visible);
  assert(scene.handleKomboProgressEvent(released)); // Full dismiss gesture is swallowed.
  assert(!scene.handleKomboProgressEvent(released));
  scene.prepareKomboProgress(); // Rebuild/retry does not repeat the presentation.
  assert(!scene.komboPresentation_.active() && scene.context.scoreRepository.loads == 1);

  // An active routine alone must not interrupt a run that qualified for no assignment.
  for (int scenario = 0; scenario < 3; ++scenario) {
    ResultScene unqualified;
    unqualified.context.scoreRepository.state.days.emplace(kombo::today(), day);
    unqualified.local.persistenceOptions.chartOutcome->saved_ = true;
    if (scenario != 0) unqualified.context.scoreRepository.progress = kombo::PlayProgress{
        .day=kombo::today(), .attemptId=scenario == 1 ? "test" : "another-run"};
    unqualified.prepareKomboProgress();
    assert(!unqualified.komboPresentation_.active());
    assert(unqualified.viewport.cancelled == 0 && unqualified.resultSkinMouseCapture);
    assert(unqualified.inputCallback()().anyKeyPressed);
    unqualified.prepareKomboProgress();
    assert(unqualified.context.scoreRepository.loads == 1);
  }

  ResultScene button;
  assert(button.komboPresentation_.arm({.saved=true, .manual=true, .currentDay=true, .activeRoutine=true}, ticks));
  ticks += 500; button.komboPresentation_.open(ticks);
  button.komboPanel_=&button.panel; button.komboOverlay_=&button.overlay;
  button.overlay.release=[&] { button.dismissKomboProgress(); };
  SDL_Event inside{.type=SDL_EVENT_MOUSE_BUTTON_DOWN}; inside.button.x=600;
  assert(button.handleKomboProgressEvent(inside));
  assert(button.komboPresentation_.active() && button.overlay.events == 1);
  assert(button.handleKomboProgressEvent(released));
  assert(!button.komboPresentation_.active() && !button.komboDismissGesture_);
  assert(!button.inputCallback()().justTouched && !button.browserCallback()("https://example.com"));
  ++button.context.currentFrame;
  assert(button.inputCallback()().justTouched && button.browserCallback()("https://example.com"));
  assert(button.komboSkinResumeMillis_ == static_cast<long long>(ticks));

  ResultScene escape;
  escape.komboPresentation_.arm({.saved=true, .manual=true, .currentDay=true, .activeRoutine=true}, ticks);
  SDL_Event key{.type=SDL_EVENT_KEY_DOWN}; key.key.key=SDLK_ESCAPE;
  assert(escape.handleKomboProgressEvent(key));
  key.type=SDL_EVENT_KEY_UP;
  assert(escape.handleKomboProgressEvent(key));
  assert(!escape.handleKomboProgressEvent(key));
}
