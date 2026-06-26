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

struct QueueItemMetadata {
  TrackMetadata metadata;
  long long itemId = 0;
};

struct QueueMetadata {
  std::string title;
  std::vector<QueueItemMetadata> items;
  int currentIndex = -1;
};

struct MetadataVisibility {
  bool showTitle = true;
  bool showArtist = true;
  bool showArtwork = true;
};

struct PlaybackState {
  bool supported = false;
  bool loaded = false;
  bool playing = false;
  long long positionMicros = 0;
  long long durationMicros = 0;
};

bool IsSupported();
bool SetMetadataVisibility(MetadataVisibility visibility,
                           std::string &errorMessage);
bool SetQueueMetadata(const QueueMetadata &queue, std::string &errorMessage);
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
