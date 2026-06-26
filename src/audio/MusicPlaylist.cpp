#include "MusicPlaylist.h"

#include "../ArchiveFile.h"
#include "../path.h"

#include <algorithm>
#include <cctype>
#include <random>
#include <string_view>
#include <utility>

namespace music_playlist {
namespace {

const std::vector<std::string_view> kArtworkExtensions = {
    "png", "jpg", "jpeg", "bmp", "gif", "webp"};

std::string lowerTrimmed(std::string value) {
  value.erase(value.begin(), std::find_if_not(value.begin(), value.end(),
                                              [](unsigned char ch) {
                                                return std::isspace(ch) != 0;
                                              }));
  value.erase(
      std::find_if_not(value.rbegin(), value.rend(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; })
          .base(),
      value.end());
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string normalizedPathString(std::filesystem::path path) {
  if (path.empty()) {
    return {};
  }
  path = path.lexically_normal();
  return path_t_to_utf8(fspath_to_path_t(path));
}

std::string fallbackTitle(const bms_parser::ChartMeta &meta) {
  if (!meta.Title.empty()) {
    return meta.Title;
  }
  if (!meta.BmsPath.empty()) {
    return path_t_to_utf8(fspath_to_path_t(meta.BmsPath.stem()));
  }
  return "Untitled";
}

std::string combineArtists(const std::string &artist,
                           const std::string &subArtist) {
  if (artist.empty()) {
    return subArtist;
  }
  if (subArtist.empty() || lowerTrimmed(artist) == lowerTrimmed(subArtist)) {
    return artist;
  }
  return artist + " / " + subArtist;
}

long long durationForChart(const bms_parser::ChartMeta &meta) {
  if (meta.TotalLength > 0) {
    return meta.TotalLength;
  }
  return std::max(0LL, meta.PlayLength);
}

std::filesystem::path nativeArtworkPathForTrack(const MusicTrack &track) {
  if (track.artworkPath.empty()) {
    return {};
  }
  std::string errorMessage;
  const auto materialized =
      archive_file::materializeFile(track.artworkPath, &errorMessage);
  return materialized.value_or(std::filesystem::path{});
}

std::uint64_t generateSeed() {
  static std::random_device device;
  const auto high = static_cast<std::uint64_t>(device()) << 32u;
  return high ^ static_cast<std::uint64_t>(device());
}

} // namespace

std::string TrackIdForChart(const bms_parser::ChartMeta &meta) {
  const std::string folder = normalizedPathString(meta.Folder);
  if (!folder.empty()) {
    return "folder:" + folder;
  }
  return "path:" + normalizedPathString(meta.BmsPath);
}

std::string ChartIdForChart(const bms_parser::ChartMeta &meta) {
  const std::string sha256 = lowerTrimmed(meta.SHA256);
  if (!sha256.empty()) {
    return "sha256:" + sha256;
  }
  const std::string md5 = lowerTrimmed(meta.MD5);
  if (!md5.empty()) {
    return "md5:" + md5;
  }
  return "path:" + normalizedPathString(meta.BmsPath);
}

std::filesystem::path ArtworkPathForChart(const bms_parser::ChartMeta &meta) {
  const std::filesystem::path candidates[] = {
      meta.StageFile,
      meta.Banner,
      meta.BackBmp,
  };
  for (const auto &candidate : candidates) {
    if (candidate.empty()) {
      continue;
    }
    if (const auto resolved =
            archive_file::findFileWithExtensions(meta.Folder / candidate,
                                                 kArtworkExtensions)) {
      return *resolved;
    }
  }

  const std::string commonNames[] = {
      "jacket", "cover", "folder", "title", "stagefile", "stage", "banner"};
  for (const auto &name : commonNames) {
    if (const auto resolved =
            archive_file::findFileWithExtensions(meta.Folder / name,
                                                 kArtworkExtensions)) {
      return *resolved;
    }
  }
  return {};
}

MusicTrack MakeTrack(const MusicTrackRecord &record) {
  const auto &meta = record.representativeChart;
  return {
      .trackId = TrackIdForChart(meta),
      .chartId = ChartIdForChart(meta),
      .representativeChart = meta,
      .title = fallbackTitle(meta),
      .subtitle = meta.SubTitle,
      .artist = meta.Artist,
      .subArtist = meta.SubArtist,
      .genre = meta.Genre,
      .artworkPath = ArtworkPathForChart(meta),
      .chartCount = std::max(1, record.chartCount),
      .durationMicros = durationForChart(meta),
  };
}

std::vector<MusicTrack>
MakeTracks(const std::vector<MusicTrackRecord> &records) {
  std::vector<MusicTrack> tracks;
  tracks.reserve(records.size());
  for (const auto &record : records) {
    tracks.push_back(MakeTrack(record));
  }
  return tracks;
}

std::vector<MusicTrack>
ShuffledTracks(std::vector<MusicTrack> tracks,
               std::optional<std::uint64_t> seed) {
  std::mt19937_64 engine(seed.value_or(generateSeed()));
  std::shuffle(tracks.begin(), tracks.end(), engine);
  return tracks;
}

native_music_player::TrackMetadata MakeNativeMetadata(const MusicTrack &track) {
  std::string title = track.title;
  if (!track.subtitle.empty()) {
    title += " " + track.subtitle;
  }
  return {
      .title = std::move(title),
      .artist = combineArtists(track.artist, track.subArtist),
      .album = track.genre,
      .artworkPath = nativeArtworkPathForTrack(track),
      .durationMicros = track.durationMicros,
  };
}

void MusicQueue::SetPlaylist(std::vector<MusicTrack> newTracks,
                             std::size_t startIndex,
                             std::string newDisplayName) {
  displayName = std::move(newDisplayName);
  tracks = std::move(newTracks);
  currentIndex.reset();
  detachedCurrentNextIndex.reset();
  if (!tracks.empty()) {
    currentIndex = std::min(startIndex, tracks.size() - 1);
  }
}

void MusicQueue::SetPlaylistAfterCurrentRemoved(
    std::vector<MusicTrack> newTracks, std::size_t nextIndex,
    std::string newDisplayName) {
  displayName = std::move(newDisplayName);
  tracks = std::move(newTracks);
  currentIndex.reset();
  detachedCurrentNextIndex.reset();
  if (!tracks.empty()) {
    const std::size_t clampedNextIndex = std::min(nextIndex, tracks.size());
    detachedCurrentNextIndex = clampedNextIndex;
  }
}

void MusicQueue::Clear() {
  tracks.clear();
  currentIndex.reset();
  detachedCurrentNextIndex.reset();
  displayName.clear();
}

const MusicTrack *MusicQueue::Current() const {
  if (!currentIndex || *currentIndex >= tracks.size()) {
    return nullptr;
  }
  return &tracks[*currentIndex];
}

MusicQueueSnapshot MusicQueue::Snapshot() const {
  MusicQueueSnapshot snapshot;
  snapshot.repeatMode = repeatMode;
  snapshot.displayName = displayName;
  snapshot.tracks = tracks;
  snapshot.currentIndex = currentIndex;
  snapshot.detachedCurrentNextIndex = detachedCurrentNextIndex;
  return snapshot;
}

const MusicTrack *MusicQueue::Next() {
  if (tracks.empty()) {
    return nullptr;
  }

  if (detachedCurrentNextIndex) {
    const std::size_t nextIndex = *detachedCurrentNextIndex;
    if (nextIndex >= tracks.size()) {
      if (repeatMode != QueueRepeatMode::All) {
        detachedCurrentNextIndex.reset();
        return nullptr;
      }
      return SelectIndex(0);
    }
    return SelectIndex(nextIndex);
  }

  if (repeatMode == QueueRepeatMode::One) {
    return Current();
  }

  if (!currentIndex) {
    return SelectIndex(0);
  }
  if (*currentIndex + 1 >= tracks.size()) {
    if (repeatMode != QueueRepeatMode::All) {
      return nullptr;
    }
    return SelectIndex(0);
  }
  const std::size_t index = *currentIndex + 1;
  return SelectIndex(index);
}

const MusicTrack *MusicQueue::Previous() {
  if (tracks.empty()) {
    return nullptr;
  }

  if (detachedCurrentNextIndex) {
    const std::size_t nextIndex = *detachedCurrentNextIndex;
    if (nextIndex == 0) {
      if (repeatMode != QueueRepeatMode::All) {
        detachedCurrentNextIndex.reset();
        return nullptr;
      }
      return SelectIndex(tracks.size() - 1);
    }
    return SelectIndex(nextIndex - 1);
  }

  if (repeatMode == QueueRepeatMode::One) {
    return Current();
  }

  if (!currentIndex) {
    return SelectIndex(0);
  }
  if (*currentIndex == 0) {
    if (repeatMode != QueueRepeatMode::All) {
      return nullptr;
    }
    return SelectIndex(tracks.size() - 1);
  }
  const std::size_t index = *currentIndex - 1;
  return SelectIndex(index);
}

const MusicTrack *MusicQueue::SelectIndex(std::size_t index) {
  if (index >= tracks.size()) {
    currentIndex.reset();
    detachedCurrentNextIndex.reset();
    return nullptr;
  }
  currentIndex = index;
  detachedCurrentNextIndex.reset();
  return &tracks[index];
}

} // namespace music_playlist
