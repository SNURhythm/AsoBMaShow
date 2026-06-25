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
  enum class MusicPlayerTab { Library, Playlists, Player };

  void buildView();
  void buildLibraryPage(View *page);
  void buildPlaylistsPage(View *page);
  void buildPlayerPage(View *page);
  View *makePanel(const std::string &title, TextView **subtitleText = nullptr);
  Button *makeButton(const std::string &label, int fontSize,
                     TextView **textOut = nullptr);
  Button *makeNavButton(const std::string &label, TextView **textOut);
  void styleButton(Button *button, TextView *text,
                   View::ThemeColorProvider normal,
                   View::ThemeColorProvider hover,
                   View::ThemeColorProvider pressed,
                   View::ThemeColorProvider border);

  void reloadData(bool preserveSelection = true);
  void refreshLibraryList(int preferredIndex = -1);
  void refreshPlaylistDirectoryList(int preferredPlaylistId = 0);
  void refreshPlaylistList(int preferredIndex = -1);
  void refreshPlaylistSelectionViews();
  void refreshUi();
  void refreshNavigation();
  void refreshArtwork(const std::optional<MusicTrack> &track);
  void setStatus(std::string message);

  std::optional<MusicTrack> selectedLibraryTrack() const;
  std::optional<PlaylistInfo> selectedPlaylistInfo() const;
  std::optional<MusicTrack> selectedPlaylistTrack() const;
  std::optional<MusicTrack> displayTrack() const;
  std::string selectedPlaylistName() const;
  std::string nextPlaylistName() const;

  void switchTab(MusicPlayerTab tab);
  void createPlaylist();
  void selectPlaylist(int index);
  void selectPlaylistTrack(int index);
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
  View *libraryPage = nullptr;
  View *playlistsPage = nullptr;
  View *playerPage = nullptr;
  Button *libraryNavButton = nullptr;
  Button *playlistsNavButton = nullptr;
  Button *playerNavButton = nullptr;
  TextView *libraryNavText = nullptr;
  TextView *playlistsNavText = nullptr;
  TextView *playerNavText = nullptr;
  TextView *railSummaryText = nullptr;
  TextView *statusText = nullptr;
  TextView *librarySubtitleText = nullptr;
  TextView *playlistSubtitleText = nullptr;
  TextView *playerSubtitleText = nullptr;
  TextView *librarySelectionTitleText = nullptr;
  TextView *librarySelectionDetailText = nullptr;
  TextView *playlistSelectionTitleText = nullptr;
  TextView *playlistSelectionDetailText = nullptr;
  TextView *currentTitleText = nullptr;
  TextView *currentDetailText = nullptr;
  TextView *playbackText = nullptr;
  TextView *artworkFallbackText = nullptr;
  ImageView *artworkImage = nullptr;
  RecyclerView<MusicTrack> *libraryList = nullptr;
  RecyclerView<PlaylistInfo> *playlistDirectoryList = nullptr;
  RecyclerView<MusicTrack> *playlistList = nullptr;
  RecyclerView<MusicTrack> *playerQueueList = nullptr;
  TextInputBox *playlistNameInput = nullptr;
  TextView *playPauseButtonText = nullptr;

  std::vector<MusicTrack> libraryTracks;
  std::vector<PlaylistInfo> playlists;
  std::vector<MusicTrack> playlistTracks;
  int selectedLibraryIndex = -1;
  int selectedPlaylistDirectoryIndex = -1;
  int selectedPlaylistId = 0;
  int selectedPlaylistIndex = -1;
  MusicPlayerTab activeTab = MusicPlayerTab::Library;
  std::string statusMessage;
  std::filesystem::path displayedArtworkPath;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
};
