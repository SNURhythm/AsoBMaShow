#include "MUSIC_SELECT_INPUT_PROCESSOR_HEADER"

#include <algorithm>
#include <cassert>
#include <memory>

MusicSelectKeyLayout musicSelectKeyLayoutForConfig(int value) {
  return value == 1 ? MusicSelectKeyLayout::Popn9K : MusicSelectKeyLayout::Beat7K;
}

struct Adapter {
  bool resetCalled = false;
  void reset() { resetCalled = true; }
};

struct MusicSelectScene {
  struct Context {
    struct Settings {
      int skinMusicSelectInput = 1;
      int skinMusicSelectScrollDurationLow = 333;
      int skinMusicSelectScrollDurationHigh = 77;
      int skinMusicSelectAnalogTicksPerScroll = 5;
    } settings;
  } context;
  std::unique_ptr<Adapter> inputBindingAdapter_ = std::make_unique<Adapter>();
  MusicSelectInputProcessor inputProcessor_{{}};
  void resetLogicalInput();
};

SCENE_METHODS

bool moved(const std::vector<MusicSelectInputAction> &actions) {
  return std::ranges::any_of(actions, [](const auto &action) {
    return action.kind == MusicSelectInputActionKind::MoveNext;
  });
}

int main() {
  MusicSelectScene scene;
  scene.resetLogicalInput();
  assert(scene.inputBindingAdapter_->resetCalled);
  MusicSelectLogicalInput down;
  down.controlHeld.insert(MusicSelectControlKey::Down);
  const auto initial = scene.inputProcessor_.process(down, 1'000);
  const auto firstMove = std::ranges::find(initial, MusicSelectInputActionKind::MoveNext,
                                         &MusicSelectInputAction::kind);
  assert(firstMove != initial.end() && firstMove->deadlineMillis == 1'333 &&
         "modal reset must retain the configured 333ms initial delay");
  assert(!moved(scene.inputProcessor_.process(down, 1'121)));
  assert(moved(scene.inputProcessor_.process(down, 1'334)));
  assert(!moved(scene.inputProcessor_.process(down, 1'375)) &&
         "modal reset must retain the configured 77ms repeat interval");
  assert(moved(scene.inputProcessor_.process(down, 1'412)));
  scene.resetLogicalInput();
  assert(moved(scene.inputProcessor_.process(down, 1'413)) &&
         "modal reset clears an in-flight repeat deadline");

  scene.resetLogicalInput();
  MusicSelectLogicalInput analog;
  analog.keys.resize(9);
  analog.changed.resize(9);
  analog.analog.resize(9);
  analog.analogDelta.resize(9);
  std::size_t lane = 0;
  while (lane < 9 && !musicSelectKeyAssigned(MusicSelectKeyLayout::Popn9K,
                                             lane, MusicSelectAssignedKey::Up)) ++lane;
  assert(lane < 9);
  analog.analog[lane] = true;
  analog.analogDelta[lane] = 4;
  assert(!moved(scene.inputProcessor_.process(analog, 2'000)) &&
         "modal reset retains the configured five analog ticks per scroll");
  analog.analogDelta[lane] = 1;
  assert(moved(scene.inputProcessor_.process(analog, 2'001)));
  analog.analogDelta[lane] = 4;
  (void)scene.inputProcessor_.process(analog, 2'200);
  scene.resetLogicalInput();
  analog.analogDelta[lane] = 1;
  assert(!moved(scene.inputProcessor_.process(analog, 2'201)) &&
         "modal reset clears partially accumulated analog ticks");
}
