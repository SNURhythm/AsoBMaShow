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
  void applyLibraryFilter(int preferredIndex = -1);
  void rebuildPlaylistChoices();
  void refreshLibraryPlaylistList(int preferredPlaylistId = 0);
  void refreshPlaylistDirectoryList(int preferredPlaylistId = 0);
  void refreshPlaylistList(int preferredIndex = -1);
  void refreshPlaylistSelectionViews();
  void refreshActiveQueueList(bool force = false);
  void refreshUi();
  void refreshNavigation();
  void refreshArtwork(const std::optional<MusicTrack> &track);
  void refreshLibraryArtwork(const std::optional<MusicTrack> &track);
  void setStatus(std::string message);

  std::optional<MusicTrack> selectedLibraryTrack() const;
  std::optional<PlaylistInfo> selectedLibraryPlaylistInfo() const;
  std::optional<PlaylistInfo> selectedPlaylistInfo() const;
  std::optional<MusicTrack> selectedPlaylistTrack() const;
  std::optional<MusicTrack> displayTrack() const;
  PlaylistInfo nowPlayingPlaylistInfo() const;
  int playlistChoiceIndexForId(int playlistId) const;
  int trackIndexInList(const std::vector<MusicTrack> &tracks,
                       const MusicTrack &track) const;
  bool selectedPlaylistIsActiveQueue() const;
  std::string selectedLibraryPlaylistName() const;
  std::string selectedPlaylistName() const;
  std::string nextPlaylistName() const;
  std::string uniquePlaylistName(std::string desiredName) const;

  void switchTab(MusicPlayerTab tab);
  void createPlaylist();
  void saveNowPlayingAsPlaylist();
  void renameSelectedPlaylist();
  void deleteSelectedPlaylist();
  void selectLibraryPlaylist(int index);
  void selectPlaylist(int index);
  void selectPlaylistTrack(int index);
  void selectQueueTrack(int index);
  void addLibraryTrackToPlaylist();
  void addLibraryTrackToNowPlaying(const MusicTrack &track);
  void removePlaylistTrack();
  void movePlaylistTrack(int delta);
  void clearPlaylist();
  void syncActiveQueueAfterPlaylistEdit(
      bool wasActiveQueue, const std::optional<MusicTrack> &previousCurrent,
      int fallbackIndex,
      std::optional<int> previousQueueIndex = std::nullopt,
      std::optional<int> removedIndex = std::nullopt);
  void replaceNowPlaying(std::vector<MusicTrack> tracks, int preferredIndex,
                         const std::string &statusMessage);
  void playNowPlaying(std::vector<MusicTrack> tracks, std::size_t startIndex,
                      const std::string &emptyMessage,
                      const std::string &successMessage);
  void playLibraryTrack();
  void playPlaylist();
  void playSelectedPlaylistTrack();
  void playSelectedQueueTrack();
  void playRandomLibrary();
  void togglePlayback();
  void cycleRepeatMode();
  void seekRelative(long long deltaMicros);
  void seekToFraction(float fraction);
  bool handleSeekEvents(SDL_Event &event);
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
  TextView *statusText = nullptr;
  TextView *librarySubtitleText = nullptr;
  TextView *playlistSubtitleText = nullptr;
  TextView *playerSubtitleText = nullptr;
  TextView *librarySelectionTitleText = nullptr;
  TextView *librarySelectionDetailText = nullptr;
  TextView *libraryArtworkFallbackText = nullptr;
  TextView *playlistSelectionTitleText = nullptr;
  TextView *playlistSelectionDetailText = nullptr;
  TextView *currentTitleText = nullptr;
  TextView *currentDetailText = nullptr;
  TextView *playbackText = nullptr;
  TextView *queueTitleText = nullptr;
  TextView *repeatModeButtonText = nullptr;
  TextView *artworkFallbackText = nullptr;
  ImageView *artworkImage = nullptr;
  ImageView *libraryArtworkImage = nullptr;
  RecyclerView<MusicTrack> *libraryList = nullptr;
  RecyclerView<PlaylistInfo> *libraryPlaylistList = nullptr;
  RecyclerView<PlaylistInfo> *playlistDirectoryList = nullptr;
  RecyclerView<MusicTrack> *playlistList = nullptr;
  RecyclerView<MusicTrack> *playerQueueList = nullptr;
  TextInputBox *librarySearchInput = nullptr;
  TextInputBox *playlistNameInput = nullptr;
  TextInputBox *playlistRenameInput = nullptr;
  TextView *playPauseButtonText = nullptr;
  TextView *deletePlaylistButtonText = nullptr;
  View *seekProgressTrack = nullptr;
  View *seekProgressFill = nullptr;

  std::vector<MusicTrack> libraryTracks;
  std::vector<MusicTrack> filteredLibraryTracks;
  std::vector<PlaylistInfo> playlists;
  std::vector<PlaylistInfo> playlistChoices;
  std::vector<MusicTrack> playlistTracks;
  std::vector<MusicTrack> queueTracks;
  int selectedLibraryIndex = -1;
  int selectedLibraryPlaylistIndex = -1;
  int selectedLibraryPlaylistId = 0;
  int selectedPlaylistDirectoryIndex = -1;
  int selectedPlaylistId = 0;
  int selectedPlaylistIndex = -1;
  int selectedQueueIndex = -1;
  int pendingDeletePlaylistId = 0;
  MusicPlayerTab activeTab = MusicPlayerTab::Library;
  music_playlist::QueueRepeatMode displayedRepeatMode =
      music_playlist::QueueRepeatMode::All;
  std::string displayedQueueName;
  std::string statusMessage;
  std::string librarySearchText;
  std::filesystem::path displayedLibraryArtworkPath;
  std::filesystem::path displayedArtworkPath;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  bool seekMouseDown = false;
  SDL_FingerID activeSeekTouchId = -1;
};
