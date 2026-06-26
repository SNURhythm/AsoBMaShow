#pragma once

#include "../audio/MusicPlaylist.h"
#include "../bms_parser.hpp"
#include "../view/RecyclerView.h"
#include "Scene.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  enum class MusicPlayerTab { Library, Favorites, Playlists, Player };
  enum class TrackBrowserKind { Library, Favorites };

  void buildView();
  void buildLibraryPage(View *page);
  void buildFavoritesPage(View *page);
  void buildTrackBrowserPage(View *page, TrackBrowserKind kind);
  void buildPlaylistsPage(View *page);
  void buildPlayerPage(View *page);
  void buildVideoOverlay();
  View *makePanel(const std::string &title, TextView **subtitleText = nullptr);
  Button *makeButton(const std::string &label, int fontSize,
                     TextView **textOut = nullptr);
  Button *makeIconButton(uint32_t iconCodepoint, int fontSize,
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
  void applyLibraryFilterForTrackId(const std::string &preferredTrackId,
                                    bool preserveScroll = false,
                                    bool revealPreferredIfOutOfView = false);
  void refreshTrackBrowserList(TrackBrowserKind kind,
                               int preferredIndex = -1);
  void applyTrackBrowserFilter(TrackBrowserKind kind,
                               int preferredIndex = -1);
  void applyTrackBrowserFilterForTrackId(
      TrackBrowserKind kind, const std::string &preferredTrackId,
      bool preserveScroll = false, bool revealPreferredIfOutOfView = false);
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
  void refreshTrackBrowserArtwork(
      TrackBrowserKind kind, const std::optional<MusicTrack> &track);
  void setStatus(std::string message);

  [[nodiscard]] std::vector<MusicTrack> &
  trackBrowserSourceTracks(TrackBrowserKind kind);
  [[nodiscard]] const std::vector<MusicTrack> &
  trackBrowserSourceTracks(TrackBrowserKind kind) const;
  [[nodiscard]] std::vector<MusicTrack> &
  trackBrowserFilteredTracks(TrackBrowserKind kind);
  [[nodiscard]] const std::vector<MusicTrack> &
  trackBrowserFilteredTracks(TrackBrowserKind kind) const;
  [[nodiscard]] int &trackBrowserSelectedIndex(TrackBrowserKind kind);
  [[nodiscard]] int trackBrowserSelectedIndex(TrackBrowserKind kind) const;
  [[nodiscard]] std::string &trackBrowserSearchText(TrackBrowserKind kind);
  [[nodiscard]] const std::string &
  trackBrowserSearchText(TrackBrowserKind kind) const;
  [[nodiscard]] RecyclerView<MusicTrack> *
  trackBrowserList(TrackBrowserKind kind) const;
  [[nodiscard]] TextView *trackBrowserSubtitleText(TrackBrowserKind kind) const;
  [[nodiscard]] TextView *
  trackBrowserSelectionTitleText(TrackBrowserKind kind) const;
  [[nodiscard]] TextView *
  trackBrowserSelectionDetailText(TrackBrowserKind kind) const;
  [[nodiscard]] ImageView *trackBrowserArtworkImage(TrackBrowserKind kind) const;
  [[nodiscard]] TextView *
  trackBrowserArtworkFallbackText(TrackBrowserKind kind) const;
  [[nodiscard]] std::filesystem::path &
  trackBrowserDisplayedArtworkPath(TrackBrowserKind kind);
  std::optional<MusicTrack> selectedLibraryTrack() const;
  std::optional<MusicTrack>
  selectedTrackBrowserTrack(TrackBrowserKind kind) const;
  std::optional<PlaylistInfo> selectedLibraryPlaylistInfo() const;
  std::optional<PlaylistInfo> selectedPlaylistInfo() const;
  std::optional<MusicTrack> selectedPlaylistTrack() const;
  std::optional<MusicTrack> displayTrack() const;
  PlaylistInfo nowPlayingPlaylistInfo() const;
  int playlistChoiceIndexForId(int playlistId) const;
  int trackIndexInList(const std::vector<MusicTrack> &tracks,
                       const MusicTrack &track) const;
  bool selectedPlaylistIsActiveQueue() const;
  bool isFavoriteTrack(const MusicTrack &track) const;
  void rebuildFavoriteTrackIds();
  void rebindFavoriteAwareTrackLists();
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
  void selectTrackBrowserTrack(TrackBrowserKind kind, int index);
  void persistPlaylistSelection();
  void toggleSelectedLibraryGroup();
  void toggleFavorite(const MusicTrack &track);
  void selectPlaylist(int index);
  void selectPlaylistTrack(int index);
  void selectQueueTrack(int index);
  void addLibraryTrackToPlaylist();
  void addTrackBrowserTrackToPlaylist(TrackBrowserKind kind);
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
  void playTrackBrowserTrack(TrackBrowserKind kind);
  void playPlaylist();
  void playSelectedPlaylistTrack();
  void playSelectedQueueTrack();
  void playRandomLibrary();
  void playRandomTrackBrowser(TrackBrowserKind kind);
  void togglePlayback();
  void cycleRepeatMode();
  void toggleSystemPlaybackJacket();
  void toggleSystemPlaybackTitle();
  void toggleSystemPlaybackArtist();
  void applySystemPlaybackPrivacy(bool persist);
  void refreshSystemPlaybackPrivacyButtons();
  void seekRelative(long long deltaMicros);
  void seekToFraction(float fraction);
  bool handleSeekEvents(SDL_Event &event);
  bool handleProgressSeekEvents(SDL_Event &event, View *progressTrack,
                                bool &mouseDown,
                                SDL_FingerID &activeTouchId);
  void watchVideo();
  bool loadVideoVisualsForTrack(const MusicTrack &track,
                                bool showStatusMessage);
  void exitVideoFullscreen();
  void updateVideoFullscreen();
  void refreshVideoOverlay();
  void showVideoArtwork(const MusicTrack &track);
  void hideVideoArtwork();
  void layoutVideoArtwork();
  void showVideoControls(Uint64 durationMs = 4000);
  void hideVideoControls();
  bool handleVideoFullscreenEvents(SDL_Event &event);
  void playNext();
  void playPrevious();
  void stopPlayback();
  void goBack();

  View *rootLayout = nullptr;
  View *videoOverlayRoot = nullptr;
  View *videoArtworkBackdrop = nullptr;
  View *videoControlsPanel = nullptr;
  View *libraryPage = nullptr;
  View *favoritesPage = nullptr;
  View *playlistsPage = nullptr;
  View *playerPage = nullptr;
  Button *libraryNavButton = nullptr;
  Button *favoritesNavButton = nullptr;
  Button *playlistsNavButton = nullptr;
  Button *playerNavButton = nullptr;
  TextView *libraryNavText = nullptr;
  TextView *favoritesNavText = nullptr;
  TextView *playlistsNavText = nullptr;
  TextView *playerNavText = nullptr;
  TextView *statusText = nullptr;
  TextView *librarySubtitleText = nullptr;
  TextView *favoritesSubtitleText = nullptr;
  TextView *playlistSubtitleText = nullptr;
  TextView *playerSubtitleText = nullptr;
  TextView *librarySelectionTitleText = nullptr;
  TextView *librarySelectionDetailText = nullptr;
  TextView *libraryArtworkFallbackText = nullptr;
  TextView *favoritesSelectionTitleText = nullptr;
  TextView *favoritesSelectionDetailText = nullptr;
  TextView *favoritesArtworkFallbackText = nullptr;
  TextView *playlistSelectionTitleText = nullptr;
  TextView *playlistSelectionDetailText = nullptr;
  TextView *currentTitleText = nullptr;
  TextView *currentDetailText = nullptr;
  TextView *playbackText = nullptr;
  TextView *libraryGroupButtonText = nullptr;
  TextView *queueTitleText = nullptr;
  TextView *repeatModeButtonText = nullptr;
  TextView *watchVideoButtonText = nullptr;
  Button *systemPlaybackJacketButton = nullptr;
  Button *systemPlaybackTitleButton = nullptr;
  Button *systemPlaybackArtistButton = nullptr;
  TextView *systemPlaybackJacketText = nullptr;
  TextView *systemPlaybackTitleText = nullptr;
  TextView *systemPlaybackArtistText = nullptr;
  TextView *artworkFallbackText = nullptr;
  TextView *videoTitleText = nullptr;
  TextView *videoDetailText = nullptr;
  TextView *videoPlaybackText = nullptr;
  TextView *videoPlayPauseButtonText = nullptr;
  TextView *videoArtworkFallbackText = nullptr;
  ImageView *artworkImage = nullptr;
  ImageView *videoArtworkImage = nullptr;
  ImageView *libraryArtworkImage = nullptr;
  ImageView *favoritesArtworkImage = nullptr;
  RecyclerView<MusicTrack> *libraryList = nullptr;
  RecyclerView<MusicTrack> *favoritesList = nullptr;
  RecyclerView<PlaylistInfo> *libraryPlaylistList = nullptr;
  RecyclerView<PlaylistInfo> *favoritesPlaylistList = nullptr;
  RecyclerView<PlaylistInfo> *playlistDirectoryList = nullptr;
  RecyclerView<MusicTrack> *playlistList = nullptr;
  RecyclerView<MusicTrack> *playerQueueList = nullptr;
  TextInputBox *librarySearchInput = nullptr;
  TextInputBox *favoritesSearchInput = nullptr;
  TextInputBox *playlistNameInput = nullptr;
  TextInputBox *playlistRenameInput = nullptr;
  TextView *playPauseButtonText = nullptr;
  TextView *deletePlaylistButtonText = nullptr;
  View *seekProgressTrack = nullptr;
  View *seekProgressFill = nullptr;
  View *videoProgressTrack = nullptr;
  View *videoProgressFill = nullptr;

  std::vector<MusicTrack> libraryTracks;
  std::vector<MusicTrack> filteredLibraryTracks;
  std::vector<MusicTrack> favoriteTracks;
  std::vector<MusicTrack> filteredFavoriteTracks;
  std::unordered_map<std::string, std::vector<MusicTrack>> libraryGroupTracks;
  std::unordered_set<std::string> expandedLibraryGroupIds;
  std::unordered_set<std::string> favoriteTrackIds;
  std::vector<PlaylistInfo> playlists;
  std::vector<PlaylistInfo> playlistChoices;
  std::vector<MusicTrack> playlistTracks;
  std::vector<MusicTrack> queueTracks;
  int selectedLibraryIndex = -1;
  int selectedFavoriteIndex = -1;
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
  std::string favoritesSearchText;
  std::filesystem::path displayedLibraryArtworkPath;
  std::filesystem::path displayedFavoritesArtworkPath;
  std::filesystem::path displayedArtworkPath;
  std::filesystem::path displayedVideoArtworkPath;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  bool seekMouseDown = false;
  SDL_FingerID activeSeekTouchId = -1;
  bool videoSeekMouseDown = false;
  SDL_FingerID activeVideoSeekTouchId = -1;
  bool videoFullscreenActive = false;
  bool videoVisualsLoaded = false;
  bool videoShowingArtwork = false;
  bool videoPreviousVisualsEnabled = true;
  bool videoRestoresVisualsEnabled = false;
  bool videoControlsVisible = false;
  Uint64 videoControlsVisibleUntil = 0;
  std::string videoTrackId;
  std::unique_ptr<bms_parser::Chart> videoChart;
};
