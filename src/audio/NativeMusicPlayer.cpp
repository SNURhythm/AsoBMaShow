#include "NativeMusicPlayer.h"

#include "../path.h"
#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace native_music_player {
namespace {

std::mutex gControlEventsMutex;
std::vector<ControlEvent> gControlEvents;
std::mutex gMetadataMutex;
MetadataVisibility gMetadataVisibility;
std::optional<TrackMetadata> gLastRawMetadata;
std::optional<QueueMetadata> gLastRawQueue;

std::string pathToUtf8(const std::filesystem::path &path) {
  return path_t_to_utf8(fspath_to_path_t(path));
}

TrackMetadata normalizedMetadata(TrackMetadata metadata,
                                 const std::filesystem::path &audioPath) {
  if (metadata.title.empty()) {
    metadata.title = audioPath.stem().string();
  }
  if (metadata.artist.empty()) {
    metadata.artist = "AsoBMaShow";
  }
  if (!metadata.artworkPath.empty()) {
    metadata.artworkPath = metadata.artworkPath.lexically_normal();
  }
  metadata.durationMicros = std::max(0LL, metadata.durationMicros);
  return metadata;
}

TrackMetadata applyMetadataVisibility(TrackMetadata metadata,
                                      MetadataVisibility visibility) {
  if (!visibility.showTitle) {
    metadata.title = "AsoBMaShow";
  }
  if (!visibility.showArtist) {
    metadata.artist.clear();
  }
  if (!visibility.showArtwork) {
    metadata.artworkPath.clear();
  }
  return metadata;
}

QueueMetadata normalizedQueueMetadata(QueueMetadata queue) {
  if (queue.title.empty()) {
    queue.title = "Now Playing";
  }
  for (std::size_t i = 0; i < queue.items.size(); ++i) {
    TrackMetadata &metadata = queue.items[i].metadata;
    if (metadata.title.empty()) {
      metadata.title = "Untitled";
    }
    if (metadata.artist.empty()) {
      metadata.artist = "AsoBMaShow";
    }
    if (!metadata.artworkPath.empty()) {
      metadata.artworkPath = metadata.artworkPath.lexically_normal();
    }
    metadata.durationMicros = std::max(0LL, metadata.durationMicros);
    if (queue.items[i].itemId <= 0) {
      queue.items[i].itemId = static_cast<long long>(i + 1);
    }
  }
  if (queue.currentIndex < 0 ||
      static_cast<std::size_t>(queue.currentIndex) >= queue.items.size()) {
    queue.currentIndex = -1;
  }
  return queue;
}

QueueMetadata applyQueueMetadataVisibility(QueueMetadata queue,
                                           MetadataVisibility visibility) {
  for (auto &item : queue.items) {
    item.metadata = applyMetadataVisibility(std::move(item.metadata),
                                            visibility);
  }
  return queue;
}

bool updatePlatformMetadata(const TrackMetadata &metadata,
                            std::string &errorMessage) {
  errorMessage.clear();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  IOSNativeMusicMetadata iosMetadata{.title = metadata.title,
                                     .artist = metadata.artist,
                                     .album = metadata.album,
                                     .artworkPath =
                                         pathToUtf8(metadata.artworkPath),
                                     .durationMicros =
                                         metadata.durationMicros};
  return UpdateIOSNativeMusicMetadata(iosMetadata, errorMessage);
#elif TARGET_OS_ANDROID
  AndroidNativeMusicMetadata androidMetadata{.title = metadata.title,
                                             .artist = metadata.artist,
                                             .album = metadata.album,
                                             .artworkPath =
                                                 pathToUtf8(
                                                     metadata.artworkPath),
                                             .durationMicros =
                                                 metadata.durationMicros};
  return UpdateAndroidNativeMusicMetadata(androidMetadata, errorMessage);
#else
  (void)metadata;
  return true;
#endif
}

bool updatePlatformQueue(const QueueMetadata &queue,
                         std::string &errorMessage) {
  errorMessage.clear();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  IOSNativeMusicQueue iosQueue;
  iosQueue.title = queue.title;
  iosQueue.currentIndex = queue.currentIndex;
  iosQueue.items.reserve(queue.items.size());
  for (const auto &item : queue.items) {
    IOSNativeMusicQueueItem iosItem;
    iosItem.metadata.title = item.metadata.title;
    iosItem.metadata.artist = item.metadata.artist;
    iosItem.metadata.album = item.metadata.album;
    iosItem.metadata.artworkPath = pathToUtf8(item.metadata.artworkPath);
    iosItem.metadata.durationMicros = item.metadata.durationMicros;
    iosItem.itemId = item.itemId;
    iosQueue.items.push_back(std::move(iosItem));
  }
  return UpdateIOSNativeMusicQueue(iosQueue, errorMessage);
#elif TARGET_OS_ANDROID
  AndroidNativeMusicQueue androidQueue;
  androidQueue.title = queue.title;
  androidQueue.currentIndex = queue.currentIndex;
  androidQueue.items.reserve(queue.items.size());
  for (const auto &item : queue.items) {
    AndroidNativeMusicQueueItem androidItem;
    androidItem.metadata.title = item.metadata.title;
    androidItem.metadata.artist = item.metadata.artist;
    androidItem.metadata.album = item.metadata.album;
    androidItem.metadata.artworkPath = pathToUtf8(item.metadata.artworkPath);
    androidItem.metadata.durationMicros = item.metadata.durationMicros;
    androidItem.itemId = item.itemId;
    androidQueue.items.push_back(std::move(androidItem));
  }
  return UpdateAndroidNativeMusicQueue(androidQueue, errorMessage);
#else
  (void)queue;
  return true;
#endif
}

} // namespace

bool IsSupported() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR || TARGET_OS_ANDROID
  return true;
#else
  return false;
#endif
}

bool SetMetadataVisibility(MetadataVisibility visibility,
                           std::string &errorMessage) {
  errorMessage.clear();
  std::optional<TrackMetadata> visibleMetadata;
  std::optional<QueueMetadata> visibleQueue;
  {
    std::lock_guard<std::mutex> lock(gMetadataMutex);
    gMetadataVisibility = visibility;
    if (gLastRawMetadata) {
      visibleMetadata =
          applyMetadataVisibility(*gLastRawMetadata, gMetadataVisibility);
    }
    if (gLastRawQueue) {
      visibleQueue =
          applyQueueMetadataVisibility(*gLastRawQueue, gMetadataVisibility);
    }
  }
  bool updated = true;
  if (visibleMetadata &&
      !updatePlatformMetadata(*visibleMetadata, errorMessage)) {
    updated = false;
  }
  if (visibleQueue) {
    std::string queueError;
    if (!updatePlatformQueue(*visibleQueue, queueError)) {
      if (errorMessage.empty()) {
        errorMessage = std::move(queueError);
      }
      updated = false;
    }
  }
  return updated;
}

bool SetQueueMetadata(const QueueMetadata &queue, std::string &errorMessage) {
  errorMessage.clear();
  const QueueMetadata normalized = normalizedQueueMetadata(queue);
  QueueMetadata visibleQueue;
  {
    std::lock_guard<std::mutex> lock(gMetadataMutex);
    gLastRawQueue = normalized;
    visibleQueue = applyQueueMetadataVisibility(normalized, gMetadataVisibility);
  }
  return updatePlatformQueue(visibleQueue, errorMessage);
}

bool Load(const std::filesystem::path &audioPath,
          const TrackMetadata &metadata, std::string &errorMessage) {
  errorMessage.clear();
  if (audioPath.empty()) {
    errorMessage = "Music audio path is empty.";
    return false;
  }

  const TrackMetadata normalized = normalizedMetadata(metadata, audioPath);
  TrackMetadata visibleMetadata;
  {
    std::lock_guard<std::mutex> lock(gMetadataMutex);
    visibleMetadata =
        applyMetadataVisibility(normalized, gMetadataVisibility);
  }
  const std::string audioPathUtf8 = pathToUtf8(audioPath);
  bool loaded = false;

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  IOSNativeMusicMetadata iosMetadata{.title = visibleMetadata.title,
                                     .artist = visibleMetadata.artist,
                                     .album = visibleMetadata.album,
                                     .artworkPath =
                                         pathToUtf8(
                                             visibleMetadata.artworkPath),
                                     .durationMicros =
                                         visibleMetadata.durationMicros};
  loaded = LoadIOSNativeMusicFile(audioPathUtf8, iosMetadata, errorMessage);
#elif TARGET_OS_ANDROID
  AndroidNativeMusicMetadata androidMetadata{.title = visibleMetadata.title,
                                             .artist = visibleMetadata.artist,
                                             .album = visibleMetadata.album,
                                             .artworkPath =
                                                 pathToUtf8(
                                                     visibleMetadata.artworkPath),
                                             .durationMicros =
                                                 visibleMetadata.durationMicros};
  loaded =
      LoadAndroidNativeMusicFile(audioPathUtf8, androidMetadata, errorMessage);
#else
  (void)audioPathUtf8;
  errorMessage = "Native music playback is not supported on this platform.";
  return false;
#endif
  if (loaded) {
    std::lock_guard<std::mutex> lock(gMetadataMutex);
    gLastRawMetadata = normalized;
  }
  return loaded;
}

bool Play(std::string &errorMessage) {
  errorMessage.clear();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return PlayIOSNativeMusic(errorMessage);
#elif TARGET_OS_ANDROID
  return PlayAndroidNativeMusic(errorMessage);
#else
  errorMessage = "Native music playback is not supported on this platform.";
  return false;
#endif
}

bool Pause(std::string &errorMessage) {
  errorMessage.clear();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return PauseIOSNativeMusic(errorMessage);
#elif TARGET_OS_ANDROID
  return PauseAndroidNativeMusic(errorMessage);
#else
  errorMessage = "Native music playback is not supported on this platform.";
  return false;
#endif
}

bool Stop(std::string &errorMessage) {
  errorMessage.clear();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return StopIOSNativeMusic(errorMessage);
#elif TARGET_OS_ANDROID
  return StopAndroidNativeMusic(errorMessage);
#else
  errorMessage = "Native music playback is not supported on this platform.";
  return false;
#endif
}

bool Seek(long long positionMicros, std::string &errorMessage) {
  errorMessage.clear();
  positionMicros = std::max(0LL, positionMicros);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return SeekIOSNativeMusic(positionMicros, errorMessage);
#elif TARGET_OS_ANDROID
  return SeekAndroidNativeMusic(positionMicros, errorMessage);
#else
  errorMessage = "Native music playback is not supported on this platform.";
  return false;
#endif
}

PlaybackState GetState() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNativeMusicState state = GetIOSNativeMusicState();
  return {.supported = true,
          .loaded = state.loaded,
          .playing = state.playing,
          .positionMicros = state.positionMicros,
          .durationMicros = state.durationMicros};
#elif TARGET_OS_ANDROID
  const AndroidNativeMusicState state = GetAndroidNativeMusicState();
  return {.supported = true,
          .loaded = state.loaded,
          .playing = state.playing,
          .positionMicros = state.positionMicros,
          .durationMicros = state.durationMicros};
#else
  return {};
#endif
}

void NotifyControlEvent(ControlEvent event) {
  std::lock_guard<std::mutex> lock(gControlEventsMutex);
  if (gControlEvents.size() < 32) {
    gControlEvents.push_back(event);
  }
}

std::vector<ControlEvent> DrainControlEvents() {
  std::vector<ControlEvent> events;
  std::lock_guard<std::mutex> lock(gControlEventsMutex);
  events.swap(gControlEvents);
  return events;
}

} // namespace native_music_player
