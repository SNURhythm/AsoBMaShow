#pragma once
#include "View.h"
#include "../audio/MusicPlaylist.h"
#include <functional>
class ImageView;
class TextView;
class Button;
std::string trackTitle(const music_playlist::MusicTrack &);
std::string trackArtist(const music_playlist::MusicTrack &);
std::string trackDetail(const music_playlist::MusicTrack &);
std::filesystem::path artworkPathForDisplay(const music_playlist::MusicTrack &);

class MusicTrackRowView : public View {
public:
  struct TrailingAction {
    std::string text;
    bool highlighted = false;
    std::function<void()> onClick;
  };
  MusicTrackRowView();
  void setTrack(const music_playlist::MusicTrack &, bool selected,
                TrailingAction action, std::string detailOverride = {});
  void setTrailingActionSize(float size);
  void onSelected() override;
  void onUnselected() override;
private:
  bool highlighted_ = false;
  bool isSelected_ = false;
  ImageView *artwork = nullptr;
  TextView *artworkFallback = nullptr;
  TextView *title = nullptr;
  TextView *detail = nullptr;
  Button *trailingButton = nullptr;
  TextView *trailingText = nullptr;
};
