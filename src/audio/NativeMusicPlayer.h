#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace native_music_player {

enum class ControlEvent {
  Previous,
  Next,
  Finished,
};

struct TrackMetadata {
  std::string title;
  std::string artist;
  std::string album;
  std::filesystem::path artworkPath;
  long long durationMicros = 0;
};

struct PlaybackState {
  bool supported = false;
  bool loaded = false;
  bool playing = false;
  long long positionMicros = 0;
  long long durationMicros = 0;
};

bool IsSupported();
bool Load(const std::filesystem::path &audioPath,
          const TrackMetadata &metadata, std::string &errorMessage);
bool Play(std::string &errorMessage);
bool Pause(std::string &errorMessage);
bool Stop(std::string &errorMessage);
bool Seek(long long positionMicros, std::string &errorMessage);
PlaybackState GetState();
void NotifyControlEvent(ControlEvent event);
std::vector<ControlEvent> DrainControlEvents();

} // namespace native_music_player
