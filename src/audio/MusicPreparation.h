#pragma once

#include "../bms_parser.hpp"
#include "ChartAudioRenderer.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace music_stream { class Source; }

namespace music_preparation {

struct PreparedMusic {
  bool success = false;
  std::string message;
  long long durationMicros = 0;
  std::shared_ptr<music_stream::Source> stream;
};

// Returns a stream as soon as its opening buffer is ready, including when a
// short song is already completely prepared. The stream owns its temporary PCM.
PreparedMusic PrepareMusicForPlayback(const bms_parser::ChartMeta &meta,
                                      std::atomic_bool &cancelled, bool clubMode,
                                      chart_audio::LogCallback log = {});

// Migration cleanup, restricted to files owned by the retired music cache.
// Unrecognized entries and symbolic links are preserved.
void RemoveLegacyMusicCacheFiles(const std::filesystem::path &directory);

} // namespace music_preparation
