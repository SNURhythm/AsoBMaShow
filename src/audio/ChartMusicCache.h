#pragma once

#include "../bms_parser.hpp"
#include "ChartAudioRenderer.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chart_music_cache {

struct CacheResult {
  bool success = false;
  bool rendered = false;
  std::filesystem::path audioPath;
  std::string message;
  long long durationMicros = 0;
};

std::filesystem::path CacheDirectory();
std::filesystem::path CachedAudioPathForChart(
    const bms_parser::ChartMeta &meta, bool clubMode = false);
bool CachedAudioExists(const bms_parser::ChartMeta &meta,
                       bool clubMode = false);
std::optional<long long>
ReadAudioFileDurationMicros(const std::filesystem::path &path);
void PruneCacheExcept(const std::vector<std::filesystem::path> &keepPaths);

CacheResult EnsureRenderedMusicFile(const bms_parser::ChartMeta &meta,
                                    std::atomic_bool &cancelled,
                                    chart_audio::LogCallback log = {});
CacheResult EnsureRenderedMusicFile(const bms_parser::ChartMeta &meta,
                                    std::atomic_bool &cancelled, bool clubMode,
                                    chart_audio::LogCallback log = {});

CacheResult EnsureRenderedMusicFile(bms_parser::Chart &chart,
                                    std::atomic_bool &cancelled,
                                    chart_audio::LogCallback log = {});
CacheResult EnsureRenderedMusicFile(bms_parser::Chart &chart,
                                    std::atomic_bool &cancelled, bool clubMode,
                                    chart_audio::LogCallback log = {});

} // namespace chart_music_cache
