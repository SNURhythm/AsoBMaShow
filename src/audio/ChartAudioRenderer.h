#pragma once

#include "../ReplayData.h"
#include "../bms_parser.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace chart_audio {

inline constexpr int kOutputSampleRate = 44100;
inline constexpr int kOutputChannels = 2;

inline long long outputTimeMicros(long long chartTimeMicros,
                                  audio::PlaybackRate playback) {
  return playback.realMicrosFromChart(chartTimeMicros);
}

inline long double
sourceFramesPerOutputFrame(int sourceSampleRate,
                           audio::PlaybackRate playback,
                           int outputSampleRate = kOutputSampleRate) {
  if (sourceSampleRate <= 0 || outputSampleRate <= 0) {
    return 0.0L;
  }
  return static_cast<long double>(sourceSampleRate) /
         static_cast<long double>(outputSampleRate) *
         static_cast<long double>(playback.percent) / 100.0L;
}

struct AudioEvent {
  long long timeMicros = 0;
  int wav = bms_parser::Parser::NoWav;
};

enum class KeySoundMode {
  BackgroundOnly,
  ChartTiming,
  ReplayTiming,
};

using LogCallback = std::function<void(const std::string &)>;

struct RenderOptions {
  KeySoundMode keySoundMode = KeySoundMode::ChartTiming;
  const ReplayData *replay = nullptr;
  audio::PlaybackRate playback;
  long long keySoundOffsetMicros = 0;
  std::atomic_bool *isCancelled = nullptr;
  LogCallback log;
};

struct RenderResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
  long long durationMicros = 0;
  std::size_t eventCount = 0;
};

std::vector<AudioEvent>
CollectBackgroundAudioEvents(const bms_parser::Chart &chart);

std::vector<AudioEvent>
CollectChartTimedAudioEvents(const bms_parser::Chart &chart);

std::vector<AudioEvent>
CollectReplayTimedAudioEvents(const bms_parser::Chart &chart,
                              const ReplayData &replay,
                              long long keySoundOffsetMicros = 0);

RenderResult RenderChartAudioToWav(const bms_parser::Chart &chart,
                                   const std::filesystem::path &path,
                                   const RenderOptions &options = {});

} // namespace chart_audio
