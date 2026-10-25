#pragma once
#include "AudioWrapper.h"
#include "../path.h"
#include <SDL3/SDL.h>
#include <limits>
#include <optional>
#include <stop_token>

// Previews the chart and the looping select BGM through the same AudioWrapper
// skin-sound path as the system-SE player. The default handle stays loaded so
// returning to the select BGM is instant; preview sounds are disposed as soon
// as they stop so the session keeps only the default decoded
// (PreviewMusicProcessor.java:97-101, symmetric fade approximated).
class MusicSelectPreviewFilePlayer {
public:
  explicit MusicSelectPreviewFilePlayer(AudioWrapper *audio,
                                       std::filesystem::path defaultPath,
                                       bool analyzeOutput = false)
      : audio_(audio), defaultKey_(fspath_to_path_t(defaultPath)),
        analyzeOutput_(analyzeOutput) {}

  ~MusicSelectPreviewFilePlayer() {
    stopCurrent();
    if (audio_ && defaultHandle_) {
      audio_->disposeSkinSound(*defaultHandle_);
    }
  }

  bool play(const std::filesystem::path &path, bool loop,
            std::atomic_bool &cancellation,
            std::stop_token stop = {}, float gain = 1.0F) {
    if (!audio_) return false;
    stopCurrent();
    const auto key = fspath_to_path_t(path);
    const bool isDefault = (key == defaultKey_);
    audio::SkinSoundHandle handle;
    if (isDefault && defaultHandle_) {
      handle = *defaultHandle_;
    } else {
      const auto loaded = audio_->loadSkinSound(
          key, cancellation, std::numeric_limits<std::size_t>::max(),
          std::numeric_limits<std::size_t>::max(), stop);
      if (!loaded.handle) {
        // A superseded request is cancelled mid-load and expected to fail; only
        // warn on a genuine load failure (e.g. the bundled default BGM missing)
        // so navigation back-and-forth never spams the log.
        if (!cancellation.load(std::memory_order_acquire) &&
            !stop.stop_requested()) {
          SDL_Log("Music-select skin sound failed to load: %s",
                  fspath_to_utf8(path).c_str());
        }
        return false;
      }
      handle = *loaded.handle;
      if (isDefault) defaultHandle_ = handle;
    }
    if (cancellation.load(std::memory_order_acquire) ||
        stop.stop_requested()) {
      (void)audio_->disposeSkinSound(handle);
      if (isDefault) defaultHandle_.reset();
      return false;
    }
    if (analyzeOutput_) audio_->setOutputAnalysisEnabled(true);
    if (!audio_->playSkinSound(handle, gain, loop)) {
      if (analyzeOutput_) audio_->setOutputAnalysisEnabled(false);
      (void)audio_->disposeSkinSound(handle);
      if (isDefault) defaultHandle_.reset();
      return false;
    }
    // The play command was just enqueued on the mixer. If the scene was paused
    // or a launch started while we were loading (cancellation set), the enqueued
    // command could otherwise survive the backend stop and resume when gameplay
    // restarts the audio. Stop it immediately so the select BGM never bleeds
    // into gameplay.
    if (cancellation.load(std::memory_order_acquire) ||
        stop.stop_requested()) {
      if (analyzeOutput_) audio_->setOutputAnalysisEnabled(false);
      (void)audio_->stopSkinSound(handle);
      (void)audio_->disposeSkinSound(handle);
      if (isDefault) defaultHandle_.reset();
      return false;
    }
    playingHandle_ = handle;
    playingKey_ = key;
    return true;
  }

  void stopCurrent() {
    if (audio_ && analyzeOutput_) audio_->setOutputAnalysisEnabled(false);
    if (!audio_ || !playingHandle_) return;
    (void)audio_->stopSkinSound(*playingHandle_);
    if (playingKey_ != defaultKey_) {
      (void)audio_->disposeSkinSound(*playingHandle_);
    }
    playingHandle_.reset();
    playingKey_ = {};
  }

  [[nodiscard]] float outputLevel() const noexcept {
    return audio_ && analyzeOutput_ ? audio_->outputLevel() : 0;
  }
  audio::Spectrum outputSpectrum() {
    return audio_ && analyzeOutput_ ? audio_->outputSpectrum() : audio::Spectrum{};
  }

private:
  AudioWrapper *audio_;
  path_t defaultKey_;
  bool analyzeOutput_;
  std::optional<audio::SkinSoundHandle> defaultHandle_;
  std::optional<audio::SkinSoundHandle> playingHandle_;
  path_t playingKey_{};
};
