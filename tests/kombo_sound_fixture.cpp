#include <array>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <limits>
#include <optional>
namespace audio { struct SkinSoundHandle { int value=0; explicit operator bool() const { return value!=0; } }; }
std::filesystem::path fspath_to_path_t(const std::filesystem::path &p) { return p; }
struct AudioWrapper {
  int loads=0, plays=0, disposed=0;
  std::size_t loadedBytes=2*1024*1024; // Retained selector/custom-skin sounds.
  struct Loaded { std::optional<audio::SkinSoundHandle> handle; };
  Loaded loadSkinSound(const std::filesystem::path &, std::atomic_bool &, std::size_t encoded, std::size_t total) {
    ++loads;
    assert(encoded == 256*1024);
    if (loadedBytes + 200*1024 > total) return {};
    loadedBytes += 200*1024;
    return {audio::SkinSoundHandle{loads}};
  }
  void playSkinSound(audio::SkinSoundHandle, float gain, bool loop) { assert(gain==1 && !loop); ++plays; }
  void stopSkinSound(audio::SkinSoundHandle) {}
  void disposeSkinSound(audio::SkinSoundHandle) { ++disposed; }
};
SOUNDS
int main() {
  AudioWrapper audio;
  {
    kombo::Sounds cues;
    cues.play(audio, kombo::Sound::Progress);
    assert(audio.plays == 1);
    cues.play(audio, kombo::Sound::Progress);
    assert(audio.loads == 1 && audio.plays == 2);
    cues.play(audio, kombo::Sound::Complete);
    assert(audio.loads == 2 && audio.plays == 3);
    cues.play(audio, kombo::Sound::RoundAdvance);
    assert(audio.loads == 3 && audio.plays == 4);
    cues.play(audio, kombo::Sound::GoalAchieved);
    assert(audio.loads == 4 && audio.plays == 5);
  }
  assert(audio.disposed == 4);
}
