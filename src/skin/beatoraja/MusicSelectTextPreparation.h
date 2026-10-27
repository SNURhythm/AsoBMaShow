#pragma once

#include "MusicSelectBarRenderer.h"

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace skin {

// The title projection of MusicSelectBarRenderer::plan. Destination coordinates,
// movement and other draw families do not affect title residency. Keep selection
// rules in sync with the renderer; parity coverage lives in its existing tests.
template <class Visitor>
void visitMusicSelectTitles(const SkinSongListObject &songList,
                            const MusicSelectSongListFrame &frame,
                            Visitor &&visit) {
  if (frame.size() == 0) return;
  const auto textExists = [&](int slot) {
    return slot >= 0 && slot < 11 &&
           static_cast<std::size_t>(slot) < songList.text.size() &&
           songList.text[slot].object != 0;
  };
  const auto count = static_cast<std::int64_t>(frame.size());
  for (std::size_t row = 0; row < MusicSelectBarRenderer::barCount; ++row) {
    const auto &bars = static_cast<int>(row) == songList.center
                           ? songList.listOn : songList.listOff;
    if (row >= bars.size() || bars[row].object == 0 ||
        bars[row].destination.frames.empty()) continue;
    const auto raw = static_cast<std::int64_t>(frame.selectedIndex) +
                     static_cast<std::int64_t>(row) - songList.center;
    const auto index = static_cast<std::size_t>((raw % count + count) % count);
    const auto &bar = frame.at(index);
    int value = -1;
    switch (bar.kind) {
    case MusicSelectBarKind::Song: value = bar.exists ? 0 : 4; break;
    case MusicSelectBarKind::Folder: value = 1; break;
    case MusicSelectBarKind::Table:
    case MusicSelectBarKind::Hash:
    case MusicSelectBarKind::Executable: value = 2; break;
    case MusicSelectBarKind::Grade: value = bar.exists ? 3 : 4; break;
    case MusicSelectBarKind::RandomCourse: value = bar.exists ? 2 : 4; break;
    case MusicSelectBarKind::Command:
    case MusicSelectBarKind::Container: value = 5; break;
    case MusicSelectBarKind::SearchWord: value = 6; break;
    case MusicSelectBarKind::SameFolder: continue;
    }
    int text = value;
    if (value >= 2) {
      text += 4;
      if (!textExists(text)) text = 0;
    } else {
      const bool old = frame.wallClockSeconds > bar.addDateSeconds + 86'400;
      text = value == 0 ? (old ? 2 : 3) : (old ? 4 : 5);
      if (!textExists(text)) text = old ? 0 : 1;
    }
    if (textExists(text)) visit(songList.text[text].object, index, bar.title);
  }
}

struct PreparedSkinTextAtlas;

// Successful checks only. The owner must invalidate on any atlas mutation,
// including metrics-only updates and updates to an atlas shared by objects.
class MusicSelectTextResidencyCache {
public:
  static constexpr std::size_t maximumEntries = 512;
  static constexpr std::size_t maximumBytes = 128 * 1024;
  static constexpr std::size_t maximumStringBytes = 4096;

  template <class Check>
  bool contains(const PreparedSkinTextAtlas *atlas, std::string_view text,
                Check &&check) {
    if (atlas == nullptr) return false;
    const auto found = strings_.find(atlas);
    if (found != strings_.end() && found->second.contains(text)) return true;
    ++validationCount_;
    if (!check()) return false;
    if (text.size() > maximumStringBytes) return true;
    if (entries_ == maximumEntries || text.size() > maximumBytes - bytes_) {
      invalidate();
    }
    strings_[atlas].emplace(text);
    ++entries_;
    bytes_ += text.size();
    return true;
  }

  void invalidate() {
    strings_.clear();
    entries_ = 0;
    bytes_ = 0;
  }
  [[nodiscard]] std::size_t validationCount() const { return validationCount_; }
  [[nodiscard]] std::size_t entryCount() const { return entries_; }
  [[nodiscard]] std::size_t byteCount() const { return bytes_; }

private:
  std::map<const PreparedSkinTextAtlas *, std::set<std::string, std::less<>>>
      strings_;
  std::size_t entries_ = 0;
  std::size_t bytes_ = 0;
  std::size_t validationCount_ = 0;
};

} // namespace skin
