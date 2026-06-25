#pragma once

#include "../audio/MusicPlaylist.h"
#include "../view/RecyclerView.h"
#include "Scene.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class Button;
class ImageView;
class TextInputBox;
class TextView;
class View;

class MusicPlayerScene : public Scene {
public:
  explicit MusicPlayerScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  using MusicTrack = music_playlist::MusicTrack;
  using PlaylistInfo = MusicPlaylistInfo;

  void buildView();
  View *makePanel(const std::string &title, TextView **subtitleText = nullptr);
  Button *makeButton(const std::string &label, int fontSize,
                     TextView **textOut = nullptr);
  void styleButton(Button *button, TextView *text,
                   View::ThemeColorProvider normal,
                   View::ThemeColorProvider hover,
                   View::ThemeColorProvider pressed,
                   View::ThemeColorProvider border);

  void reloadData(bool preserveSelection = true);
  void refreshLibraryList(int preferredIndex = -1);
  void refreshPlaylistDirectoryList(int preferredPlaylistId = 0);
  void refreshPlaylistList(int preferredIndex = -1);
  void refreshUi();
  void refreshArtwork(const std::optional<MusicTrack> &track);
  void setStatus(std::string message);

  std::optional<MusicTrack> selectedLibraryTrack() const;
  std::optional<PlaylistInfo> selectedPlaylistInfo() const;
  std::optional<MusicTrack> selectedPlaylistTrack() const;
  std::optional<MusicTrack> displayTrack() const;
  std::string selectedPlaylistName() const;
  std::string nextPlaylistName() const;

  void createPlaylist();
  void selectPlaylist(int index);
  void addLibraryTrackToPlaylist();
  void removePlaylistTrack();
  void movePlaylistTrack(int delta);
  void clearPlaylist();
  void playLibraryTrack();
  void playPlaylist();
  void playSelectedPlaylistTrack();
  void playRandomLibrary();
  void togglePlayback();
  void seekRelative(long long deltaMicros);
  void playNext();
  void playPrevious();
  void stopPlayback();
  void goBack();

  View *rootLayout = nullptr;
  TextView *statusText = nullptr;
  TextView *librarySubtitleText = nullptr;
  TextView *playlistSubtitleText = nullptr;
  TextView *currentTitleText = nullptr;
  TextView *currentDetailText = nullptr;
  TextView *playbackText = nullptr;
  TextView *artworkFallbackText = nullptr;
  ImageView *artworkImage = nullptr;
  RecyclerView<MusicTrack> *libraryList = nullptr;
  RecyclerView<PlaylistInfo> *playlistDirectoryList = nullptr;
  RecyclerView<MusicTrack> *playlistList = nullptr;
  TextInputBox *playlistNameInput = nullptr;
  TextView *playPauseButtonText = nullptr;

  std::vector<MusicTrack> libraryTracks;
  std::vector<PlaylistInfo> playlists;
  std::vector<MusicTrack> playlistTracks;
  int selectedLibraryIndex = -1;
  int selectedPlaylistDirectoryIndex = -1;
  int selectedPlaylistId = 0;
  int selectedPlaylistIndex = -1;
  std::string statusMessage;
  std::filesystem::path displayedArtworkPath;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
};
