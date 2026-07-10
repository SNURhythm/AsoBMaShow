#include "audio/AudioMix.h"
#include "audio/AudioWrapper.h"
#include "audio/Jukebox.h"

#include <cmath>
#include <concepts>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

using PlaySoundSignature = bool (AudioWrapper::*)(const path_t &, audio::Bus,
                                                  long long);
using ScheduleSoundSignature = bool (AudioWrapper::*)(const path_t &,
                                                      audio::Bus, long long);
using SetVolumesSignature = void (AudioWrapper::*)(const audio::Volumes &);
using SetSettingsVolumesSignature =
    void (AudioWrapper::*)(const player_settings::AudioSettings &);

static_assert(std::same_as<decltype(std::declval<SoundData>().sourceData),
                           std::vector<short>>);
static_assert(std::same_as<decltype(std::declval<SoundData>().outputData),
                           std::vector<short>>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().sourceFrameCount), size_t>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().outputFrameCount), size_t>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().sourceSampleRate), int>);
static_assert(
    std::same_as<decltype(std::declval<PlayingSound>().bus), audio::Bus>);
static_assert(
    std::same_as<decltype(std::declval<ScheduledSound>().bus), audio::Bus>);
static_assert(
    std::same_as<decltype(std::declval<AudioCommand>().bus), audio::Bus>);
static_assert(std::same_as<decltype(static_cast<PlaySoundSignature>(
                               &AudioWrapper::playSound)),
                           PlaySoundSignature>);
static_assert(std::same_as<decltype(static_cast<ScheduleSoundSignature>(
                               &AudioWrapper::scheduleSound)),
                           ScheduleSoundSignature>);
static_assert(std::same_as<decltype(static_cast<SetVolumesSignature>(
                               &AudioWrapper::setVolumes)),
                           SetVolumesSignature>);
static_assert(std::same_as<decltype(static_cast<SetSettingsVolumesSignature>(
                               &AudioWrapper::setVolumes)),
                           SetSettingsVolumesSignature>);

std::vector<short> stereoRamp(size_t frameCount) {
  std::vector<short> result;
  result.reserve(frameCount * 2);
  for (size_t frame = 0; frame < frameCount; ++frame) {
    result.push_back(static_cast<short>(frame % 30000));
    result.push_back(static_cast<short>(-static_cast<int>(frame % 30000)));
  }
  return result;
}

} // namespace

int main() {
  try {
    const audio::Volumes volumes{
        .master = 0.5f,
        .bgm = 0.25f,
        .keysound = 0.75f,
    };
    require(audio::EffectiveGain(audio::Bus::Bgm, volumes) == 0.125f,
            "BGM gain multiplies master and BGM volumes");
    require(audio::EffectiveGain(audio::Bus::Keysound, volumes) == 0.375f,
            "keysound gain multiplies master and keysound volumes");

    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 2.0f, .bgm = 2.0f}) == 1.0f,
            "gain inputs clamp above one");
    require(audio::EffectiveGain(audio::Bus::Keysound,
                                 {.master = 1.0f, .keysound = -1.0f}) == 0.0f,
            "gain inputs clamp below zero");
    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 0.0f, .bgm = 1.0f}) == 0.0f,
            "zero master silences a bus");
    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 1.0f, .bgm = 0.0f}) == 0.0f,
            "zero bus volume silences its bus");
    require(
        audio::EffectiveGain(audio::Bus::Keysound,
                             {.master = std::numeric_limits<float>::quiet_NaN(),
                              .keysound = 1.0f}) == 1.0f,
        "non-finite volume falls back to full volume");

    const auto source48 = stereoRamp(480);
    const auto same = audio::ResamplePcm(source48, 2, 48000, 48000);
    require(same == source48, "same-rate resampling returns an exact copy");

    const auto source441 = stereoRamp(441);
    const auto upsampled = audio::ResamplePcm(source441, 2, 44100, 48000);
    require(upsampled.size() == 480 * 2,
            "44.1 to 48 kHz produces the expected stereo frame count");
    require(upsampled.front() == source441.front(),
            "upsampling preserves the first frame");

    const auto downsampled = audio::ResamplePcm(source48, 2, 48000, 44100);
    require(downsampled.size() == 441 * 2,
            "48 to 44.1 kHz produces the expected stereo frame count");
    require(downsampled.front() == source48.front(),
            "downsampling preserves the first frame");

    require(audio::ResamplePcm({}, 2, 44100, 48000).empty(),
            "empty PCM remains empty");
    require(audio::ResamplePcm(source48, 0, 44100, 48000).empty(),
            "invalid channel count is rejected");
    require(audio::ResamplePcm(source48, 2, 0, 48000).empty(),
            "invalid source rate is rejected");
    require(audio::ResamplePcm(source48, 2, 48000, 0).empty(),
            "invalid target rate is rejected");

    const ScheduledAudioEvent defaultEvent;
    require(defaultEvent.wav == bms_parser::Parser::NoWav &&
                defaultEvent.bus == audio::Bus::Bgm,
            "scheduled audio defaults to no WAV on the BGM bus");
    const ScheduledAudioEvent keysoundEvent{
        .timeMicros = 123,
        .wav = 42,
        .bus = audio::Bus::Keysound,
    };
    require(keysoundEvent.timeMicros == 123 && keysoundEvent.wav == 42 &&
                keysoundEvent.bus == audio::Bus::Keysound,
            "scheduled audio retains explicit keysound classification");

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "audio_mix_tests: " << error.what() << '\n';
    return 1;
  }
}
