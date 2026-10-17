#include <cassert>
#include "ui/PresentationSchedule.h"

enum class PresentationMode { BuiltIn, Skin };
struct Presentation {
  PresentationMode mode = PresentationMode::BuiltIn;
  PresentationMode activeMode() const { return mode; }
};
struct Jukebox {
  bool paused = true, visuals = false, video = false;
  bool isPaused() const { return paused; }
  bool hasActiveVisuals() const { return visuals; }
  bool hasActiveVideoVisuals() const { return visuals && video; }
};
struct Playback { bool playing = false; };
struct Player {
  Playback state;
  Playback PlaybackState() const { return state; }
  Playback GetState() const { return state; }
};
struct Context { Jukebox jukebox; Player musicPlayer; };
struct Scene {
  virtual ~Scene() = default;
  virtual bool needsContinuousPresentation() const { return true; }
};
struct StaticScene : Scene {
  bool needsContinuousPresentation() const override { return false; }
};
struct SettingsScene : Scene {
  bool previewActive = false, previewPaused = false, previewRendererDirty = false;
  Presentation *previewPresentation = nullptr;
  bool needsContinuousPresentation() const override;
};
struct ChartViewerScene : Scene {
  bool listenActive = false;
  bool needsContinuousPresentation() const override;
};
struct GamePlayScene : Scene {
  Context context;
  bool playbackInitializationFailed = false, guidedAccessReminderPending = false;
  bool practiceMenuActive = false, coursePauseHoldActive = false, coursePauseHoldRewinding = false;
  long long practiceMenuStartPressedMicros = 0;
  struct State { bool isPlaying = true, isEnding = false; };
  State *state = nullptr;
  Presentation *presentation = nullptr;
  bool needsContinuousPresentation() const override;
};
struct MusicPlayerScene : Scene {
  Context context;
  bool videoFullscreenActive = false, videoVisualsLoaded = false;
  bool needsContinuousPresentation() const override;
};
struct MainMenuScene : Scene {
  Context context;
  Player previewPlayer_;
  Scene *utility = nullptr;
  Scene *activeUtilityTab() const { return utility; }
  bool needsContinuousPresentation() const override;
};
struct ResultScene : Scene {
  void *resultSkinSession = nullptr;
  bool needsContinuousPresentation() const override;
};

SCENE_ACTIVITY_METHODS

int main() {
  Presentation builtIn, skin{PresentationMode::Skin};
  SettingsScene settings;
  assert(!settings.needsContinuousPresentation());
  settings.previewActive = true;
  settings.previewPresentation = &builtIn;
  assert(settings.needsContinuousPresentation());
  settings.previewPaused = true;
  assert(!settings.needsContinuousPresentation());
  settings.previewPresentation = &skin;
  assert(settings.needsContinuousPresentation());
  settings.previewActive = false;
  assert(!settings.needsContinuousPresentation());

  ChartViewerScene chart;
  assert(!chart.needsContinuousPresentation());
  chart.listenActive = true;
  assert(chart.needsContinuousPresentation());
  chart.listenActive = false;
  assert(!chart.needsContinuousPresentation());

  GamePlayScene game;
  GamePlayScene::State state;
  game.state = &state;
  game.presentation = &builtIn;
  game.context.jukebox.paused = false;
  assert(game.needsContinuousPresentation());
  // Ordinary pause freezes playback; a course menu deliberately leaves it live.
  game.context.jukebox.paused = true;
  assert(!game.needsContinuousPresentation());
  game.context.jukebox.visuals = true;
  assert(!game.needsContinuousPresentation());
  game.context.jukebox.video = true;
  ui::PresentationSchedule pausedSchedule;
  pausedSchedule.didPresent(ui::revisions(), {}, 0); // Pause rendered before decode completed.
  assert(pausedSchedule.shouldRender(8, game.needsContinuousPresentation()));
  game.context.jukebox.video = false;
  assert(!pausedSchedule.shouldRender(8, game.needsContinuousPresentation()));
  game.coursePauseHoldActive = true;
  assert(game.needsContinuousPresentation());
  game.coursePauseHoldActive = false;
  game.context.jukebox.paused = false;
  assert(game.needsContinuousPresentation());
  game.context.jukebox.paused = true;
  game.presentation = &skin;
  assert(game.needsContinuousPresentation());
  game.presentation = &builtIn;
  game.practiceMenuActive = true;
  assert(!game.needsContinuousPresentation());
  game.practiceMenuStartPressedMicros = 100;
  assert(game.needsContinuousPresentation());
  game.playbackInitializationFailed = true;
  assert(!game.needsContinuousPresentation());

  MusicPlayerScene music;
  MainMenuScene menu;
  StaticScene ir;
  assert(!menu.needsContinuousPresentation());
  menu.utility = &ir;
  assert(!menu.needsContinuousPresentation());
  menu.utility = &music;
  music.context.musicPlayer.state.playing = true;
  assert(menu.needsContinuousPresentation());
  music.context.musicPlayer.state.playing = false;
  assert(!menu.needsContinuousPresentation());
  music.videoFullscreenActive = true;
  assert(!menu.needsContinuousPresentation()); // Artwork-only fallback can idle.
  music.videoVisualsLoaded = true;

  // Opening or seeking can render before the asynchronous decoder supplies its
  // frame. No View mutation or completion signal wakes this later upload.
  ui::PresentationSchedule schedule;
  for (const auto eventTime : {0U, 100U}) {
    ui::invalidate();
    assert(schedule.shouldRender(eventTime, menu.needsContinuousPresentation()));
    schedule.didPresent(ui::revisions(), {}, eventTime); // Decoder not ready yet.
    assert(schedule.shouldRender(eventTime + 8, menu.needsContinuousPresentation()));
    schedule.didPresent(ui::revisions(), {}, eventTime + 8); // Promote decoded frame.
  }
  music.videoFullscreenActive = false;
  assert(!menu.needsContinuousPresentation());
  music.videoVisualsLoaded = false;
  assert(!menu.needsContinuousPresentation());
  menu.utility = nullptr;
  menu.context.jukebox.visuals = true;
  menu.previewPlayer_.state.playing = true;
  assert(menu.needsContinuousPresentation());
  menu.previewPlayer_.state.playing = false;
  assert(!menu.needsContinuousPresentation());

  menu.context.jukebox.video = true;
  ui::PresentationSchedule eofSchedule;
  eofSchedule.didPresent(ui::revisions(), {}, 0); // EOF seek rendered before decode completed.
  assert(eofSchedule.shouldRender(8, menu.needsContinuousPresentation()));
  menu.context.jukebox.visuals = false;
  assert(!eofSchedule.shouldRender(8, menu.needsContinuousPresentation()));

  ResultScene result;
  assert(!result.needsContinuousPresentation());
  result.resultSkinSession = &skin;
  assert(result.needsContinuousPresentation() == bool(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS));
}
