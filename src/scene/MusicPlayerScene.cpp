#include "MusicPlayerScene.h"

#include "../PlayOptionUtils.h"
#include "../path.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ImageView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "SceneManager.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iterator>
#include <sstream>
#include <utility>

namespace {

constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr float kScreenPadding = 18.0f;
constexpr float kHeaderHeight = 82.0f;
constexpr float kRailWidth = 180.0f;
constexpr int kTrackRowHeight = 82;
constexpr int kPlaylistRowHeight = 58;
constexpr int kNowPlayingPlaylistId = -1;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(normalized.top * rendering::window_height);
  insets.left = static_cast<int>(normalized.left * rendering::window_width);
  insets.bottom = static_cast<int>(normalized.bottom * rendering::window_height);
  insets.right = static_cast<int>(normalized.right * rendering::window_width);
#endif
  return insets;
}

std::string trackTitle(const music_playlist::MusicTrack &track) {
  std::string title = track.title.empty() ? "Untitled" : track.title;
  if (!track.subtitle.empty()) {
    title += " " + track.subtitle;
  }
  return title;
}

std::string trackArtist(const music_playlist::MusicTrack &track) {
  if (!track.artist.empty() && !track.subArtist.empty() &&
      track.artist != track.subArtist) {
    return track.artist + " / " + track.subArtist;
  }
  if (!track.artist.empty()) {
    return track.artist;
  }
  if (!track.subArtist.empty()) {
    return track.subArtist;
  }
  return "Unknown artist";
}

std::string trackDetail(const music_playlist::MusicTrack &track) {
  std::string detail = trackArtist(track);
  if (!track.genre.empty()) {
    detail += "  " + track.genre;
  }
  if (track.groupRepresentative && track.chartCount > 1) {
    detail += "  " + std::to_string(track.chartCount) + " charts";
  } else if (track.expandedChart) {
    detail += "  chart";
  }
  return detail;
}

std::string formatMusicTime(long long micros) {
  micros = std::max(0LL, micros);
  const long long totalSeconds = micros / 1000000LL;
  const long long minutes = totalSeconds / 60LL;
  const long long seconds = totalSeconds % 60LL;
  std::ostringstream stream;
  stream << minutes << ':';
  if (seconds < 10) {
    stream << '0';
  }
  stream << seconds;
  return stream.str();
}

std::string trimPlaylistName(const std::string &value) {
  const auto begin =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
  const auto end =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
      }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

std::string lowercaseText(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool containsText(const std::string &haystack, const std::string &needle) {
  return lowercaseText(haystack).find(needle) != std::string::npos;
}

bool trackMatchesSearch(const music_playlist::MusicTrack &track,
                        const std::string &query) {
  if (query.empty()) {
    return true;
  }
  const auto &meta = track.representativeChart;
  const std::string searchable =
      track.title + " " + track.subtitle + " " + track.artist + " " +
      track.subArtist + " " + track.genre + " " + track.trackId + " " +
      track.chartId + " " + meta.Title + " " + meta.SubTitle + " " +
      meta.Artist + " " + meta.SubArtist + " " + meta.Genre + " " +
      meta.MD5 + " " + meta.SHA256 + " " +
      path_t_to_utf8(fspath_to_path_t(meta.BmsPath)) + " " +
      path_t_to_utf8(fspath_to_path_t(meta.Folder));
  return containsText(searchable, query);
}

bool sameTrackIdentity(const music_playlist::MusicTrack &a,
                       const music_playlist::MusicTrack &b) {
  return a.trackId == b.trackId && a.chartId == b.chartId;
}

bool sameTrackList(const std::vector<music_playlist::MusicTrack> &a,
                   const std::vector<music_playlist::MusicTrack> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  return std::equal(a.begin(), a.end(), b.begin(), sameTrackIdentity);
}

std::string favoriteKeyForTrack(const music_playlist::MusicTrack &track) {
  return music_playlist::ChartTrackIdForChart(track.representativeChart);
}

std::string repeatModeLabel(music_playlist::QueueRepeatMode mode) {
  switch (mode) {
  case music_playlist::QueueRepeatMode::One:
    return "1-Loop";
  case music_playlist::QueueRepeatMode::All:
    return "Playlist Loop";
  case music_playlist::QueueRepeatMode::None:
  default:
    return "Loop Off";
  }
}

std::string queueDisplayName(const std::string &name) {
  return name.empty() ? music_playlist::kNowPlayingDisplayName : name;
}

bool isNowPlayingPlaylistId(int playlistId) {
  return playlistId == kNowPlayingPlaylistId;
}

bool chartHasBgaEvents(const bms_parser::Chart &chart) {
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline != nullptr &&
          (timeline->BgaBase != -1 || timeline->BgaLayer != -1)) {
        return true;
      }
    }
  }
  return false;
}

int adjustedDetachedNextIndex(std::optional<std::size_t> previousNextIndex,
                              std::optional<int> removedIndex,
                              int fallbackIndex, std::size_t nextSize) {
  int index = previousNextIndex
                  ? static_cast<int>(*previousNextIndex)
                  : fallbackIndex;
  if (removedIndex && previousNextIndex && *removedIndex < index) {
    --index;
  }
  return std::clamp(index, 0, static_cast<int>(nextSize));
}

std::filesystem::path artworkPathForDisplay(
    const music_playlist::MusicTrack &track) {
  const auto &meta = track.representativeChart;
  if (!meta.StageFile.empty()) {
    return meta.Folder / meta.StageFile;
  }
  if (!meta.Banner.empty()) {
    return meta.Folder / meta.Banner;
  }
  return track.artworkPath;
}

void mouseButtonEventToUi(const SDL_MouseButtonEvent &event, int &uiX,
                          int &uiY) {
  const int screenX = static_cast<int>(event.x * rendering::widthScale);
  const int screenY = static_cast<int>(event.y * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

void mouseMotionEventToUi(const SDL_MouseMotionEvent &event, int &uiX,
                          int &uiY) {
  const int screenX = static_cast<int>(event.x * rendering::widthScale);
  const int screenY = static_cast<int>(event.y * rendering::heightScale);
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

bool isInsideView(const View *view, float uiX, float uiY) {
  return view != nullptr && uiX >= view->getX() &&
         uiX <= view->getX() + view->getWidth() && uiY >= view->getY() &&
         uiY <= view->getY() + view->getHeight();
}

class MusicTrackRowView : public View {
public:
  MusicTrackRowView() {
    setHeight(kTrackRowHeight)
        ->setPadding(Edge::All, 12)
        ->setFlexDirection(FlexDirection::Row)
        ->setAlignItems(YGAlignCenter)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(12)
        ->setCornerRadius(ui_theme::controlRadius())
        ->setBorderWidth(1);

    auto *artworkFrame = new View();
    artworkFrame->setWidth(58)
        ->setHeight(58)
        ->setFlexShrink(0)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setThemedBackgroundColor(ui_theme::insetSurface)
        ->setThemedBorderColor(ui_theme::hairlineSubtle)
        ->setBorderWidth(1)
        ->setCornerRadius(ui_theme::controlRadius());

    artworkFallback = new TextView(kFontPath, 13);
    artworkFallback->setText("ART");
    artworkFallback->setHeight(22);
    artworkFallback->setAlign(TextView::CENTER);
    artworkFallback->setVAlign(TextView::MIDDLE);
    artworkFallback->setThemedColor(ui_theme::textMuted);
    artworkFrame->addView(artworkFallback);

    artwork = new ImageView(0, 0, 0, 0);
    artwork->setWidth(58)
        ->setHeight(58)
        ->setPositionType(YGPositionTypeAbsolute)
        ->setPosition(Edge::Left, 0)
        ->setPosition(Edge::Top, 0)
        ->setCornerRadius(ui_theme::controlRadius());
    artworkFrame->addView(artwork);
    addView(artworkFrame);

    auto *textColumn = new View();
    textColumn->setFlex(1)
        ->setMinWidth(0)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(4);

    title = new TextView(kFontPath, 19);
    title->setHeight(26);
    title->setThemedColor(ui_theme::textPrimary);
    title->setOverflow(TextView::TextOverflow::Hidden);
    textColumn->addView(title);

    detail = new TextView(kFontPath, 15);
    detail->setHeight(22);
    detail->setThemedColor(ui_theme::textSecondary);
    detail->setOverflow(TextView::TextOverflow::Marquee);
    textColumn->addView(detail);
    addView(textColumn);

    favoriteButton = new Button();
    favoriteButton->setWidth(48)
        ->setHeight(48)
        ->setFlexShrink(0)
        ->setCornerRadius(ui_theme::controlRadius());
    favoriteButton
        ->setThemedBackgroundColors(ui_theme::control,
                                    ui_theme::controlHover,
                                    ui_theme::controlPressed)
        ->setThemedBorderColors(ui_theme::hairlineSubtle,
                                ui_theme::hairlineStrong,
                                ui_theme::accentBorderStrong)
        ->setStyledBorderWidth(1);
    favoriteText = new TextView(kFontPath, 25);
    favoriteText->setText("☆");
    favoriteText->setAlign(TextView::CENTER);
    favoriteText->setVAlign(TextView::MIDDLE);
    favoriteText->setThemedColor(ui_theme::textSecondary);
    favoriteButton->setContentView(favoriteText);
    addView(favoriteButton);
  }

  void setTrack(const music_playlist::MusicTrack &track, bool selected,
                bool favorite, std::function<void()> onFavoriteToggle) {
    title->setText(trackTitle(track));
    detail->setText(trackDetail(track));
    favoriteText->setText(favorite ? "★" : "☆");
    favoriteText->setThemedColor(favorite ? ui_theme::amber
                                          : ui_theme::textSecondary);
    favoriteButton->setOnClickListener(std::move(onFavoriteToggle));
    if (artwork != nullptr) {
      const auto path = artworkPathForDisplay(track);
      if (path.empty()) {
        artwork->freeImage();
      } else {
        artwork->setImageAsync(fspath_to_path_t(path), true);
      }
    }
    if (selected) {
      onSelected();
    } else {
      onUnselected();
    }
  }

  void onSelected() override {
    setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
    setThemedBorderColor(ui_theme::accentBorderStrong);
  }

  void onUnselected() override {
    setThemedBackgroundColor(ui_theme::mainMenuItem);
    setThemedBorderColor(ui_theme::hairlineSubtle);
  }

private:
  ImageView *artwork = nullptr;
  TextView *artworkFallback = nullptr;
  TextView *title = nullptr;
  TextView *detail = nullptr;
  Button *favoriteButton = nullptr;
  TextView *favoriteText = nullptr;
};

class MusicSeekProgressFillView : public View {
public:
  MusicSeekProgressFillView() {
    batch.setSubmitView(rendering::ui_view);
    setCornerRadius(std::max(0.0f, ui_theme::controlRadius() - 1.0f));
  }

  void setFraction(float value) {
    fraction = std::clamp(value, 0.0f, 1.0f);
  }

protected:
  void renderImpl(RenderContext &context) override {
    const float fillWidth =
        std::clamp(static_cast<float>(getWidth()) * fraction, 0.0f,
                   static_cast<float>(getWidth()));
    if (fillWidth <= 0.0f || getHeight() <= 0) {
      return;
    }

    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin();
    const float radius =
        std::min(getCornerRadius(),
                 std::min(fillWidth, static_cast<float>(getHeight())) * 0.5f);
    batch.addRoundedRect(static_cast<float>(getX()), static_cast<float>(getY()),
                         fillWidth, static_cast<float>(getHeight()), radius,
                         ui_theme::primaryAction().toABGR());
    batch.end();
  }

private:
  float fraction = 0.0f;
  rendering::SimpleBatchRenderer batch;
};

void setSeekFillFraction(View *view, float fraction) {
  if (auto *fill = dynamic_cast<MusicSeekProgressFillView *>(view)) {
    fill->setFraction(fraction);
  }
}

class PlaylistRowView : public View {
public:
  PlaylistRowView() {
    setHeight(kPlaylistRowHeight)
        ->setPadding(Edge::All, 10)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(2)
        ->setCornerRadius(ui_theme::controlRadius())
        ->setBorderWidth(1);

    title = new TextView(kFontPath, 18);
    title->setHeight(24);
    title->setThemedColor(ui_theme::textPrimary);
    title->setOverflow(TextView::TextOverflow::Hidden);
    addView(title);

    detail = new TextView(kFontPath, 14);
    detail->setHeight(20);
    detail->setThemedColor(ui_theme::textSecondary);
    detail->setOverflow(TextView::TextOverflow::Hidden);
    addView(detail);
  }

  void setPlaylist(const MusicPlaylistInfo &playlist, bool selected) {
    title->setText(playlist.name.empty() ? "Untitled Playlist" : playlist.name);
    detail->setText(std::to_string(playlist.trackCount) + " tracks");
    if (selected) {
      onSelected();
    } else {
      onUnselected();
    }
  }

  void onSelected() override {
    setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
    setThemedBorderColor(ui_theme::accentBorderStrong);
  }

  void onUnselected() override {
    setThemedBackgroundColor(ui_theme::mainMenuItem);
    setThemedBorderColor(ui_theme::hairlineSubtle);
  }

private:
  TextView *title = nullptr;
  TextView *detail = nullptr;
};

} // namespace

void MusicPlayerScene::init() {
  context.jukebox.stop();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  buildView();
  reloadData(false);
}

EventHandleResult MusicPlayerScene::handleEvents(SDL_Event &event) {
  if (videoFullscreenActive) {
    handleVideoFullscreenEvents(event);
    return {};
  }
  if (handleSeekEvents(event)) {
    return {};
  }
  if (event.type == SDL_KEYDOWN) {
    if (event.key.keysym.sym == SDLK_ESCAPE) {
      goBack();
      return {};
    }
    if (event.key.keysym.sym == SDLK_1) {
      switchTab(MusicPlayerTab::Library);
      return {};
    }
    if (event.key.keysym.sym == SDLK_2) {
      switchTab(MusicPlayerTab::Favorites);
      return {};
    }
    if (event.key.keysym.sym == SDLK_3) {
      switchTab(MusicPlayerTab::Playlists);
      return {};
    }
    if (event.key.keysym.sym == SDLK_4) {
      switchTab(MusicPlayerTab::Player);
      return {};
    }
  }
  return Scene::handleEvents(event);
}

void MusicPlayerScene::update(float) {
  if (rootLayout != nullptr &&
      (lastLayoutWidth != rendering::window_width ||
       lastLayoutHeight != rendering::window_height)) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    rootLayout->setSize(rendering::window_width, rendering::window_height);
    rootLayout->applyYogaLayout();
    if (videoOverlayRoot != nullptr) {
      videoOverlayRoot->setSize(rendering::window_width,
                                rendering::window_height);
      videoOverlayRoot->applyYogaLayout();
      layoutVideoArtwork();
    }
  }

  std::string nativeStatus;
  bool queueMayHaveChanged = false;
  if (context.musicPlayer.ProcessNativeControlEvents(nativeStatus)) {
    queueMayHaveChanged = true;
    if (!nativeStatus.empty()) {
      setStatus(nativeStatus);
    }
  }
  if (context.musicPlayer.ConsumeNativeControlStatus(nativeStatus)) {
    queueMayHaveChanged = true;
    if (!nativeStatus.empty()) {
      setStatus(nativeStatus);
    }
  }
  if (queueMayHaveChanged) {
    refreshActiveQueueList(true);
  }
  updateVideoFullscreen();
  refreshUi();
  refreshVideoOverlay();
}

void MusicPlayerScene::renderScene() { updateVideoFullscreen(); }

void MusicPlayerScene::cleanupScene() {
  if (videoVisualsLoaded || videoFullscreenActive) {
    context.jukebox.unloadVisuals();
  }
  context.ignoreBgaPostOptions.store(false, std::memory_order_release);
  if (videoRestoresVisualsEnabled) {
    context.jukebox.setVisualsEnabled(videoPreviousVisualsEnabled);
  }
  rootLayout = nullptr;
  videoOverlayRoot = nullptr;
  videoArtworkBackdrop = nullptr;
  videoControlsPanel = nullptr;
  libraryPage = nullptr;
  favoritesPage = nullptr;
  playlistsPage = nullptr;
  playerPage = nullptr;
  libraryNavButton = nullptr;
  favoritesNavButton = nullptr;
  playlistsNavButton = nullptr;
  playerNavButton = nullptr;
  libraryNavText = nullptr;
  favoritesNavText = nullptr;
  playlistsNavText = nullptr;
  playerNavText = nullptr;
  statusText = nullptr;
  librarySubtitleText = nullptr;
  favoritesSubtitleText = nullptr;
  playlistSubtitleText = nullptr;
  playerSubtitleText = nullptr;
  librarySelectionTitleText = nullptr;
  librarySelectionDetailText = nullptr;
  libraryArtworkFallbackText = nullptr;
  favoritesSelectionTitleText = nullptr;
  favoritesSelectionDetailText = nullptr;
  favoritesArtworkFallbackText = nullptr;
  playlistSelectionTitleText = nullptr;
  playlistSelectionDetailText = nullptr;
  currentTitleText = nullptr;
  currentDetailText = nullptr;
  playbackText = nullptr;
  queueTitleText = nullptr;
  repeatModeButtonText = nullptr;
  watchVideoButtonText = nullptr;
  artworkFallbackText = nullptr;
  videoTitleText = nullptr;
  videoDetailText = nullptr;
  videoPlaybackText = nullptr;
  videoPlayPauseButtonText = nullptr;
  videoArtworkFallbackText = nullptr;
  artworkImage = nullptr;
  videoArtworkImage = nullptr;
  libraryArtworkImage = nullptr;
  favoritesArtworkImage = nullptr;
  libraryList = nullptr;
  favoritesList = nullptr;
  libraryPlaylistList = nullptr;
  favoritesPlaylistList = nullptr;
  playlistDirectoryList = nullptr;
  playlistList = nullptr;
  playerQueueList = nullptr;
  librarySearchInput = nullptr;
  favoritesSearchInput = nullptr;
  playlistNameInput = nullptr;
  playlistRenameInput = nullptr;
  playPauseButtonText = nullptr;
  deletePlaylistButtonText = nullptr;
  seekProgressTrack = nullptr;
  seekProgressFill = nullptr;
  videoProgressTrack = nullptr;
  videoProgressFill = nullptr;
  displayedLibraryArtworkPath.clear();
  displayedFavoritesArtworkPath.clear();
  displayedArtworkPath.clear();
  displayedVideoArtworkPath.clear();
  displayedQueueName.clear();
  seekMouseDown = false;
  activeSeekTouchId = -1;
  videoSeekMouseDown = false;
  activeVideoSeekTouchId = -1;
  videoFullscreenActive = false;
  videoVisualsLoaded = false;
  videoShowingArtwork = false;
  videoRestoresVisualsEnabled = false;
  videoControlsVisible = false;
  videoControlsVisibleUntil = 0;
  videoTrackId.clear();
  videoChart.reset();
}

void MusicPlayerScene::buildView() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  rootLayout = new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout);

  auto *header = new View();
  header->setHeight(safe.top + kHeaderHeight)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14)
      ->setPadding(Edge::Top, safe.top + 10)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding)
      ->setPadding(Edge::Bottom, 10)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setThemedShadow(ui_theme::shadow, ui_theme::kHeaderShadow)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);

  auto *titleColumn = new View();
  titleColumn->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setFlex(1)
      ->setMinWidth(0);

  auto *title = new TextView(kFontPath, 34);
  title->setText("Music Player");
  title->setHeight(42);
  title->setThemedColor(ui_theme::textPrimary);
  titleColumn->addView(title);

  statusText = new TextView(kFontPath, 17);
  statusText->setHeight(26);
  statusText->setThemedColor(ui_theme::textSecondary);
  statusText->setOverflow(TextView::TextOverflow::Hidden);
  titleColumn->addView(statusText);
  header->addView(titleColumn);

  TextView *backText = nullptr;
  auto *backButton = makeButton("Back", 20, &backText);
  backButton->setWidth(118);
  backButton->setHeight(52);
  styleButton(backButton, backText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  backButton->setOnClickListener([this]() { goBack(); });
  header->addView(backButton);
  rootLayout->addView(header);

  auto *content = new View();
  content->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::Top, 16)
      ->setPadding(Edge::Bottom, safe.bottom + 16)
      ->setPadding(Edge::Left, safe.left + kScreenPadding)
      ->setPadding(Edge::Right, safe.right + kScreenPadding);

  auto *rail = new View();
  rail->setWidth(kRailWidth)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(10)
      ->setPadding(Edge::All, 12)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());

  auto *railTitle = new TextView(kFontPath, 18);
  railTitle->setText("Music");
  railTitle->setHeight(26);
  railTitle->setThemedColor(ui_theme::textSecondary);
  railTitle->setOverflow(TextView::TextOverflow::Hidden);
  rail->addView(railTitle);

  libraryNavButton = makeNavButton("Library", &libraryNavText);
  libraryNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Library); });
  favoritesNavButton = makeNavButton("Favorites", &favoritesNavText);
  favoritesNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Favorites); });
  playlistsNavButton = makeNavButton("Playlists", &playlistsNavText);
  playlistsNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Playlists); });
  playerNavButton = makeNavButton("Player", &playerNavText);
  playerNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Player); });
  rail->addView(libraryNavButton);
  rail->addView(favoritesNavButton);
  rail->addView(playlistsNavButton);
  rail->addView(playerNavButton);

  auto *railSpacer = new View();
  railSpacer->setFlex(1);
  rail->addView(railSpacer);
  content->addView(rail);

  auto *pageStack = new View();
  pageStack->setFlex(1)->setMinWidth(0);

  auto makePage = [] {
    auto *page = new View();
    page->setPositionType(YGPositionTypeAbsolute)
        ->setPosition(Edge::Left, 0)
        ->setPosition(Edge::Right, 0)
        ->setPosition(Edge::Top, 0)
        ->setPosition(Edge::Bottom, 0)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch);
    return page;
  };

  libraryPage = makePage();
  buildLibraryPage(libraryPage);
  favoritesPage = makePage();
  buildFavoritesPage(favoritesPage);
  playlistsPage = makePage();
  buildPlaylistsPage(playlistsPage);
  playerPage = makePage();
  buildPlayerPage(playerPage);
  pageStack->addView(libraryPage);
  pageStack->addView(favoritesPage);
  pageStack->addView(playlistsPage);
  pageStack->addView(playerPage);
  content->addView(pageStack);

  rootLayout->addView(content);
  refreshNavigation();
  rootLayout->applyYogaLayout();
  buildVideoOverlay();
}

void MusicPlayerScene::buildVideoOverlay() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  videoOverlayRoot =
      new View(0, 0, rendering::window_width, rendering::window_height);
  videoOverlayRoot->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyFlexEnd)
      ->setPadding(Edge::Left, safe.left + 24)
      ->setPadding(Edge::Right, safe.right + 24)
      ->setPadding(Edge::Bottom, safe.bottom + 24);
  videoOverlayRoot->setVisible(false);

  videoArtworkBackdrop = new View();
  videoArtworkBackdrop->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Right, 0)
      ->setPosition(Edge::Top, 0)
      ->setPosition(Edge::Bottom, 0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setBackgroundColor(Color(0, 0, 0, 255));
  videoArtworkBackdrop->setVisible(false);

  videoArtworkFallbackText = new TextView(kFontPath, 24);
  videoArtworkFallbackText->setText("No jacket available");
  videoArtworkFallbackText->setHeight(40);
  videoArtworkFallbackText->setAlign(TextView::CENTER);
  videoArtworkFallbackText->setVAlign(TextView::MIDDLE);
  videoArtworkFallbackText->setThemedColor(ui_theme::textMuted);
  videoArtworkBackdrop->addView(videoArtworkFallbackText);

  videoArtworkImage = new ImageView(0, 0, 0, 0);
  videoArtworkImage->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setThemedBorderColor(ui_theme::hairlineStrong)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());
  videoArtworkBackdrop->addView(videoArtworkImage);

  videoControlsPanel = new View();
  videoControlsPanel->setHeight(190)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setPadding(Edge::All, 14)
      ->setThemedBackgroundColor(
          [] { return ui_theme::withAlpha(ui_theme::panelStrong(), 232); })
      ->setThemedBorderColor(ui_theme::hairlineStrong)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());

  auto *titleRow = new View();
  titleRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *titleColumn = new View();
  titleColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(2);

  videoTitleText = new TextView(kFontPath, 20);
  videoTitleText->setHeight(27);
  videoTitleText->setThemedColor(ui_theme::textPrimary);
  videoTitleText->setOverflow(TextView::TextOverflow::Marquee);
  titleColumn->addView(videoTitleText);

  videoDetailText = new TextView(kFontPath, 15);
  videoDetailText->setHeight(22);
  videoDetailText->setThemedColor(ui_theme::textSecondary);
  videoDetailText->setOverflow(TextView::TextOverflow::Marquee);
  titleColumn->addView(videoDetailText);

  videoPlaybackText = new TextView(kFontPath, 17);
  videoPlaybackText->setWidth(132);
  videoPlaybackText->setHeight(52);
  videoPlaybackText->setAlign(TextView::RIGHT);
  videoPlaybackText->setVAlign(TextView::MIDDLE);
  videoPlaybackText->setThemedColor(ui_theme::textSecondary);
  titleRow->addView(titleColumn);
  titleRow->addView(videoPlaybackText);
  videoControlsPanel->addView(titleRow);

  videoProgressTrack = new View();
  videoProgressTrack->setHeight(24)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(ui_theme::insetSurface)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  videoProgressFill = new MusicSeekProgressFillView();
  videoProgressFill->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 1)
      ->setPosition(Edge::Right, 1)
      ->setPosition(Edge::Top, 1)
      ->setPosition(Edge::Bottom, 1);
  videoProgressTrack->addView(videoProgressFill);
  videoControlsPanel->addView(videoProgressTrack);

  auto *transportRow = new View();
  transportRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);

  TextView *back10Text = nullptr;
  auto *back10Button = makeButton("-10s", 17, &back10Text);
  back10Button->setFlex(1);
  styleButton(back10Button, back10Text, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  back10Button->setOnClickListener([this]() {
    showVideoControls();
    seekRelative(-10000000LL);
  });

  auto *playPauseButton = makeButton("Pause", 18, &videoPlayPauseButtonText);
  playPauseButton->setFlex(1);
  styleButton(playPauseButton, videoPlayPauseButtonText, ui_theme::infoAction,
              ui_theme::infoActionHover, ui_theme::infoActionPressed,
              ui_theme::accentBorder);
  playPauseButton->setOnClickListener([this]() {
    showVideoControls();
    togglePlayback();
  });

  TextView *forward10Text = nullptr;
  auto *forward10Button = makeButton("+10s", 17, &forward10Text);
  forward10Button->setFlex(1);
  styleButton(forward10Button, forward10Text, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  forward10Button->setOnClickListener([this]() {
    showVideoControls();
    seekRelative(10000000LL);
  });

  TextView *closeText = nullptr;
  auto *closeButton = makeButton("Close", 17, &closeText);
  closeButton->setFlex(1);
  styleButton(closeButton, closeText, ui_theme::warningAction,
              ui_theme::warningActionHover, ui_theme::warningActionPressed,
              ui_theme::accentBorder);
  closeButton->setOnClickListener([this]() { exitVideoFullscreen(); });

  transportRow->addView(back10Button);
  transportRow->addView(playPauseButton);
  transportRow->addView(forward10Button);
  transportRow->addView(closeButton);
  videoControlsPanel->addView(transportRow);

  videoOverlayRoot->addView(videoArtworkBackdrop);
  videoOverlayRoot->addView(videoControlsPanel);
  addView(videoOverlayRoot);
  videoOverlayRoot->applyYogaLayout();
  layoutVideoArtwork();
}

void MusicPlayerScene::buildLibraryPage(View *page) {
  buildTrackBrowserPage(page, TrackBrowserKind::Library);
}

void MusicPlayerScene::buildFavoritesPage(View *page) {
  buildTrackBrowserPage(page, TrackBrowserKind::Favorites);
}

void MusicPlayerScene::buildTrackBrowserPage(View *page,
                                             TrackBrowserKind kind) {
  const bool isLibrary = kind == TrackBrowserKind::Library;
  TextView **subtitleText =
      isLibrary ? &librarySubtitleText : &favoritesSubtitleText;
  auto *panel = makePanel(isLibrary ? "Library" : "Favorites", subtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *searchRow = new View();
  searchRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10);
  auto *searchLabel = new TextView(kFontPath, 17);
  searchLabel->setText("Search");
  searchLabel->setWidth(72);
  searchLabel->setHeight(52);
  searchLabel->setVAlign(TextView::MIDDLE);
  searchLabel->setThemedColor(ui_theme::textSecondary);
  auto *searchInput = new TextInputBox(kFontPath, 18);
  if (isLibrary) {
    librarySearchInput = searchInput;
  } else {
    favoritesSearchInput = searchInput;
  }
  searchInput->setFlex(1);
  searchInput->setHeight(52);
  searchInput->setThemedBackgroundColor(ui_theme::control);
  searchInput->setThemedBorderColor(ui_theme::hairlineStrong);
  searchInput->setBorderWidth(1);
  searchInput->setCornerRadius(ui_theme::controlRadius());
  searchInput->setThemedColor(ui_theme::textPrimary);
  searchInput->setVAlign(TextView::MIDDLE);
  searchInput->onTextChanged([this, kind](const std::string &value) {
    trackBrowserSearchText(kind) = value;
    applyTrackBrowserFilter(kind);
    refreshUi();
  });
  TextView *clearSearchText = nullptr;
  auto *clearSearchButton = makeButton("Clear", 17, &clearSearchText);
  clearSearchButton->setWidth(104);
  styleButton(clearSearchButton, clearSearchText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  clearSearchButton->setOnClickListener([this, kind, searchInput]() {
    trackBrowserSearchText(kind).clear();
    if (searchInput != nullptr) {
      searchInput->setEditingText("");
    }
    applyTrackBrowserFilter(kind);
    refreshUi();
  });
  searchRow->addView(searchLabel);
  searchRow->addView(searchInput);
  searchRow->addView(clearSearchButton);
  panel->addView(searchRow);

  auto *workspace = new View();
  workspace->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  panel->addView(workspace);

  auto *trackList = new RecyclerView<MusicTrack>(
      [](const MusicTrack &a, const MusicTrack &b) {
        return a.trackId == b.trackId && a.chartId == b.chartId;
      });
  if (isLibrary) {
    libraryList = trackList;
  } else {
    favoritesList = trackList;
  }
  trackList->setFlex(1);
  trackList->itemHeight = kTrackRowHeight;
  trackList->reserveScrollbarGutter = true;
  trackList->onCreateView = [](const MusicTrack &) {
    return new MusicTrackRowView();
  };
  trackList->onBind = [this](View *view, const MusicTrack &track, int,
                             bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected, isFavoriteTrack(track),
                    [this, track]() { toggleFavorite(track); });
    }
  };
  trackList->onSelected = [this, kind](const MusicTrack &, int index) {
    selectTrackBrowserTrack(kind, index);
  };
  trackList->onUnselected = [trackList](const MusicTrack &, int index) {
    if (auto *unselectedView = trackList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  workspace->addView(trackList);

  auto *actions = new View();
  actions->setWidth(390)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *libraryArtFrame = new View();
  libraryArtFrame->setHeight(250)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(ui_theme::insetSurface)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  auto *artworkFallbackText = new TextView(kFontPath, 18);
  if (isLibrary) {
    libraryArtworkFallbackText = artworkFallbackText;
  } else {
    favoritesArtworkFallbackText = artworkFallbackText;
  }
  artworkFallbackText->setText("No album art");
  artworkFallbackText->setHeight(36);
  artworkFallbackText->setAlign(TextView::CENTER);
  artworkFallbackText->setVAlign(TextView::MIDDLE);
  artworkFallbackText->setThemedColor(ui_theme::textMuted);
  libraryArtFrame->addView(artworkFallbackText);
  auto *artworkImage = new ImageView(0, 0, 0, 0);
  if (isLibrary) {
    libraryArtworkImage = artworkImage;
  } else {
    favoritesArtworkImage = artworkImage;
  }
  artworkImage->setWidth(388)
      ->setHeight(248)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setCornerRadius(ui_theme::controlRadius());
  libraryArtFrame->addView(artworkImage);
  actions->addView(libraryArtFrame);

  auto *selectionTitle = new TextView(kFontPath, 18);
  selectionTitle->setText("Selected Track");
  selectionTitle->setHeight(28);
  selectionTitle->setThemedColor(ui_theme::textSecondary);
  actions->addView(selectionTitle);

  auto *selectionTitleText = new TextView(kFontPath, 24);
  if (isLibrary) {
    librarySelectionTitleText = selectionTitleText;
  } else {
    favoritesSelectionTitleText = selectionTitleText;
  }
  selectionTitleText->setHeight(42);
  selectionTitleText->setOverflow(TextView::TextOverflow::Marquee);
  selectionTitleText->setThemedColor(ui_theme::textPrimary);
  actions->addView(selectionTitleText);

  auto *selectionDetailText = new TextView(kFontPath, 16);
  if (isLibrary) {
    librarySelectionDetailText = selectionDetailText;
  } else {
    favoritesSelectionDetailText = selectionDetailText;
  }
  selectionDetailText->setHeight(32);
  selectionDetailText->setOverflow(TextView::TextOverflow::Marquee);
  selectionDetailText->setThemedColor(ui_theme::textSecondary);
  actions->addView(selectionDetailText);

  auto *primaryRow = new View();
  primaryRow->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  TextView *playTrackText = nullptr;
  auto *playTrackButton = makeButton("Play Track", 17, &playTrackText);
  playTrackButton->setFlex(1);
  styleButton(playTrackButton, playTrackText, ui_theme::primaryAction,
              ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
              ui_theme::accentBorderStrong);
  playTrackButton->setOnClickListener(
      [this, kind]() { playTrackBrowserTrack(kind); });
  TextView *addText = nullptr;
  auto *addButton = makeButton("Add to playlist", 15, &addText);
  addButton->setFlex(1);
  styleButton(addButton, addText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  addButton->setOnClickListener(
      [this, kind]() { addTrackBrowserTrackToPlaylist(kind); });
  primaryRow->addView(playTrackButton);
  primaryRow->addView(addButton);
  actions->addView(primaryRow);

  auto *secondaryRow = new View();
  secondaryRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  TextView *randomText = nullptr;
  auto *randomButton = makeButton("Random All", 17, &randomText);
  randomButton->setFlex(1);
  styleButton(randomButton, randomText, ui_theme::successAction,
              ui_theme::successActionHover, ui_theme::successActionPressed,
              ui_theme::accentBorder);
  randomButton->setOnClickListener(
      [this, kind]() { playRandomTrackBrowser(kind); });
  TextView *reloadText = nullptr;
  auto *reloadButton = makeButton("Refresh", 17, &reloadText);
  reloadButton->setFlex(1);
  styleButton(reloadButton, reloadText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  reloadButton->setOnClickListener([this]() { reloadData(true); });
  secondaryRow->addView(randomButton);
  secondaryRow->addView(reloadButton);
  actions->addView(secondaryRow);

  if (isLibrary) {
    TextView *groupText = nullptr;
    auto *groupButton = makeButton("Expand Group", 17, &groupText);
    libraryGroupButtonText = groupText;
    groupButton->setHeight(48);
    styleButton(groupButton, groupText, ui_theme::control,
                ui_theme::controlHover, ui_theme::controlPressed,
                ui_theme::hairlineStrong);
    groupButton->setOnClickListener(
        [this]() { toggleSelectedLibraryGroup(); });
    actions->addView(groupButton);
  }

  auto *addToHeader = new TextView(kFontPath, 18);
  addToHeader->setText("Add To");
  addToHeader->setHeight(28);
  addToHeader->setThemedColor(ui_theme::textSecondary);
  actions->addView(addToHeader);

  auto *addToPlaylistList = new RecyclerView<PlaylistInfo>(
      [](const PlaylistInfo &a, const PlaylistInfo &b) { return a.id == b.id; });
  if (isLibrary) {
    libraryPlaylistList = addToPlaylistList;
  } else {
    favoritesPlaylistList = addToPlaylistList;
  }
  addToPlaylistList->setFlex(1);
  addToPlaylistList->itemHeight = kPlaylistRowHeight;
  addToPlaylistList->reserveScrollbarGutter = true;
  addToPlaylistList->onCreateView = [](const PlaylistInfo &) {
    return new PlaylistRowView();
  };
  addToPlaylistList->onBind = [](View *view, const PlaylistInfo &playlist,
                                 int, bool selected) {
    if (auto *row = dynamic_cast<PlaylistRowView *>(view)) {
      row->setPlaylist(playlist, selected);
    }
  };
  addToPlaylistList->onSelected = [this](const PlaylistInfo &, int index) {
    selectLibraryPlaylist(index);
  };
  addToPlaylistList->onUnselected = [addToPlaylistList](const PlaylistInfo &,
                                                        int index) {
    if (auto *unselectedView = addToPlaylistList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  actions->addView(addToPlaylistList);
  workspace->addView(actions);
}

void MusicPlayerScene::buildPlaylistsPage(View *page) {
  auto *panel = makePanel("Playlists", &playlistSubtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *workspace = new View();
  workspace->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  panel->addView(workspace);

  auto *directoryColumn = new View();
  directoryColumn->setWidth(360)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *createPlaylistRow = new View();
  createPlaylistRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  playlistNameInput = new TextInputBox(kFontPath, 18);
  playlistNameInput->setFlex(1);
  playlistNameInput->setHeight(52);
  playlistNameInput->setEditingText("");
  playlistNameInput->setThemedBackgroundColor(ui_theme::control);
  playlistNameInput->setThemedBorderColor(ui_theme::hairlineStrong);
  playlistNameInput->setBorderWidth(1);
  playlistNameInput->setCornerRadius(ui_theme::controlRadius());
  playlistNameInput->setThemedColor(ui_theme::textPrimary);
  playlistNameInput->setVAlign(TextView::MIDDLE);
  playlistNameInput->onSubmit(
      [this](const std::string &) { createPlaylist(); });
  TextView *createText = nullptr;
  auto *createButton = makeButton("Create", 17, &createText);
  createButton->setWidth(112);
  styleButton(createButton, createText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  createButton->setOnClickListener([this]() { createPlaylist(); });
  createPlaylistRow->addView(playlistNameInput);
  createPlaylistRow->addView(createButton);
  directoryColumn->addView(createPlaylistRow);

  TextView *saveNowPlayingText = nullptr;
  auto *saveNowPlayingButton =
      makeButton("Save Now Playing", 16, &saveNowPlayingText);
  saveNowPlayingButton->setHeight(52);
  styleButton(saveNowPlayingButton, saveNowPlayingText,
              ui_theme::successAction, ui_theme::successActionHover,
              ui_theme::successActionPressed, ui_theme::accentBorder);
  saveNowPlayingButton->setOnClickListener(
      [this]() { saveNowPlayingAsPlaylist(); });
  directoryColumn->addView(saveNowPlayingButton);

  auto *renamePlaylistRow = new View();
  renamePlaylistRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  playlistRenameInput = new TextInputBox(kFontPath, 18);
  playlistRenameInput->setFlex(1);
  playlistRenameInput->setHeight(52);
  playlistRenameInput->setThemedBackgroundColor(ui_theme::control);
  playlistRenameInput->setThemedBorderColor(ui_theme::hairlineStrong);
  playlistRenameInput->setBorderWidth(1);
  playlistRenameInput->setCornerRadius(ui_theme::controlRadius());
  playlistRenameInput->setThemedColor(ui_theme::textPrimary);
  playlistRenameInput->setVAlign(TextView::MIDDLE);
  playlistRenameInput->onSubmit(
      [this](const std::string &) { renameSelectedPlaylist(); });
  TextView *renameText = nullptr;
  auto *renameButton = makeButton("Rename", 16, &renameText);
  renameButton->setWidth(112);
  styleButton(renameButton, renameText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  renameButton->setOnClickListener([this]() { renameSelectedPlaylist(); });
  renamePlaylistRow->addView(playlistRenameInput);
  renamePlaylistRow->addView(renameButton);
  directoryColumn->addView(renamePlaylistRow);

  auto *playlistManageRow = new View();
  playlistManageRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  auto *deletePlaylistButton =
      makeButton("Delete Playlist", 15, &deletePlaylistButtonText);
  deletePlaylistButton->setFlex(1);
  styleButton(deletePlaylistButton, deletePlaylistButtonText,
              ui_theme::warningAction, ui_theme::warningActionHover,
              ui_theme::warningActionPressed, ui_theme::accentBorder);
  deletePlaylistButton->setOnClickListener(
      [this]() { deleteSelectedPlaylist(); });
  playlistManageRow->addView(deletePlaylistButton);
  directoryColumn->addView(playlistManageRow);

  playlistDirectoryList = new RecyclerView<PlaylistInfo>(
      [](const PlaylistInfo &a, const PlaylistInfo &b) { return a.id == b.id; });
  playlistDirectoryList->setFlex(1);
  playlistDirectoryList->itemHeight = kPlaylistRowHeight;
  playlistDirectoryList->reserveScrollbarGutter = true;
  playlistDirectoryList->onCreateView = [](const PlaylistInfo &) {
    return new PlaylistRowView();
  };
  playlistDirectoryList->onBind = [](View *view, const PlaylistInfo &playlist,
                                     int, bool selected) {
    if (auto *row = dynamic_cast<PlaylistRowView *>(view)) {
      row->setPlaylist(playlist, selected);
    }
  };
  playlistDirectoryList->onSelected = [this](const PlaylistInfo &, int index) {
    if (auto *selectedView = playlistDirectoryList->getViewByIndex(index)) {
      selectedView->onSelected();
    }
    selectPlaylist(index);
  };
  playlistDirectoryList->onUnselected = [this](const PlaylistInfo &, int index) {
    if (auto *unselectedView = playlistDirectoryList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  directoryColumn->addView(playlistDirectoryList);
  workspace->addView(directoryColumn);

  auto *editorColumn = new View();
  editorColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *selectedHeader = new TextView(kFontPath, 18);
  selectedHeader->setText("Playlist Contents");
  selectedHeader->setHeight(28);
  selectedHeader->setThemedColor(ui_theme::textSecondary);
  editorColumn->addView(selectedHeader);

  playlistSelectionTitleText = new TextView(kFontPath, 24);
  playlistSelectionTitleText->setHeight(36);
  playlistSelectionTitleText->setOverflow(TextView::TextOverflow::Marquee);
  playlistSelectionTitleText->setThemedColor(ui_theme::textPrimary);
  editorColumn->addView(playlistSelectionTitleText);

  playlistSelectionDetailText = new TextView(kFontPath, 16);
  playlistSelectionDetailText->setHeight(32);
  playlistSelectionDetailText->setOverflow(TextView::TextOverflow::Marquee);
  playlistSelectionDetailText->setThemedColor(ui_theme::textSecondary);
  editorColumn->addView(playlistSelectionDetailText);

  playlistList = new RecyclerView<MusicTrack>(
      [](const MusicTrack &a, const MusicTrack &b) {
        return a.trackId == b.trackId && a.chartId == b.chartId;
      });
  playlistList->setFlex(1);
  playlistList->itemHeight = kTrackRowHeight;
  playlistList->reserveScrollbarGutter = true;
  playlistList->onCreateView = [](const MusicTrack &) {
    return new MusicTrackRowView();
  };
  playlistList->onBind = [this](View *view, const MusicTrack &track, int,
                                bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected, isFavoriteTrack(track),
                    [this, track]() { toggleFavorite(track); });
    }
  };
  playlistList->onSelected = [this](const MusicTrack &, int index) {
    selectPlaylistTrack(index);
  };
  playlistList->onUnselected = [this](const MusicTrack &, int index) {
    if (auto *unselectedView = playlistList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  editorColumn->addView(playlistList);

  auto *playlistButtons = new View();
  playlistButtons->setHeight(118)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(10);
  auto *playlistRowA = new View();
  playlistRowA->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  auto *playlistRowB = new View();
  playlistRowB->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  TextView *playPlaylistText = nullptr;
  auto *playPlaylistButton = makeButton("Play Playlist", 17, &playPlaylistText);
  playPlaylistButton->setFlex(1);
  styleButton(playPlaylistButton, playPlaylistText, ui_theme::primaryAction,
              ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
              ui_theme::accentBorderStrong);
  playPlaylistButton->setOnClickListener([this]() { playPlaylist(); });
  TextView *removeText = nullptr;
  auto *removeButton = makeButton("Remove Track", 16, &removeText);
  removeButton->setFlex(1);
  styleButton(removeButton, removeText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  removeButton->setOnClickListener([this]() { removePlaylistTrack(); });
  TextView *upText = nullptr;
  auto *upButton = makeButton("Up", 17, &upText);
  upButton->setFlex(1);
  styleButton(upButton, upText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  upButton->setOnClickListener([this]() { movePlaylistTrack(-1); });
  TextView *downText = nullptr;
  auto *downButton = makeButton("Down", 17, &downText);
  downButton->setFlex(1);
  styleButton(downButton, downText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  downButton->setOnClickListener([this]() { movePlaylistTrack(1); });
  TextView *clearText = nullptr;
  auto *clearButton = makeButton("Clear", 17, &clearText);
  clearButton->setFlex(1);
  styleButton(clearButton, clearText, ui_theme::warningAction,
              ui_theme::warningActionHover, ui_theme::warningActionPressed,
              ui_theme::accentBorder);
  clearButton->setOnClickListener([this]() { clearPlaylist(); });
  playlistRowA->addView(playPlaylistButton);
  playlistRowA->addView(removeButton);
  playlistRowB->addView(upButton);
  playlistRowB->addView(downButton);
  playlistRowB->addView(clearButton);
  playlistButtons->addView(playlistRowA);
  playlistButtons->addView(playlistRowB);
  editorColumn->addView(playlistButtons);
  workspace->addView(editorColumn);
}

void MusicPlayerScene::buildPlayerPage(View *page) {
  auto *panel = makePanel("Player", &playerSubtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *workspace = new View();
  workspace->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  panel->addView(workspace);

  auto *nowColumn = new View();
  nowColumn->setWidth(420)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *artFrame = new View();
  artFrame->setHeight(330)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(ui_theme::insetSurface)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  artworkFallbackText = new TextView(kFontPath, 18);
  artworkFallbackText->setText("No album art");
  artworkFallbackText->setHeight(36);
  artworkFallbackText->setAlign(TextView::CENTER);
  artworkFallbackText->setVAlign(TextView::MIDDLE);
  artworkFallbackText->setThemedColor(ui_theme::textMuted);
  artFrame->addView(artworkFallbackText);
  artworkImage = new ImageView(0, 0, 0, 0);
  artworkImage->setWidth(418)
      ->setHeight(328)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setCornerRadius(ui_theme::controlRadius());
  artFrame->addView(artworkImage);
  nowColumn->addView(artFrame);

  currentTitleText = new TextView(kFontPath, 27);
  currentTitleText->setHeight(40);
  currentTitleText->setThemedColor(ui_theme::textPrimary);
  currentTitleText->setOverflow(TextView::TextOverflow::Marquee);
  nowColumn->addView(currentTitleText);

  currentDetailText = new TextView(kFontPath, 17);
  currentDetailText->setHeight(32);
  currentDetailText->setOverflow(TextView::TextOverflow::Marquee);
  currentDetailText->setThemedColor(ui_theme::textSecondary);
  nowColumn->addView(currentDetailText);

  playbackText = new TextView(kFontPath, 18);
  playbackText->setHeight(32);
  playbackText->setThemedColor(ui_theme::textSecondary);
  nowColumn->addView(playbackText);

  seekProgressTrack = new View();
  seekProgressTrack->setHeight(24)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(ui_theme::insetSurface)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  seekProgressFill = new MusicSeekProgressFillView();
  seekProgressFill->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 1)
      ->setPosition(Edge::Right, 1)
      ->setPosition(Edge::Top, 1)
      ->setPosition(Edge::Bottom, 1);
  seekProgressTrack->addView(seekProgressFill);
  nowColumn->addView(seekProgressTrack);

  auto *transport = new View();
  transport->setHeight(178)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(10);
  auto *transportRowA = new View();
  transportRowA->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  auto *transportRowB = new View();
  transportRowB->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  auto *transportRowC = new View();
  transportRowC->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);

  TextView *previousText = nullptr;
  auto *previousButton = makeButton("Previous", 15, &previousText);
  previousButton->setFlex(1);
  styleButton(previousButton, previousText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  previousButton->setOnClickListener([this]() { playPrevious(); });
  auto *playPauseButton = makeButton("Play", 18, &playPauseButtonText);
  playPauseButton->setFlex(1);
  styleButton(playPauseButton, playPauseButtonText, ui_theme::infoAction,
              ui_theme::infoActionHover, ui_theme::infoActionPressed,
              ui_theme::accentBorder);
  playPauseButton->setOnClickListener([this]() { togglePlayback(); });
  TextView *nextText = nullptr;
  auto *nextButton = makeButton("Next", 18, &nextText);
  nextButton->setFlex(1);
  styleButton(nextButton, nextText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  nextButton->setOnClickListener([this]() { playNext(); });

  TextView *back10Text = nullptr;
  auto *back10Button = makeButton("-10s", 17, &back10Text);
  back10Button->setFlex(1);
  styleButton(back10Button, back10Text, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  back10Button->setOnClickListener([this]() { seekRelative(-10000000LL); });
  TextView *forward10Text = nullptr;
  auto *forward10Button = makeButton("+10s", 17, &forward10Text);
  forward10Button->setFlex(1);
  styleButton(forward10Button, forward10Text, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  forward10Button->setOnClickListener([this]() { seekRelative(10000000LL); });
  TextView *stopText = nullptr;
  auto *stopButton = makeButton("Stop", 18, &stopText);
  stopButton->setFlex(1);
  styleButton(stopButton, stopText, ui_theme::warningAction,
              ui_theme::warningActionHover, ui_theme::warningActionPressed,
              ui_theme::accentBorder);
  stopButton->setOnClickListener([this]() { stopPlayback(); });

  TextView *playSelectedText = nullptr;
  auto *playSelectedButton =
      makeButton("Play Queue Track", 16, &playSelectedText);
  playSelectedButton->setFlex(1);
  styleButton(playSelectedButton, playSelectedText, ui_theme::primaryAction,
              ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
              ui_theme::accentBorderStrong);
  playSelectedButton->setOnClickListener([this]() { playSelectedQueueTrack(); });

  auto *repeatButton = makeButton("Playlist Loop", 16, &repeatModeButtonText);
  repeatButton->setFlex(1);
  styleButton(repeatButton, repeatModeButtonText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  repeatButton->setOnClickListener([this]() { cycleRepeatMode(); });

  auto *watchVideoButton = makeButton("Watch Video", 17, &watchVideoButtonText);
  watchVideoButton->setFlex(1);
  styleButton(watchVideoButton, watchVideoButtonText, ui_theme::successAction,
              ui_theme::successActionHover, ui_theme::successActionPressed,
              ui_theme::accentBorder);
  watchVideoButton->setOnClickListener([this]() { watchVideo(); });

  transportRowA->addView(previousButton);
  transportRowA->addView(playPauseButton);
  transportRowA->addView(nextButton);
  transportRowB->addView(back10Button);
  transportRowB->addView(forward10Button);
  transportRowB->addView(stopButton);
  transportRowC->addView(playSelectedButton);
  transportRowC->addView(repeatButton);
  transportRowC->addView(watchVideoButton);
  transport->addView(transportRowA);
  transport->addView(transportRowB);
  transport->addView(transportRowC);
  nowColumn->addView(transport);
  workspace->addView(nowColumn);

  auto *queueColumn = new View();
  queueColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  queueTitleText = new TextView(kFontPath, 18);
  queueTitleText->setText("Playback Queue");
  queueTitleText->setHeight(28);
  queueTitleText->setThemedColor(ui_theme::textSecondary);
  queueColumn->addView(queueTitleText);

  playerQueueList = new RecyclerView<MusicTrack>(
      [](const MusicTrack &a, const MusicTrack &b) {
        return a.trackId == b.trackId && a.chartId == b.chartId;
      });
  playerQueueList->setFlex(1);
  playerQueueList->itemHeight = kTrackRowHeight;
  playerQueueList->reserveScrollbarGutter = true;
  playerQueueList->onCreateView = [](const MusicTrack &) {
    return new MusicTrackRowView();
  };
  playerQueueList->onBind = [this](View *view, const MusicTrack &track, int,
                                   bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected, isFavoriteTrack(track),
                    [this, track]() { toggleFavorite(track); });
    }
  };
  playerQueueList->onSelected = [this](const MusicTrack &, int index) {
    selectQueueTrack(index);
  };
  playerQueueList->onUnselected = [this](const MusicTrack &, int index) {
    if (auto *unselectedView = playerQueueList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  queueColumn->addView(playerQueueList);

  auto *queueButtons = new View();
  queueButtons->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  TextView *playPlaylistText = nullptr;
  auto *playPlaylistButton = makeButton("Play Playlist", 17, &playPlaylistText);
  playPlaylistButton->setFlex(1);
  styleButton(playPlaylistButton, playPlaylistText, ui_theme::primaryAction,
              ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
              ui_theme::accentBorderStrong);
  playPlaylistButton->setOnClickListener([this]() { playPlaylist(); });
  TextView *editText = nullptr;
  auto *editButton = makeButton("Edit Playlist", 17, &editText);
  editButton->setFlex(1);
  styleButton(editButton, editText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  editButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Playlists); });
  TextView *saveQueueText = nullptr;
  auto *saveQueueButton = makeButton("Save Queue", 17, &saveQueueText);
  saveQueueButton->setFlex(1);
  styleButton(saveQueueButton, saveQueueText, ui_theme::successAction,
              ui_theme::successActionHover, ui_theme::successActionPressed,
              ui_theme::accentBorder);
  saveQueueButton->setOnClickListener(
      [this]() { saveNowPlayingAsPlaylist(); });
  queueButtons->addView(playPlaylistButton);
  queueButtons->addView(editButton);
  queueButtons->addView(saveQueueButton);
  queueColumn->addView(queueButtons);
  workspace->addView(queueColumn);
}

View *MusicPlayerScene::makePanel(const std::string &title,
                                  TextView **subtitleText) {
  auto *panel = new View();
  panel->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, 16)
      ->setThemedBackgroundColor(ui_theme::mainMenuPanel)
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::panelRadius());

  auto *titleText = new TextView(kFontPath, 26);
  titleText->setText(title);
  titleText->setHeight(34);
  titleText->setThemedColor(ui_theme::textPrimary);
  titleText->setOverflow(TextView::TextOverflow::Hidden);
  panel->addView(titleText);

  if (subtitleText != nullptr) {
    *subtitleText = new TextView(kFontPath, 16);
    (*subtitleText)->setHeight(24);
    (*subtitleText)->setThemedColor(ui_theme::textSecondary);
    (*subtitleText)->setOverflow(TextView::TextOverflow::Hidden);
    panel->addView(*subtitleText);
  }
  return panel;
}

Button *MusicPlayerScene::makeButton(const std::string &label, int fontSize,
                                     TextView **textOut) {
  auto *button = new Button();
  button->setHeight(52);
  button->setCornerRadius(ui_theme::controlRadius());

  auto *text = new TextView(kFontPath, fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Button *MusicPlayerScene::makeNavButton(const std::string &label,
                                        TextView **textOut) {
  auto *button = makeButton(label, 18, textOut);
  button->setHeight(58);
  return button;
}

void MusicPlayerScene::styleButton(Button *button, TextView *text,
                                   View::ThemeColorProvider normal,
                                   View::ThemeColorProvider hover,
                                   View::ThemeColorProvider pressed,
                                   View::ThemeColorProvider border) {
  if (button == nullptr) {
    return;
  }
  button->setThemedBackgroundColors(normal, hover, pressed);
  button->setThemedBorderColors(border, border, border);
  button->setStyledBorderWidth(1);
  if (text != nullptr) {
    text->setThemedColor([normal] { return ui_theme::textOn(normal()); });
  }
}

void MusicPlayerScene::reloadData(bool preserveSelection) {
  const auto selectedLibrary = selectedLibraryTrack();
  const auto selectedFavorite =
      selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
  const auto selectedPlaylist = selectedPlaylistTrack();
  const std::string selectedLibraryId =
      selectedLibrary ? selectedLibrary->trackId : "";
  const std::string selectedFavoriteId =
      selectedFavorite ? selectedFavorite->trackId : "";
  const std::string selectedPlaylistTrackId =
      selectedPlaylist ? selectedPlaylist->trackId : "";
  const int preferredPlaylistId =
      preserveSelection ? selectedPlaylistId : 0;

  std::string errorMessage;
  if (!context.musicPlayer.ReloadLibraryAndPlaylists(errorMessage,
                                                     preferredPlaylistId)) {
    setStatus(errorMessage);
  }
  libraryTracks = context.musicPlayer.LibraryTracksSnapshot();
  favoriteTracks = context.musicPlayer.FavoriteTracksSnapshot();
  rebuildFavoriteTrackIds();
  libraryGroupTracks.clear();
  expandedLibraryGroupIds.clear();
  playlists = context.musicPlayer.PlaylistsSnapshot();
  selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
  playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();

  auto findIndex = [](const std::vector<MusicTrack> &tracks,
                      const std::string &trackId) {
    if (trackId.empty()) {
      return -1;
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].trackId == trackId) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const int libraryIndex =
      preserveSelection ? findIndex(libraryTracks, selectedLibraryId) : -1;
  const int favoriteIndex =
      preserveSelection ? findIndex(favoriteTracks, selectedFavoriteId) : -1;
  const int playlistIndex =
      preserveSelection ? findIndex(playlistTracks, selectedPlaylistTrackId)
                        : -1;
  refreshActiveQueueList(true);
  refreshLibraryList(libraryIndex);
  refreshTrackBrowserList(TrackBrowserKind::Favorites, favoriteIndex);
  refreshLibraryPlaylistList(preserveSelection ? selectedLibraryPlaylistId : 0);
  refreshPlaylistDirectoryList(preferredPlaylistId);
  refreshPlaylistList(playlistIndex);
  refreshUi();
}

void MusicPlayerScene::refreshLibraryList(int preferredIndex) {
  refreshTrackBrowserList(TrackBrowserKind::Library, preferredIndex);
}

void MusicPlayerScene::applyLibraryFilter(int preferredIndex) {
  applyTrackBrowserFilter(TrackBrowserKind::Library, preferredIndex);
}

void MusicPlayerScene::applyLibraryFilterForTrackId(
    const std::string &preferredTrackId, bool preserveScroll,
    bool revealPreferredIfOutOfView) {
  applyTrackBrowserFilterForTrackId(TrackBrowserKind::Library,
                                    preferredTrackId, preserveScroll,
                                    revealPreferredIfOutOfView);
}

void MusicPlayerScene::refreshTrackBrowserList(TrackBrowserKind kind,
                                               int preferredIndex) {
  applyTrackBrowserFilter(kind, preferredIndex);
}

void MusicPlayerScene::applyTrackBrowserFilter(TrackBrowserKind kind,
                                               int preferredIndex) {
  if (trackBrowserList(kind) == nullptr) {
    return;
  }

  std::string preferredTrackId;
  const auto &filtered = trackBrowserFilteredTracks(kind);
  const auto &source = trackBrowserSourceTracks(kind);
  if (preferredIndex >= 0 &&
      preferredIndex < static_cast<int>(filtered.size())) {
    preferredTrackId =
        filtered[static_cast<std::size_t>(preferredIndex)].trackId;
  } else if (preferredIndex >= 0 &&
             preferredIndex < static_cast<int>(source.size())) {
    preferredTrackId =
        source[static_cast<std::size_t>(preferredIndex)].trackId;
  }

  applyTrackBrowserFilterForTrackId(kind, preferredTrackId);
}

void MusicPlayerScene::applyTrackBrowserFilterForTrackId(
    TrackBrowserKind kind, const std::string &preferredTrackId,
    bool preserveScroll,
    bool revealPreferredIfOutOfView) {
  auto *list = trackBrowserList(kind);
  if (list == nullptr) {
    return;
  }

  const float previousScrollOffset =
      preserveScroll ? list->scrollOffset : 0.0f;

  auto &filtered = trackBrowserFilteredTracks(kind);
  const auto &source = trackBrowserSourceTracks(kind);
  filtered.clear();
  const std::string query =
      lowercaseText(trimPlaylistName(trackBrowserSearchText(kind)));
  for (const auto &track : source) {
    if (kind == TrackBrowserKind::Library) {
      const bool expanded = !track.groupId.empty() &&
                            expandedLibraryGroupIds.contains(track.groupId);
      const auto childrenIt = expanded ? libraryGroupTracks.find(track.groupId)
                                       : libraryGroupTracks.end();
      if (expanded && childrenIt != libraryGroupTracks.end()) {
        std::copy_if(childrenIt->second.begin(), childrenIt->second.end(),
                     std::back_inserter(filtered),
                     [&query](const MusicTrack &childTrack) {
                       return trackMatchesSearch(childTrack, query);
                     });
        continue;
      }
    }
    if (trackMatchesSearch(track, query)) {
      filtered.push_back(track);
    }
  }

  int &selectedIndex = trackBrowserSelectedIndex(kind);
  selectedIndex = -1;
  if (!preferredTrackId.empty()) {
    for (std::size_t i = 0; i < filtered.size(); ++i) {
      if (filtered[i].trackId == preferredTrackId) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (selectedIndex < 0 && !filtered.empty()) {
    selectedIndex = 0;
  }

  list->setItems(filtered);
  list->selectedIndex = selectedIndex;
  if (preserveScroll) {
    const float maxScrollOffset = std::max(
        0.0f, static_cast<float>(std::max(1, list->size()) *
                                     list->itemHeight -
                                 list->getContentHeight()));
    list->scrollOffset =
        std::clamp(previousScrollOffset, 0.0f, maxScrollOffset);
    if (revealPreferredIfOutOfView && selectedIndex >= 0) {
      const float itemTop =
          static_cast<float>(selectedIndex * list->itemHeight);
      const float itemBottom =
          itemTop + static_cast<float>(list->itemHeight);
      const float viewportTop = list->scrollOffset;
      const float viewportBottom =
          viewportTop + static_cast<float>(list->getContentHeight());
      if (itemTop < viewportTop || itemBottom > viewportBottom) {
        list->scrollOffset = std::clamp(itemTop, 0.0f, maxScrollOffset);
      }
    }
  }
  list->rebindVisibleItems();
}

void MusicPlayerScene::rebuildPlaylistChoices() {
  playlistChoices.clear();
  playlistChoices.reserve(playlists.size() + 1);
  playlistChoices.push_back(nowPlayingPlaylistInfo());
  playlistChoices.insert(playlistChoices.end(), playlists.begin(),
                         playlists.end());
}

void MusicPlayerScene::refreshLibraryPlaylistList(int preferredPlaylistId) {
  if (libraryPlaylistList == nullptr && favoritesPlaylistList == nullptr) {
    return;
  }
  rebuildPlaylistChoices();

  int playlistIndex = -1;
  const int targetPlaylistId =
      preferredPlaylistId != 0 ? preferredPlaylistId
                               : (selectedLibraryPlaylistId != 0
                                      ? selectedLibraryPlaylistId
                                      : selectedPlaylistId);
  playlistIndex = playlistChoiceIndexForId(targetPlaylistId);
  if (playlistIndex < 0 && !playlistChoices.empty()) {
    playlistIndex = 0;
  }

  selectedLibraryPlaylistIndex = playlistIndex;
  selectedLibraryPlaylistId =
      playlistIndex >= 0
          ? playlistChoices[static_cast<std::size_t>(playlistIndex)].id
          : 0;

  const auto updateList = [this](RecyclerView<PlaylistInfo> *list) {
    if (list == nullptr) {
      return;
    }
    list->setItems(playlistChoices);
    list->selectedIndex = selectedLibraryPlaylistIndex;
    list->rebindVisibleItems();
  };
  updateList(libraryPlaylistList);
  updateList(favoritesPlaylistList);
}

void MusicPlayerScene::refreshPlaylistDirectoryList(int preferredPlaylistId) {
  if (playlistDirectoryList == nullptr) {
    return;
  }
  rebuildPlaylistChoices();
  if (pendingDeletePlaylistId != 0 &&
      playlistChoiceIndexForId(pendingDeletePlaylistId) < 0) {
    pendingDeletePlaylistId = 0;
  }
  playlistDirectoryList->setItems(playlistChoices);
  int playlistIndex = -1;
  const int targetPlaylistId =
      preferredPlaylistId != 0 ? preferredPlaylistId : selectedPlaylistId;
  playlistIndex = playlistChoiceIndexForId(targetPlaylistId);
  if (playlistIndex < 0 && !playlistChoices.empty()) {
    playlistIndex = 0;
  }
  selectedPlaylistDirectoryIndex = playlistIndex;
  selectedPlaylistId =
      playlistIndex >= 0
          ? playlistChoices[static_cast<std::size_t>(playlistIndex)].id
          : 0;
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    playlistTracks = queueTracks;
  }
  playlistDirectoryList->selectedIndex = selectedPlaylistDirectoryIndex;
  playlistDirectoryList->rebindVisibleItems();
  if (playlistNameInput != nullptr &&
      trimPlaylistName(playlistNameInput->getText()).empty()) {
    playlistNameInput->setEditingText(nextPlaylistName());
  }
  if (playlistRenameInput != nullptr) {
    playlistRenameInput->setEditingText(selectedPlaylistName());
  }
}

void MusicPlayerScene::refreshPlaylistList(int preferredIndex) {
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    playlistTracks = queueTracks;
  }
  const int trackCount = static_cast<int>(playlistTracks.size());
  selectedPlaylistIndex =
      preferredIndex >= 0 && preferredIndex < trackCount
          ? preferredIndex
          : (playlistTracks.empty() ? -1 : 0);
  if (playlistList != nullptr) {
    playlistList->setItems(playlistTracks);
  }
  refreshPlaylistSelectionViews();
}

void MusicPlayerScene::refreshPlaylistSelectionViews() {
  if (playlistList != nullptr) {
    playlistList->selectedIndex = selectedPlaylistIndex;
    playlistList->rebindVisibleItems();
  }
}

void MusicPlayerScene::refreshActiveQueueList(bool force) {
  const auto snapshot = context.musicPlayer.QueueSnapshot();
  const int previousIndex = selectedQueueIndex;
  const int previousPlaylistIndex = selectedPlaylistIndex;
  (void)force;
  const bool tracksChanged = !sameTrackList(queueTracks, snapshot.tracks);
  const bool queueLabelChanged = snapshot.displayName != displayedQueueName;

  displayedRepeatMode = snapshot.repeatMode;
  displayedQueueName = snapshot.displayName;
  queueTracks = snapshot.tracks;
  selectedQueueIndex =
      snapshot.currentIndex && *snapshot.currentIndex < queueTracks.size()
          ? static_cast<int>(*snapshot.currentIndex)
          : -1;

  if (playerQueueList != nullptr) {
    playerQueueList->selectedIndex = selectedQueueIndex;
    if (tracksChanged) {
      playerQueueList->setItems(queueTracks);
      playerQueueList->selectedIndex = selectedQueueIndex;
      playerQueueList->rebindVisibleItems();
    } else if (previousIndex != selectedQueueIndex) {
      if (previousIndex >= 0) {
        if (auto *previousView =
                playerQueueList->getViewByIndex(previousIndex)) {
          previousView->onUnselected();
        }
      }
      if (selectedQueueIndex >= 0) {
        if (auto *selectedView =
                playerQueueList->getViewByIndex(selectedQueueIndex)) {
          selectedView->onSelected();
        }
      }
    }
  }

  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    playlistTracks = queueTracks;
    if (playlistList != nullptr) {
      if (tracksChanged) {
        refreshPlaylistList(selectedQueueIndex);
      } else if (previousPlaylistIndex != selectedQueueIndex) {
        selectPlaylistTrack(selectedQueueIndex);
      }
    }
  }
  if (tracksChanged || queueLabelChanged) {
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
  }
}

void MusicPlayerScene::refreshUi() {
  const auto playback = context.musicPlayer.PlaybackState();
  if (librarySubtitleText != nullptr) {
    std::string text = std::to_string(filteredLibraryTracks.size()) +
                       " of " + std::to_string(libraryTracks.size()) +
                       " music tracks";
    if (!trimPlaylistName(librarySearchText).empty()) {
      text += " matched";
    }
    librarySubtitleText->setText(text);
  }
  if (favoritesSubtitleText != nullptr) {
    std::string text = std::to_string(filteredFavoriteTracks.size()) +
                       " of " + std::to_string(favoriteTracks.size()) +
                       " favorite tracks";
    if (!trimPlaylistName(favoritesSearchText).empty()) {
      text += " matched";
    }
    favoritesSubtitleText->setText(text);
  }
  if (playlistSubtitleText != nullptr) {
    playlistSubtitleText->setText(
        selectedPlaylistName() + " | " + std::to_string(playlists.size()) +
        " playlists | " + std::to_string(playlistTracks.size()) + " tracks");
  }
  if (playerSubtitleText != nullptr) {
    playerSubtitleText->setText(queueDisplayName(displayedQueueName) + " | " +
                                std::to_string(queueTracks.size()) +
                                " tracks | " +
                                repeatModeLabel(displayedRepeatMode));
  }
  if (queueTitleText != nullptr) {
    queueTitleText->setText(queueDisplayName(displayedQueueName));
  }
  if (deletePlaylistButtonText != nullptr) {
    deletePlaylistButtonText->setText(
        pendingDeletePlaylistId != 0 &&
                pendingDeletePlaylistId == selectedPlaylistId
            ? "Confirm Delete"
            : "Delete Playlist");
  }
  if (libraryGroupButtonText != nullptr) {
    const auto track = selectedLibraryTrack();
    if (track && !track->groupId.empty() &&
        expandedLibraryGroupIds.contains(track->groupId)) {
      libraryGroupButtonText->setText("Collapse Group");
    } else {
      libraryGroupButtonText->setText("Expand Group");
    }
  }

  std::optional<MusicTrack> current = context.musicPlayer.CurrentTrackSnapshot();
  std::optional<MusicTrack> shown =
      current.has_value() ? current : displayTrack();
  if (librarySelectionTitleText != nullptr) {
    const auto track = selectedLibraryTrack();
    librarySelectionTitleText->setText(track ? trackTitle(*track)
                                             : "No library track selected");
  }
  if (librarySelectionDetailText != nullptr) {
    const auto track = selectedLibraryTrack();
    librarySelectionDetailText->setText(
        track ? trackDetail(*track) : "No track selected.");
  }
  if (favoritesSelectionTitleText != nullptr) {
    const auto track = selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
    favoritesSelectionTitleText->setText(
        track ? trackTitle(*track) : "No favorite track selected");
  }
  if (favoritesSelectionDetailText != nullptr) {
    const auto track = selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
    favoritesSelectionDetailText->setText(
        track ? trackDetail(*track) : "No track selected.");
  }
  if (playlistSelectionTitleText != nullptr) {
    const auto track = selectedPlaylistTrack();
    playlistSelectionTitleText->setText(track ? trackTitle(*track)
                                              : "No playlist track selected");
  }
  if (playlistSelectionDetailText != nullptr) {
    const auto track = selectedPlaylistTrack();
    playlistSelectionDetailText->setText(
        track ? trackDetail(*track) : "No queue track selected.");
  }
  if (currentTitleText != nullptr) {
    currentTitleText->setText(shown ? trackTitle(*shown) : "No track selected");
  }
  if (currentDetailText != nullptr) {
    currentDetailText->setText(shown ? trackDetail(*shown)
                                     : "No music loaded.");
  }
  if (playbackText != nullptr) {
    if (!playback.supported) {
      playbackText->setText("Native playback unavailable");
    } else if (!playback.loaded) {
      playbackText->setText("Idle");
    } else {
      playbackText->setText((playback.playing ? "Playing " : "Paused ") +
                            formatMusicTime(playback.positionMicros) + " / " +
                            formatMusicTime(playback.durationMicros));
    }
  }
  if (seekProgressFill != nullptr) {
    const float fraction =
        playback.loaded && playback.durationMicros > 0
            ? std::clamp(static_cast<float>(playback.positionMicros) /
                             static_cast<float>(playback.durationMicros),
                         0.0f, 1.0f)
            : 0.0f;
    setSeekFillFraction(seekProgressFill, fraction);
  }
  if (playPauseButtonText != nullptr) {
    playPauseButtonText->setText(
        playback.playing ? "Pause" : (playback.loaded ? "Resume" : "Play"));
  }
  if (repeatModeButtonText != nullptr) {
    repeatModeButtonText->setText(repeatModeLabel(displayedRepeatMode));
  }
  if (statusText != nullptr) {
    statusText->setText(statusMessage.empty()
                            ? "Ready."
                            : statusMessage);
  }
  refreshNavigation();
  refreshLibraryArtwork(selectedLibraryTrack());
  refreshTrackBrowserArtwork(
      TrackBrowserKind::Favorites,
      selectedTrackBrowserTrack(TrackBrowserKind::Favorites));
  refreshArtwork(shown);
}

void MusicPlayerScene::refreshNavigation() {
  const auto styleNav = [this](Button *button, TextView *text,
                               MusicPlayerTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      styleButton(button, text, ui_theme::primaryAction,
                  ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
                  ui_theme::accentBorderStrong);
    } else {
      styleButton(button, text, ui_theme::control, ui_theme::controlHover,
                  ui_theme::controlPressed, ui_theme::hairlineStrong);
    }
  };
  styleNav(libraryNavButton, libraryNavText, MusicPlayerTab::Library);
  styleNav(favoritesNavButton, favoritesNavText, MusicPlayerTab::Favorites);
  styleNav(playlistsNavButton, playlistsNavText, MusicPlayerTab::Playlists);
  styleNav(playerNavButton, playerNavText, MusicPlayerTab::Player);

  if (libraryPage != nullptr) {
    libraryPage->setVisible(activeTab == MusicPlayerTab::Library);
  }
  if (favoritesPage != nullptr) {
    favoritesPage->setVisible(activeTab == MusicPlayerTab::Favorites);
  }
  if (playlistsPage != nullptr) {
    playlistsPage->setVisible(activeTab == MusicPlayerTab::Playlists);
  }
  if (playerPage != nullptr) {
    playerPage->setVisible(activeTab == MusicPlayerTab::Player);
  }
}

void MusicPlayerScene::refreshArtwork(const std::optional<MusicTrack> &track) {
  const std::filesystem::path path =
      track ? artworkPathForDisplay(*track) : std::filesystem::path{};
  if (path == displayedArtworkPath) {
    return;
  }
  displayedArtworkPath = path;
  if (artworkImage == nullptr || artworkFallbackText == nullptr) {
    return;
  }
  if (path.empty()) {
    artworkImage->freeImage();
    artworkFallbackText->setVisible(true);
    return;
  }
  artworkImage->setImageAsync(fspath_to_path_t(path), true);
  artworkFallbackText->setVisible(true);
}

void MusicPlayerScene::refreshLibraryArtwork(
    const std::optional<MusicTrack> &track) {
  refreshTrackBrowserArtwork(TrackBrowserKind::Library, track);
}

void MusicPlayerScene::refreshTrackBrowserArtwork(
    TrackBrowserKind kind, const std::optional<MusicTrack> &track) {
  const std::filesystem::path path =
      track ? artworkPathForDisplay(*track) : std::filesystem::path{};
  auto &displayedPath = trackBrowserDisplayedArtworkPath(kind);
  if (path == displayedPath) {
    return;
  }
  displayedPath = path;
  auto *image = trackBrowserArtworkImage(kind);
  auto *fallback = trackBrowserArtworkFallbackText(kind);
  if (image == nullptr || fallback == nullptr) {
    return;
  }
  if (path.empty()) {
    image->freeImage();
    fallback->setVisible(true);
    return;
  }
  image->setImageAsync(fspath_to_path_t(path), true);
  fallback->setVisible(true);
}

void MusicPlayerScene::setStatus(std::string message) {
  statusMessage = std::move(message);
  if (statusText != nullptr) {
    statusText->setText(statusMessage);
  }
}

std::vector<MusicPlayerScene::MusicTrack> &
MusicPlayerScene::trackBrowserSourceTracks(TrackBrowserKind kind) {
  return kind == TrackBrowserKind::Library ? libraryTracks : favoriteTracks;
}

const std::vector<MusicPlayerScene::MusicTrack> &
MusicPlayerScene::trackBrowserSourceTracks(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? libraryTracks : favoriteTracks;
}

std::vector<MusicPlayerScene::MusicTrack> &
MusicPlayerScene::trackBrowserFilteredTracks(TrackBrowserKind kind) {
  return kind == TrackBrowserKind::Library ? filteredLibraryTracks
                                           : filteredFavoriteTracks;
}

const std::vector<MusicPlayerScene::MusicTrack> &
MusicPlayerScene::trackBrowserFilteredTracks(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? filteredLibraryTracks
                                           : filteredFavoriteTracks;
}

int &MusicPlayerScene::trackBrowserSelectedIndex(TrackBrowserKind kind) {
  return kind == TrackBrowserKind::Library ? selectedLibraryIndex
                                           : selectedFavoriteIndex;
}

int MusicPlayerScene::trackBrowserSelectedIndex(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? selectedLibraryIndex
                                           : selectedFavoriteIndex;
}

std::string &MusicPlayerScene::trackBrowserSearchText(TrackBrowserKind kind) {
  return kind == TrackBrowserKind::Library ? librarySearchText
                                           : favoritesSearchText;
}

const std::string &
MusicPlayerScene::trackBrowserSearchText(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySearchText
                                           : favoritesSearchText;
}

RecyclerView<MusicPlayerScene::MusicTrack> *
MusicPlayerScene::trackBrowserList(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? libraryList : favoritesList;
}

TextView *MusicPlayerScene::trackBrowserSubtitleText(
    TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySubtitleText
                                           : favoritesSubtitleText;
}

TextView *MusicPlayerScene::trackBrowserSelectionTitleText(
    TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySelectionTitleText
                                           : favoritesSelectionTitleText;
}

TextView *MusicPlayerScene::trackBrowserSelectionDetailText(
    TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySelectionDetailText
                                           : favoritesSelectionDetailText;
}

ImageView *MusicPlayerScene::trackBrowserArtworkImage(
    TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? libraryArtworkImage
                                           : favoritesArtworkImage;
}

TextView *MusicPlayerScene::trackBrowserArtworkFallbackText(
    TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? libraryArtworkFallbackText
                                           : favoritesArtworkFallbackText;
}

std::filesystem::path &
MusicPlayerScene::trackBrowserDisplayedArtworkPath(TrackBrowserKind kind) {
  return kind == TrackBrowserKind::Library ? displayedLibraryArtworkPath
                                           : displayedFavoritesArtworkPath;
}

std::optional<MusicPlayerScene::MusicTrack>
MusicPlayerScene::selectedLibraryTrack() const {
  return selectedTrackBrowserTrack(TrackBrowserKind::Library);
}

std::optional<MusicPlayerScene::MusicTrack>
MusicPlayerScene::selectedTrackBrowserTrack(TrackBrowserKind kind) const {
  const int selectedIndex = trackBrowserSelectedIndex(kind);
  const auto &tracks = trackBrowserFilteredTracks(kind);
  if (selectedIndex < 0 ||
      selectedIndex >= static_cast<int>(tracks.size())) {
    return std::nullopt;
  }
  return tracks[static_cast<std::size_t>(selectedIndex)];
}

std::optional<MusicPlayerScene::PlaylistInfo>
MusicPlayerScene::selectedLibraryPlaylistInfo() const {
  if (selectedLibraryPlaylistIndex < 0 ||
      selectedLibraryPlaylistIndex >=
          static_cast<int>(playlistChoices.size())) {
    return std::nullopt;
  }
  return playlistChoices[static_cast<std::size_t>(
      selectedLibraryPlaylistIndex)];
}

std::optional<MusicPlayerScene::PlaylistInfo>
MusicPlayerScene::selectedPlaylistInfo() const {
  if (selectedPlaylistDirectoryIndex < 0 ||
      selectedPlaylistDirectoryIndex >=
          static_cast<int>(playlistChoices.size())) {
    return std::nullopt;
  }
  return playlistChoices[static_cast<std::size_t>(
      selectedPlaylistDirectoryIndex)];
}

std::optional<MusicPlayerScene::MusicTrack>
MusicPlayerScene::selectedPlaylistTrack() const {
  if (selectedPlaylistIndex < 0 ||
      selectedPlaylistIndex >= static_cast<int>(playlistTracks.size())) {
    return std::nullopt;
  }
  return playlistTracks[static_cast<std::size_t>(selectedPlaylistIndex)];
}

std::optional<MusicPlayerScene::MusicTrack> MusicPlayerScene::displayTrack()
    const {
  if (selectedQueueIndex >= 0 &&
      selectedQueueIndex < static_cast<int>(queueTracks.size())) {
    return queueTracks[static_cast<std::size_t>(selectedQueueIndex)];
  }
  if (const auto playlistTrack = selectedPlaylistTrack()) {
    return playlistTrack;
  }
  if (activeTab == MusicPlayerTab::Favorites) {
    if (const auto favoriteTrack =
            selectedTrackBrowserTrack(TrackBrowserKind::Favorites)) {
      return favoriteTrack;
    }
  }
  return selectedLibraryTrack();
}

MusicPlayerScene::PlaylistInfo MusicPlayerScene::nowPlayingPlaylistInfo()
    const {
  return {.id = kNowPlayingPlaylistId,
          .name = music_playlist::kNowPlayingDisplayName,
          .trackCount = static_cast<int>(queueTracks.size())};
}

int MusicPlayerScene::playlistChoiceIndexForId(int playlistId) const {
  for (std::size_t i = 0; i < playlistChoices.size(); ++i) {
    if (playlistChoices[i].id == playlistId) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int MusicPlayerScene::trackIndexInList(const std::vector<MusicTrack> &tracks,
                                       const MusicTrack &track) const {
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (sameTrackIdentity(tracks[i], track)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool MusicPlayerScene::selectedPlaylistIsActiveQueue() const {
  if (selectedPlaylistId <= 0 || isNowPlayingPlaylistId(selectedPlaylistId) ||
      playlistTracks.empty()) {
    return false;
  }
  if (queueDisplayName(displayedQueueName) != selectedPlaylistName()) {
    return false;
  }
  return sameTrackList(queueTracks, playlistTracks);
}

bool MusicPlayerScene::isFavoriteTrack(const MusicTrack &track) const {
  return favoriteTrackIds.contains(favoriteKeyForTrack(track));
}

void MusicPlayerScene::rebuildFavoriteTrackIds() {
  favoriteTrackIds.clear();
  favoriteTrackIds.reserve(favoriteTracks.size());
  for (const auto &track : favoriteTracks) {
    favoriteTrackIds.insert(favoriteKeyForTrack(track));
  }
}

void MusicPlayerScene::rebindFavoriteAwareTrackLists() {
  if (libraryList != nullptr) {
    libraryList->rebindVisibleItems();
  }
  if (favoritesList != nullptr) {
    favoritesList->rebindVisibleItems();
  }
  if (playlistList != nullptr) {
    playlistList->rebindVisibleItems();
  }
  if (playerQueueList != nullptr) {
    playerQueueList->rebindVisibleItems();
  }
}

std::string MusicPlayerScene::selectedLibraryPlaylistName() const {
  if (const auto playlist = selectedLibraryPlaylistInfo()) {
    return playlist->name.empty() ? "Untitled Playlist" : playlist->name;
  }
  return "No playlist";
}

std::string MusicPlayerScene::selectedPlaylistName() const {
  if (const auto playlist = selectedPlaylistInfo()) {
    return playlist->name.empty() ? "Untitled Playlist" : playlist->name;
  }
  return "No playlist";
}

std::string MusicPlayerScene::nextPlaylistName() const {
  auto exists = [this](const std::string &name) {
    return std::any_of(playlists.begin(), playlists.end(),
                       [&name](const PlaylistInfo &playlist) {
                         return playlist.name == name;
                       });
  };
  for (int i = 2; i < 10000; ++i) {
    const std::string name = "Playlist " + std::to_string(i);
    if (!exists(name)) {
      return name;
    }
  }
  return "Playlist";
}

std::string MusicPlayerScene::uniquePlaylistName(std::string desiredName) const {
  desiredName = trimPlaylistName(desiredName);
  if (desiredName.empty()) {
    desiredName = music_playlist::kNowPlayingDisplayName;
  }

  auto exists = [this](const std::string &name) {
    return std::any_of(playlists.begin(), playlists.end(),
                       [&name](const PlaylistInfo &playlist) {
                         return playlist.name == name;
                       });
  };
  if (!exists(desiredName)) {
    return desiredName;
  }

  for (int i = 2; i < 10000; ++i) {
    const std::string name = desiredName + " " + std::to_string(i);
    if (!exists(name)) {
      return name;
    }
  }
  return desiredName + " Copy";
}

void MusicPlayerScene::switchTab(MusicPlayerTab tab) {
  activeTab = tab;
  if (activeTab != MusicPlayerTab::Player) {
    seekMouseDown = false;
    activeSeekTouchId = -1;
  }
  refreshNavigation();
  refreshUi();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MusicPlayerScene::createPlaylist() {
  std::string name =
      playlistNameInput != nullptr ? playlistNameInput->getText() : "";
  name = trimPlaylistName(name);
  if (name.empty()) {
    name = nextPlaylistName();
  }
  std::string errorMessage;
  const int playlistId = context.musicPlayer.CreatePlaylist(name, errorMessage);
  if (playlistId > 0) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(playlistId);
    refreshLibraryPlaylistList(playlistId);
    refreshPlaylistList(-1);
    if (playlistNameInput != nullptr) {
      playlistNameInput->setEditingText(nextPlaylistName());
    }
    setStatus("Created " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::saveNowPlayingAsPlaylist() {
  const auto snapshot = context.musicPlayer.QueueSnapshot();
  if (snapshot.tracks.empty()) {
    setStatus("Now Playing is empty.");
    return;
  }

  std::string name = activeTab == MusicPlayerTab::Playlists &&
                             playlistNameInput != nullptr
                         ? playlistNameInput->getText()
                         : "";
  name = uniquePlaylistName(name.empty() ? music_playlist::kNowPlayingDisplayName
                                         : name);

  std::string errorMessage;
  const int playlistId =
      context.musicPlayer.CreatePlaylistFromTracks(name, snapshot.tracks,
                                                   errorMessage);
  if (playlistId <= 0) {
    setStatus(errorMessage);
    return;
  }

  playlists = context.musicPlayer.PlaylistsSnapshot();
  selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
  playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
  refreshActiveQueueList(true);
  refreshPlaylistDirectoryList(playlistId);
  refreshLibraryPlaylistList(playlistId);
  refreshPlaylistList(-1);
  if (playlistNameInput != nullptr) {
    playlistNameInput->setEditingText(nextPlaylistName());
  }
  setStatus("Saved Now Playing as " + selectedPlaylistName() + ".");
}

void MusicPlayerScene::renameSelectedPlaylist() {
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    setStatus("Now Playing cannot be renamed.");
    return;
  }

  std::string name =
      playlistRenameInput != nullptr ? playlistRenameInput->getText() : "";
  name = trimPlaylistName(name);
  if (name.empty()) {
    setStatus("Playlist name is empty.");
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.RenameSelectedPlaylist(name, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedPlaylistId);
    refreshPlaylistList(selectedPlaylistIndex);
    setStatus("Renamed playlist.");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::deleteSelectedPlaylist() {
  if (selectedPlaylistId <= 0) {
    pendingDeletePlaylistId = 0;
    setStatus("Select a playlist first.");
    refreshUi();
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    pendingDeletePlaylistId = 0;
    setStatus("Now Playing cannot be deleted. Use Clear to empty it.");
    refreshUi();
    return;
  }

  const std::string playlistName = selectedPlaylistName();
  if (pendingDeletePlaylistId != selectedPlaylistId) {
    pendingDeletePlaylistId = selectedPlaylistId;
    setStatus("Tap Confirm Delete to remove " + playlistName + ".");
    refreshUi();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.DeleteSelectedPlaylist(errorMessage)) {
    pendingDeletePlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
    refreshPlaylistList(-1);
    setStatus("Deleted " + playlistName + ".");
    refreshUi();
  } else {
    pendingDeletePlaylistId = 0;
    setStatus(errorMessage);
    refreshUi();
  }
}

void MusicPlayerScene::selectLibraryPlaylist(int index) {
  const int previousIndex = selectedLibraryPlaylistIndex;
  if (index < 0 || index >= static_cast<int>(playlistChoices.size())) {
    selectedLibraryPlaylistIndex = -1;
    selectedLibraryPlaylistId = 0;
  } else {
    selectedLibraryPlaylistIndex = index;
    selectedLibraryPlaylistId =
        playlistChoices[static_cast<std::size_t>(index)].id;
  }

  const auto updateList = [this,
                           previousIndex](RecyclerView<PlaylistInfo> *list) {
    if (list == nullptr) {
      return;
    }
    list->selectedIndex = selectedLibraryPlaylistIndex;
    if (previousIndex >= 0 && previousIndex != selectedLibraryPlaylistIndex) {
      if (auto *previousView = list->getViewByIndex(previousIndex)) {
        previousView->onUnselected();
      }
    }
    if (selectedLibraryPlaylistIndex >= 0) {
      if (auto *selectedView =
              list->getViewByIndex(selectedLibraryPlaylistIndex)) {
        selectedView->onSelected();
      }
    }
  };
  updateList(libraryPlaylistList);
  updateList(favoritesPlaylistList);
}

void MusicPlayerScene::selectTrackBrowserTrack(TrackBrowserKind kind,
                                               int index) {
  const int previousIndex = trackBrowserSelectedIndex(kind);
  auto &selectedIndex = trackBrowserSelectedIndex(kind);
  const auto &tracks = trackBrowserFilteredTracks(kind);
  selectedIndex = index >= 0 && index < static_cast<int>(tracks.size())
                      ? index
                      : -1;

  if (auto *list = trackBrowserList(kind)) {
    list->selectedIndex = selectedIndex;
    if (previousIndex >= 0 && previousIndex != selectedIndex) {
      if (auto *previousView = list->getViewByIndex(previousIndex)) {
        previousView->onUnselected();
      }
    }
    if (selectedIndex >= 0) {
      if (auto *selectedView = list->getViewByIndex(selectedIndex)) {
        selectedView->onSelected();
      }
    }
  }

  refreshTrackBrowserArtwork(kind, selectedTrackBrowserTrack(kind));
  refreshUi();
}

void MusicPlayerScene::toggleFavorite(const MusicTrack &track) {
  const bool nextFavorite = !isFavoriteTrack(track);
  const auto previousFavorite =
      selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
  const std::string previousFavoriteId =
      previousFavorite ? previousFavorite->trackId : "";
  const int previousFavoriteIndex = selectedFavoriteIndex;
  std::string fallbackFavoriteId;
  if (!nextFavorite && previousFavoriteId == favoriteKeyForTrack(track)) {
    const int nextIndex = previousFavoriteIndex + 1;
    if (nextIndex >= 0 &&
        nextIndex < static_cast<int>(filteredFavoriteTracks.size())) {
      fallbackFavoriteId =
          filteredFavoriteTracks[static_cast<std::size_t>(nextIndex)].trackId;
    } else if (previousFavoriteIndex > 0 &&
               previousFavoriteIndex - 1 <
                   static_cast<int>(filteredFavoriteTracks.size())) {
      fallbackFavoriteId =
          filteredFavoriteTracks[static_cast<std::size_t>(
                                     previousFavoriteIndex - 1)]
              .trackId;
    }
  } else if (!nextFavorite) {
    fallbackFavoriteId = previousFavoriteId;
  }

  std::string errorMessage;
  if (!context.musicPlayer.SetFavorite(track.representativeChart, nextFavorite,
                                       errorMessage)) {
    setStatus(errorMessage);
    refreshUi();
    return;
  }

  const std::string toggledTrackId = favoriteKeyForTrack(track);
  favoriteTracks = context.musicPlayer.FavoriteTracksSnapshot();
  rebuildFavoriteTrackIds();
  applyTrackBrowserFilterForTrackId(
      TrackBrowserKind::Favorites,
      nextFavorite ? toggledTrackId : fallbackFavoriteId,
      true, true);
  rebindFavoriteAwareTrackLists();
  refreshTrackBrowserArtwork(TrackBrowserKind::Favorites,
                             selectedTrackBrowserTrack(
                                 TrackBrowserKind::Favorites));
  setStatus(nextFavorite ? "Added to Favorites." : "Removed from Favorites.");
  refreshUi();
}

void MusicPlayerScene::toggleSelectedLibraryGroup() {
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus("Select a library track first.");
    return;
  }
  if (track->groupId.empty()) {
    setStatus("Selected track has no chart group.");
    return;
  }

  if (expandedLibraryGroupIds.contains(track->groupId)) {
    expandedLibraryGroupIds.erase(track->groupId);
    applyLibraryFilterForTrackId(track->groupId, true, true);
    refreshLibraryArtwork(selectedLibraryTrack());
    refreshUi();
    setStatus("Collapsed chart group.");
    return;
  }

  if (!track->groupRepresentative && !track->expandedChart) {
    setStatus("Selected track has no grouped charts.");
    return;
  }

  auto childrenIt = libraryGroupTracks.find(track->groupId);
  if (childrenIt == libraryGroupTracks.end()) {
    std::vector<MusicTrack> groupTracks;
    std::string errorMessage;
    if (!context.musicPlayer.LoadLibraryGroupTracks(*track, groupTracks,
                                                    errorMessage)) {
      setStatus(errorMessage);
      return;
    }
    childrenIt =
        libraryGroupTracks.emplace(track->groupId, std::move(groupTracks))
            .first;
  }

  if (childrenIt->second.size() <= 1) {
    setStatus("Selected track has no alternate charts.");
    return;
  }

  expandedLibraryGroupIds.insert(track->groupId);
  applyLibraryFilterForTrackId(childrenIt->second.front().trackId, true);
  refreshLibraryArtwork(selectedLibraryTrack());
  refreshUi();
  setStatus("Expanded chart group.");
}

void MusicPlayerScene::selectPlaylist(int index) {
  if (index < 0 || index >= static_cast<int>(playlistChoices.size())) {
    setStatus("Select a playlist first.");
    return;
  }
  const int playlistId = playlistChoices[static_cast<std::size_t>(index)].id;
  if (isNowPlayingPlaylistId(playlistId)) {
    selectedPlaylistId = kNowPlayingPlaylistId;
    playlistTracks = queueTracks;
    selectedPlaylistDirectoryIndex = index;
    if (playlistDirectoryList != nullptr) {
      playlistDirectoryList->selectedIndex = selectedPlaylistDirectoryIndex;
    }
    if (playlistRenameInput != nullptr) {
      playlistRenameInput->setEditingText(
          music_playlist::kNowPlayingDisplayName);
    }
    refreshPlaylistList(selectedQueueIndex);
    setStatus("Selected Now Playing.");
    refreshUi();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.SelectPlaylist(playlistId, errorMessage)) {
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    selectedPlaylistDirectoryIndex = index;
    if (playlistDirectoryList != nullptr) {
      playlistDirectoryList->selectedIndex = selectedPlaylistDirectoryIndex;
    }
    if (playlistRenameInput != nullptr) {
      playlistRenameInput->setEditingText(selectedPlaylistName());
    }
    refreshPlaylistList(-1);
    setStatus("Selected " + selectedPlaylistName() + ".");
    refreshUi();
  } else {
    setStatus(errorMessage);
    refreshUi();
  }
}

void MusicPlayerScene::selectPlaylistTrack(int index) {
  const int previousIndex = selectedPlaylistIndex;
  if (index < 0 || index >= static_cast<int>(playlistTracks.size())) {
    selectedPlaylistIndex = -1;
  } else {
    selectedPlaylistIndex = index;
  }
  auto updateSelection = [previousIndex, this](RecyclerView<MusicTrack> *list) {
    if (list == nullptr) {
      return;
    }
    list->selectedIndex = selectedPlaylistIndex;
    if (previousIndex >= 0 && previousIndex != selectedPlaylistIndex) {
      if (auto *previousView = list->getViewByIndex(previousIndex)) {
        previousView->onUnselected();
      }
    }
    if (selectedPlaylistIndex >= 0) {
      if (auto *selectedView = list->getViewByIndex(selectedPlaylistIndex)) {
        selectedView->onSelected();
      }
    }
  };
  updateSelection(playlistList);
  refreshUi();
}

void MusicPlayerScene::selectQueueTrack(int index) {
  const int previousIndex = selectedQueueIndex;
  selectedQueueIndex = index >= 0 && index < static_cast<int>(queueTracks.size())
                           ? index
                           : -1;
  if (playerQueueList != nullptr) {
    playerQueueList->selectedIndex = selectedQueueIndex;
    if (previousIndex >= 0 && previousIndex != selectedQueueIndex) {
      if (auto *previousView = playerQueueList->getViewByIndex(previousIndex)) {
        previousView->onUnselected();
      }
    }
    if (selectedQueueIndex >= 0) {
      if (auto *selectedView =
              playerQueueList->getViewByIndex(selectedQueueIndex)) {
        selectedView->onSelected();
      }
    }
  }
  refreshUi();
}

void MusicPlayerScene::addLibraryTrackToPlaylist() {
  addTrackBrowserTrackToPlaylist(TrackBrowserKind::Library);
}

void MusicPlayerScene::addTrackBrowserTrackToPlaylist(TrackBrowserKind kind) {
  const auto track = selectedTrackBrowserTrack(kind);
  if (!track) {
    setStatus(kind == TrackBrowserKind::Library
                  ? "Select a library track first."
                  : "Select a favorite track first.");
    return;
  }
  const auto targetPlaylist = selectedLibraryPlaylistInfo();
  if (!targetPlaylist) {
    setStatus("Select a playlist first.");
    return;
  }
  const int targetPlaylistId = targetPlaylist->id;
  const std::string targetPlaylistName = selectedLibraryPlaylistName();
  if (isNowPlayingPlaylistId(targetPlaylistId)) {
    addLibraryTrackToNowPlaying(*track);
    return;
  }

  const bool wasActiveQueue =
      targetPlaylistId == selectedPlaylistId && selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  std::string errorMessage;
  if (context.musicPlayer.AddChartToPlaylist(targetPlaylistId,
                                             track->representativeChart,
                                             errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    syncActiveQueueAfterPlaylistEdit(wasActiveQueue, previousCurrent,
                                     selectedQueueIndex);
    refreshLibraryPlaylistList(targetPlaylistId);
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(selectedPlaylistId == targetPlaylistId
                            ? static_cast<int>(playlistTracks.size()) - 1
                            : selectedPlaylistIndex);
    setStatus("Added to " + targetPlaylistName + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::addLibraryTrackToNowPlaying(const MusicTrack &track) {
  std::vector<MusicTrack> tracks = queueTracks;
  const int nextIndex = static_cast<int>(tracks.size());
  tracks.push_back(track);
  replaceNowPlaying(std::move(tracks), selectedQueueIndex >= 0
                                           ? selectedQueueIndex
                                           : nextIndex,
                    "Added to Now Playing.");
}

void MusicPlayerScene::removePlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus("Select a playlist track first.");
    return;
  }
  const int nextIndex = selectedPlaylistIndex;
  const bool wasActiveQueue = selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  const auto previousQueue = context.musicPlayer.QueueSnapshot();
  const std::optional<int> previousQueueIndex =
      previousQueue.currentIndex
          ? std::optional<int>(
                static_cast<int>(*previousQueue.currentIndex))
          : std::nullopt;
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    std::vector<MusicTrack> tracks = playlistTracks;
    tracks.erase(tracks.begin() + selectedPlaylistIndex);
    const int preferredIndex =
        std::min(nextIndex, static_cast<int>(tracks.size()) - 1);
    const auto playback = context.musicPlayer.PlaybackState();
    int queueIndex = -1;
    bool currentTrackRemoved = false;
    int detachedNextIndex =
        adjustedDetachedNextIndex(previousQueue.detachedCurrentNextIndex,
                                  nextIndex, nextIndex, tracks.size());
    if (previousQueueIndex) {
      if (*previousQueueIndex == nextIndex) {
        currentTrackRemoved = true;
      } else if (nextIndex < *previousQueueIndex) {
        queueIndex =
            std::clamp(*previousQueueIndex - 1, 0,
                       std::max(0, static_cast<int>(tracks.size()) - 1));
      } else {
        queueIndex =
            std::clamp(*previousQueueIndex, 0,
                       std::max(0, static_cast<int>(tracks.size()) - 1));
      }
    } else if (previousCurrent && playback.loaded &&
               sameTrackIdentity(*previousCurrent, *track)) {
      currentTrackRemoved = true;
    } else if (previousQueue.detachedCurrentNextIndex && playback.loaded) {
      currentTrackRemoved = true;
    }

    if (currentTrackRemoved && playback.loaded) {
      context.musicPlayer.SetPlaylistAfterCurrentRemoved(
          std::move(tracks), static_cast<std::size_t>(detachedNextIndex),
          music_playlist::kNowPlayingDisplayName);
      refreshActiveQueueList(true);
      setStatus("Removed from Now Playing.");
    } else {
      replaceNowPlaying(std::move(tracks),
                        queueIndex >= 0 ? queueIndex : preferredIndex,
                        "Removed from Now Playing.");
    }
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.RemoveChartFromSelectedPlaylist(
          track->representativeChart, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    syncActiveQueueAfterPlaylistEdit(wasActiveQueue, previousCurrent,
                                     nextIndex, previousQueueIndex, nextIndex);
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
    refreshPlaylistList(std::min(nextIndex,
                                 static_cast<int>(playlistTracks.size()) - 1));
    setStatus("Removed from " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::movePlaylistTrack(int delta) {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus("Select a playlist track first.");
    return;
  }
  const bool wasActiveQueue = selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    const int targetIndex = selectedPlaylistIndex + delta;
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(playlistTracks.size())) {
      setStatus(delta < 0 ? "Selected track is already at the top."
                          : "Selected track is already at the bottom.");
      return;
    }
    std::vector<MusicTrack> tracks = playlistTracks;
    std::swap(tracks[static_cast<std::size_t>(selectedPlaylistIndex)],
              tracks[static_cast<std::size_t>(targetIndex)]);
    replaceNowPlaying(std::move(tracks), targetIndex,
                      delta < 0 ? "Moved track up." : "Moved track down.");
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.MoveChartInSelectedPlaylist(
          track->representativeChart, delta, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    syncActiveQueueAfterPlaylistEdit(
        wasActiveQueue, previousCurrent,
        std::clamp(selectedPlaylistIndex + delta, 0,
                   std::max(0, static_cast<int>(playlistTracks.size()) - 1)));
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(std::clamp(selectedPlaylistIndex + delta, 0,
                                   std::max(0, static_cast<int>(
                                                   playlistTracks.size()) -
                                                   1)));
    setStatus(delta < 0 ? "Moved track up." : "Moved track down.");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::clearPlaylist() {
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    std::string ignoredStatus;
    context.musicPlayer.Stop(ignoredStatus);
    replaceNowPlaying({}, -1, "Cleared Now Playing.");
    return;
  }

  const bool wasActiveQueue = selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  std::string errorMessage;
  if (context.musicPlayer.ClearSelectedPlaylist(errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks.clear();
    syncActiveQueueAfterPlaylistEdit(wasActiveQueue, previousCurrent, -1);
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
    refreshPlaylistList(-1);
    setStatus("Cleared " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::syncActiveQueueAfterPlaylistEdit(
    bool wasActiveQueue, const std::optional<MusicTrack> &previousCurrent,
    int fallbackIndex, std::optional<int> previousQueueIndex,
    std::optional<int> removedIndex) {
  if (!wasActiveQueue) {
    return;
  }

  const auto previousQueue = context.musicPlayer.QueueSnapshot();
  const auto playback = context.musicPlayer.PlaybackState();
  if (playlistTracks.empty()) {
    if (previousCurrent && playback.loaded) {
      context.musicPlayer.SetPlaylistAfterCurrentRemoved(
          {}, 0, selectedPlaylistName());
    } else {
      context.musicPlayer.SetPlaylist({}, 0, selectedPlaylistName());
    }
    refreshActiveQueueList(true);
    return;
  }

  int queueIndex = -1;
  bool currentTrackRemoved = false;
  if (previousQueueIndex && removedIndex) {
    if (*removedIndex == *previousQueueIndex) {
      currentTrackRemoved = true;
    } else if (*removedIndex < *previousQueueIndex) {
      queueIndex =
          std::clamp(*previousQueueIndex - 1, 0,
                     static_cast<int>(playlistTracks.size()) - 1);
    } else {
      queueIndex =
          std::clamp(*previousQueueIndex, 0,
                     static_cast<int>(playlistTracks.size()) - 1);
    }
  }
  if (previousCurrent) {
    if (queueIndex < 0 && !currentTrackRemoved) {
      queueIndex = trackIndexInList(playlistTracks, *previousCurrent);
    }
  }
  currentTrackRemoved =
      currentTrackRemoved || (previousCurrent && queueIndex < 0);
  if (queueIndex < 0) {
    queueIndex = std::clamp(fallbackIndex, 0,
                            static_cast<int>(playlistTracks.size()) - 1);
  }

  if (currentTrackRemoved && playback.loaded) {
    const int nextIndex = adjustedDetachedNextIndex(
        previousQueue.detachedCurrentNextIndex, removedIndex, fallbackIndex,
        playlistTracks.size());
    context.musicPlayer.SetPlaylistAfterCurrentRemoved(
        playlistTracks, static_cast<std::size_t>(nextIndex),
        selectedPlaylistName());
  } else {
    context.musicPlayer.SetPlaylist(playlistTracks,
                                    static_cast<std::size_t>(queueIndex),
                                    selectedPlaylistName());
  }
  refreshActiveQueueList(true);
}

void MusicPlayerScene::replaceNowPlaying(std::vector<MusicTrack> tracks,
                                         int preferredIndex,
                                         const std::string &message) {
  const auto current = context.musicPlayer.CurrentTrackSnapshot();
  const auto playback = context.musicPlayer.PlaybackState();
  const auto previousQueue = context.musicPlayer.QueueSnapshot();
  int startIndex = tracks.empty()
                       ? 0
                       : std::clamp(preferredIndex, 0,
                                    static_cast<int>(tracks.size()) - 1);
  const int currentIndex =
      current && playback.loaded ? trackIndexInList(tracks, *current) : -1;
  if (currentIndex >= 0) {
    startIndex = currentIndex;
  }
  if (!tracks.empty() && current && playback.loaded && currentIndex < 0) {
    startIndex =
        adjustedDetachedNextIndex(previousQueue.detachedCurrentNextIndex,
                                  std::nullopt, startIndex, tracks.size());
    context.musicPlayer.SetPlaylistAfterCurrentRemoved(
        std::move(tracks), static_cast<std::size_t>(startIndex),
        music_playlist::kNowPlayingDisplayName);
  } else {
    context.musicPlayer.SetNowPlaying(std::move(tracks),
                                      static_cast<std::size_t>(startIndex));
  }
  refreshActiveQueueList(true);
  setStatus(message);
}

void MusicPlayerScene::playNowPlaying(std::vector<MusicTrack> tracks,
                                      std::size_t startIndex,
                                      const std::string &emptyMessage,
                                      const std::string &successMessage) {
  if (tracks.empty()) {
    setStatus(emptyMessage);
    return;
  }

  context.jukebox.stop();
  context.musicPlayer.SetNowPlaying(std::move(tracks), startIndex);
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, successMessage);
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playLibraryTrack() {
  playTrackBrowserTrack(TrackBrowserKind::Library);
}

void MusicPlayerScene::playTrackBrowserTrack(TrackBrowserKind kind) {
  const auto track = selectedTrackBrowserTrack(kind);
  if (!track) {
    setStatus(kind == TrackBrowserKind::Library
                  ? "Select a library track first."
                  : "Select a favorite track first.");
    return;
  }
  playNowPlaying({*track}, 0,
                 kind == TrackBrowserKind::Library
                     ? "Select a library track first."
                     : "Select a favorite track first.",
                 "Playing Now Playing.");
}

void MusicPlayerScene::playPlaylist() {
  if (playlistTracks.empty()) {
    setStatus(selectedPlaylistName() + " is empty.");
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    const std::size_t startIndex =
        selectedPlaylistIndex >= 0
            ? static_cast<std::size_t>(selectedPlaylistIndex)
            : 0;
    playNowPlaying(playlistTracks, startIndex, "Now Playing is empty.",
                   "Playing Now Playing.");
    return;
  }

  context.jukebox.stop();
  std::string status;
  if (!context.musicPlayer.StartSelectedPlaylist(status)) {
    setStatus(status);
    return;
  }
  context.musicPlayer.PlayCurrentAsync(status,
                                       "Playing " + selectedPlaylistName() + ".");
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playSelectedPlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus("Select a playlist track first.");
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    playNowPlaying(playlistTracks,
                   static_cast<std::size_t>(selectedPlaylistIndex),
                   "Now Playing is empty.", "Playing Now Playing.");
    return;
  }

  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(playlistTracks,
                                  static_cast<std::size_t>(selectedPlaylistIndex),
                                  selectedPlaylistName());
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, "Playing playlist track.");
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playSelectedQueueTrack() {
  if (selectedQueueIndex < 0 ||
      selectedQueueIndex >= static_cast<int>(queueTracks.size())) {
    setStatus("Select a queue track first.");
    return;
  }
  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(queueTracks,
                                  static_cast<std::size_t>(selectedQueueIndex),
                                  queueDisplayName(displayedQueueName));
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, "Playing queue track.");
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playRandomLibrary() {
  playRandomTrackBrowser(TrackBrowserKind::Library);
}

void MusicPlayerScene::playRandomTrackBrowser(TrackBrowserKind kind) {
  std::vector<MusicTrack> tracks;
  if (kind == TrackBrowserKind::Library) {
    tracks = trimPlaylistName(librarySearchText).empty() &&
                     expandedLibraryGroupIds.empty()
                 ? libraryTracks
                 : filteredLibraryTracks;
  } else {
    tracks = trimPlaylistName(favoritesSearchText).empty()
                 ? favoriteTracks
                 : filteredFavoriteTracks;
  }
  playNowPlaying(music_playlist::ShuffledTracks(std::move(tracks)), 0,
                 kind == TrackBrowserKind::Library
                     ? "No library tracks available."
                     : "No favorite tracks available.",
                 "Playing Now Playing.");
}

void MusicPlayerScene::togglePlayback() {
  std::string status;
  const auto playback = context.musicPlayer.PlaybackState();
  if (playback.playing) {
    context.musicPlayer.Pause(status);
  } else if (playback.loaded) {
    context.musicPlayer.Resume(status);
  } else if (selectedQueueIndex >= 0 &&
             selectedQueueIndex < static_cast<int>(queueTracks.size())) {
    playSelectedQueueTrack();
    return;
  } else if (selectedPlaylistTrack()) {
    playSelectedPlaylistTrack();
    return;
  } else if (activeTab == MusicPlayerTab::Favorites &&
             selectedTrackBrowserTrack(TrackBrowserKind::Favorites)) {
    playTrackBrowserTrack(TrackBrowserKind::Favorites);
    return;
  } else {
    playLibraryTrack();
    return;
  }
  setStatus(status);
}

void MusicPlayerScene::seekRelative(long long deltaMicros) {
  const auto playback = context.musicPlayer.PlaybackState();
  if (!playback.supported || !playback.loaded) {
    setStatus("No music is loaded.");
    return;
  }
  long long target = std::max(0LL, playback.positionMicros + deltaMicros);
  if (playback.durationMicros > 0) {
    target = std::min(target, playback.durationMicros);
  }
  std::string status;
  if (context.musicPlayer.Seek(target, status)) {
    setStatus("Seeked to " + formatMusicTime(target) + ".");
  } else {
    setStatus(status);
  }
}

void MusicPlayerScene::cycleRepeatMode() {
  const auto current = context.musicPlayer.RepeatMode();
  music_playlist::QueueRepeatMode next = music_playlist::QueueRepeatMode::None;
  switch (current) {
  case music_playlist::QueueRepeatMode::None:
    next = music_playlist::QueueRepeatMode::One;
    break;
  case music_playlist::QueueRepeatMode::One:
    next = music_playlist::QueueRepeatMode::All;
    break;
  case music_playlist::QueueRepeatMode::All:
  default:
    next = music_playlist::QueueRepeatMode::None;
    break;
  }
  context.musicPlayer.SetRepeatMode(next);
  displayedRepeatMode = next;
  setStatus(repeatModeLabel(next));
  refreshUi();
}

void MusicPlayerScene::seekToFraction(float fraction) {
  const auto playback = context.musicPlayer.PlaybackState();
  if (!playback.supported || !playback.loaded || playback.durationMicros <= 0) {
    setStatus("No seekable track loaded.");
    return;
  }
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  const long long target =
      static_cast<long long>(static_cast<double>(playback.durationMicros) *
                             static_cast<double>(fraction));
  std::string status;
  if (context.musicPlayer.Seek(target, status)) {
    setSeekFillFraction(seekProgressFill, fraction);
    setSeekFillFraction(videoProgressFill, fraction);
  } else if (!status.empty()) {
    setStatus(status);
  }
}

bool MusicPlayerScene::handleSeekEvents(SDL_Event &event) {
  if (activeTab != MusicPlayerTab::Player) {
    seekMouseDown = false;
    activeSeekTouchId = -1;
    return false;
  }
  return handleProgressSeekEvents(event, seekProgressTrack, seekMouseDown,
                                  activeSeekTouchId);
}

bool MusicPlayerScene::handleProgressSeekEvents(
    SDL_Event &event, View *progressTrack, bool &mouseDown,
    SDL_FingerID &activeTouchId) {
  if (progressTrack == nullptr || !progressTrack->getVisible()) {
    return false;
  }

  const auto seekAt = [this, progressTrack](float uiX) {
    const int width = std::max(1, progressTrack->getWidth());
    const float fraction =
        (uiX - static_cast<float>(progressTrack->getX())) /
        static_cast<float>(width);
    seekToFraction(fraction);
  };

  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return false;
    }
    int uiX = 0;
    int uiY = 0;
    mouseButtonEventToUi(event.button, uiX, uiY);
    if (!isInsideView(progressTrack, uiX, uiY)) {
      return false;
    }
    mouseDown = true;
    seekAt(static_cast<float>(uiX));
    return true;
  }
  case SDL_MOUSEMOTION: {
    if (!mouseDown || event.motion.which == SDL_TOUCH_MOUSEID) {
      return false;
    }
    int uiX = 0;
    int uiY = 0;
    mouseMotionEventToUi(event.motion, uiX, uiY);
    (void)uiY;
    seekAt(static_cast<float>(uiX));
    return true;
  }
  case SDL_MOUSEBUTTONUP: {
    if (!mouseDown || event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return false;
    }
    mouseDown = false;
    int uiX = 0;
    int uiY = 0;
    mouseButtonEventToUi(event.button, uiX, uiY);
    (void)uiY;
    seekAt(static_cast<float>(uiX));
    return true;
  }
  case SDL_FINGERDOWN: {
    if (activeTouchId != -1) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    if (!isInsideView(progressTrack, uiX, uiY)) {
      return false;
    }
    activeTouchId = event.tfinger.fingerId;
    seekAt(uiX);
    return true;
  }
  case SDL_FINGERMOTION: {
    if (event.tfinger.fingerId != activeTouchId) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    (void)uiY;
    seekAt(uiX);
    return true;
  }
  case SDL_FINGERUP: {
    if (event.tfinger.fingerId != activeTouchId) {
      return false;
    }
    activeTouchId = -1;
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    (void)uiY;
    seekAt(uiX);
    return true;
  }
  default:
    return false;
  }
}

void MusicPlayerScene::watchVideo() {
  if (videoFullscreenActive) {
    showVideoControls();
    return;
  }

  const auto playback = context.musicPlayer.PlaybackState();
  std::optional<MusicTrack> track = context.musicPlayer.CurrentTrackSnapshot();
  if (!track) {
    track = displayTrack();
  }
  if (!track) {
    setStatus("Select or play a track first.");
    return;
  }

  if (!playback.loaded) {
    playNowPlaying({*track}, 0, "Select a track first.",
                   "Playing Now Playing.");
  } else {
    context.jukebox.stop();
  }

  videoPreviousVisualsEnabled = context.jukebox.getVisualsEnabled();
  videoRestoresVisualsEnabled = true;
  context.jukebox.setVisualsEnabled(true);

  videoTrackId = favoriteKeyForTrack(*track);
  videoFullscreenActive = true;
  context.ignoreBgaPostOptions.store(true, std::memory_order_release);
  videoSeekMouseDown = false;
  activeVideoSeekTouchId = -1;
  if (rootLayout != nullptr) {
    rootLayout->setVisible(false);
  }
  if (videoOverlayRoot != nullptr) {
    videoOverlayRoot->setVisible(true);
    videoOverlayRoot->setSize(rendering::window_width,
                              rendering::window_height);
    videoOverlayRoot->applyYogaLayout();
  }
  loadVideoVisualsForTrack(*track, true);
  showVideoControls(5000);
  updateVideoFullscreen();
  refreshVideoOverlay();
}

bool MusicPlayerScene::loadVideoVisualsForTrack(const MusicTrack &track,
                                                bool showStatusMessage) {
  videoTrackId = favoriteKeyForTrack(track);
  std::atomic_bool cancelled{false};
  auto chart = play_options::parseChart(track.representativeChart, cancelled,
                                        "music video");
  if (cancelled.load() || !chart || !chartHasBgaEvents(*chart)) {
    context.jukebox.unloadVisuals();
    videoChart.reset();
    videoVisualsLoaded = false;
    showVideoArtwork(track);
    if (showStatusMessage) {
      if (cancelled.load()) {
        setStatus("Video loading cancelled. Showing jacket.");
      } else if (!chart) {
        setStatus("Could not parse chart video. Showing jacket.");
      } else {
        setStatus("No BGA is available for this track. Showing jacket.");
      }
    }
    return false;
  }

  hideVideoArtwork();
  context.jukebox.loadVisuals(*chart, cancelled);
  if (cancelled.load()) {
    context.jukebox.unloadVisuals();
    videoChart.reset();
    videoVisualsLoaded = false;
    showVideoArtwork(track);
    if (showStatusMessage) {
      setStatus("Video loading cancelled. Showing jacket.");
    }
    return false;
  }

  videoChart = std::move(chart);
  videoVisualsLoaded = true;
  if (showStatusMessage) {
    setStatus("Watching BGA.");
  }
  return true;
}

void MusicPlayerScene::exitVideoFullscreen() {
  if (!videoFullscreenActive && !videoVisualsLoaded &&
      !videoRestoresVisualsEnabled) {
    return;
  }

  videoFullscreenActive = false;
  videoVisualsLoaded = false;
  videoShowingArtwork = false;
  videoTrackId.clear();
  videoChart.reset();
  displayedVideoArtworkPath.clear();
  videoSeekMouseDown = false;
  activeVideoSeekTouchId = -1;
  videoControlsVisible = false;
  videoControlsVisibleUntil = 0;
  if (videoOverlayRoot != nullptr) {
    videoOverlayRoot->setVisible(false);
  }
  if (videoControlsPanel != nullptr) {
    videoControlsPanel->setVisible(false);
  }
  if (videoArtworkBackdrop != nullptr) {
    videoArtworkBackdrop->setVisible(false);
  }
  if (videoArtworkImage != nullptr) {
    videoArtworkImage->freeImage();
  }
  if (rootLayout != nullptr) {
    rootLayout->setVisible(true);
  }
  context.jukebox.unloadVisuals();
  context.ignoreBgaPostOptions.store(false, std::memory_order_release);
  if (videoRestoresVisualsEnabled) {
    context.jukebox.setVisualsEnabled(videoPreviousVisualsEnabled);
    videoRestoresVisualsEnabled = false;
  }
  refreshUi();
}

void MusicPlayerScene::updateVideoFullscreen() {
  if (!videoFullscreenActive) {
    return;
  }

  const auto current = context.musicPlayer.CurrentTrackSnapshot();
  if (current && !videoTrackId.empty() &&
      favoriteKeyForTrack(*current) != videoTrackId) {
    loadVideoVisualsForTrack(*current, true);
  }

  const auto playback = context.musicPlayer.PlaybackState();
  const long long position = playback.loaded ? playback.positionMicros : 0;
  if (videoVisualsLoaded) {
    context.jukebox.seekVisualsToSongTime(position);
  }

  if (videoControlsVisible && videoControlsVisibleUntil > 0 &&
      SDL_GetTicks64() > videoControlsVisibleUntil && !videoSeekMouseDown &&
      activeVideoSeekTouchId == -1) {
    hideVideoControls();
  }
}

void MusicPlayerScene::refreshVideoOverlay() {
  if (videoOverlayRoot == nullptr) {
    return;
  }
  videoOverlayRoot->setVisible(videoFullscreenActive);
  if (!videoFullscreenActive) {
    return;
  }

  const auto playback = context.musicPlayer.PlaybackState();
  const auto current = context.musicPlayer.CurrentTrackSnapshot();
  const auto shown = current ? current : displayTrack();
  if (videoTitleText != nullptr) {
    videoTitleText->setText(shown ? trackTitle(*shown) : "No track selected");
  }
  if (videoDetailText != nullptr) {
    videoDetailText->setText(shown ? trackDetail(*shown) : "No music loaded.");
  }
  if (videoPlaybackText != nullptr) {
    if (!playback.loaded) {
      videoPlaybackText->setText("Idle");
    } else {
      videoPlaybackText->setText(formatMusicTime(playback.positionMicros) +
                                 " / " +
                                 formatMusicTime(playback.durationMicros));
    }
  }
  if (videoPlayPauseButtonText != nullptr) {
    videoPlayPauseButtonText->setText(
        playback.playing ? "Pause" : (playback.loaded ? "Resume" : "Play"));
  }
  if (videoProgressFill != nullptr) {
    const float fraction =
        playback.loaded && playback.durationMicros > 0
            ? std::clamp(static_cast<float>(playback.positionMicros) /
                             static_cast<float>(playback.durationMicros),
                         0.0f, 1.0f)
            : 0.0f;
    setSeekFillFraction(videoProgressFill, fraction);
  }
  if (videoControlsPanel != nullptr) {
    videoControlsPanel->setVisible(videoControlsVisible);
  }
}

void MusicPlayerScene::showVideoArtwork(const MusicTrack &track) {
  videoShowingArtwork = true;
  const std::filesystem::path path = artworkPathForDisplay(track);
  if (videoArtworkBackdrop != nullptr) {
    videoArtworkBackdrop->setVisible(true);
  }
  if (videoArtworkImage == nullptr || videoArtworkFallbackText == nullptr) {
    return;
  }
  if (path.empty()) {
    displayedVideoArtworkPath.clear();
    videoArtworkImage->freeImage();
    videoArtworkImage->setVisible(false);
    videoArtworkFallbackText->setVisible(true);
    return;
  }
  if (path != displayedVideoArtworkPath) {
    displayedVideoArtworkPath = path;
    videoArtworkImage->setImageAsync(fspath_to_path_t(path), true);
  }
  layoutVideoArtwork();
  videoArtworkImage->setVisible(true);
  videoArtworkFallbackText->setVisible(false);
}

void MusicPlayerScene::hideVideoArtwork() {
  videoShowingArtwork = false;
  if (videoArtworkBackdrop != nullptr) {
    videoArtworkBackdrop->setVisible(false);
  }
}

void MusicPlayerScene::layoutVideoArtwork() {
  if (videoArtworkBackdrop != nullptr) {
    videoArtworkBackdrop->setSize(rendering::window_width,
                                  rendering::window_height);
    videoArtworkBackdrop->setPositionNoLayout(0, 0, YGPositionTypeAbsolute);
  }
  if (videoArtworkImage == nullptr) {
    return;
  }
  const int width = std::max(1, rendering::window_width);
  const int height = std::max(1, rendering::window_height);
  const int limit = std::max(1, std::min(width, height));
  const int size = std::clamp(static_cast<int>(static_cast<float>(limit) *
                                               0.76f),
                              180, limit);
  const int x = std::max(0, (width - size) / 2);
  const int y = std::max(0, (height - size) / 2);
  videoArtworkImage->setSize(size, size);
  videoArtworkImage->setPositionNoLayout(x, y, YGPositionTypeAbsolute);
}

void MusicPlayerScene::showVideoControls(Uint64 durationMs) {
  if (!videoFullscreenActive) {
    return;
  }
  videoControlsVisible = true;
  videoControlsVisibleUntil = SDL_GetTicks64() + durationMs;
  if (videoControlsPanel != nullptr) {
    videoControlsPanel->setVisible(true);
  }
  refreshVideoOverlay();
}

void MusicPlayerScene::hideVideoControls() {
  if (videoSeekMouseDown || activeVideoSeekTouchId != -1) {
    return;
  }
  videoControlsVisible = false;
  videoControlsVisibleUntil = 0;
  if (videoControlsPanel != nullptr) {
    videoControlsPanel->setVisible(false);
  }
}

bool MusicPlayerScene::handleVideoFullscreenEvents(SDL_Event &event) {
  if (!videoFullscreenActive) {
    return false;
  }

  if (event.type == SDL_KEYDOWN) {
    showVideoControls();
    switch (event.key.keysym.sym) {
    case SDLK_ESCAPE:
      exitVideoFullscreen();
      return true;
    case SDLK_SPACE:
      togglePlayback();
      return true;
    case SDLK_LEFT:
      seekRelative(-10000000LL);
      return true;
    case SDLK_RIGHT:
      seekRelative(10000000LL);
      return true;
    default:
      return true;
    }
  }

  const bool controlsVisible =
      videoControlsPanel != nullptr && videoControlsPanel->getVisible();
  if (controlsVisible) {
    if (event.type == SDL_MOUSEBUTTONDOWN &&
        event.button.button == SDL_BUTTON_LEFT &&
        event.button.which != SDL_TOUCH_MOUSEID) {
      int uiX = 0;
      int uiY = 0;
      mouseButtonEventToUi(event.button, uiX, uiY);
      if (!isInsideView(videoControlsPanel, uiX, uiY)) {
        hideVideoControls();
        return true;
      }
    } else if (event.type == SDL_FINGERDOWN) {
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
      if (!isInsideView(videoControlsPanel, uiX, uiY)) {
        hideVideoControls();
        return true;
      }
    }
    if (handleProgressSeekEvents(event, videoProgressTrack, videoSeekMouseDown,
                                 activeVideoSeekTouchId)) {
      showVideoControls();
      return true;
    }
    Scene::handleEvents(event);
    return true;
  }

  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN:
    if (event.button.button == SDL_BUTTON_LEFT &&
        event.button.which != SDL_TOUCH_MOUSEID) {
      showVideoControls();
      return true;
    }
    break;
  case SDL_FINGERDOWN:
    showVideoControls();
    return true;
  case SDL_MOUSEBUTTONUP:
  case SDL_MOUSEMOTION:
  case SDL_FINGERUP:
  case SDL_FINGERMOTION:
    return true;
  default:
    break;
  }
  return true;
}

void MusicPlayerScene::playNext() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayNextAsync(status, "Playing next track.");
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playPrevious() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayPreviousAsync(status, "Playing previous track.");
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::stopPlayback() {
  if (videoFullscreenActive) {
    exitVideoFullscreen();
  }
  std::string status;
  if (context.musicPlayer.Stop(status)) {
    setStatus("Stopped.");
  } else {
    setStatus(status);
  }
}

void MusicPlayerScene::goBack() {
  if (videoFullscreenActive) {
    exitVideoFullscreen();
    return;
  }
  if (context.sceneManager != nullptr) {
    context.sceneManager->changeScene("MainMenu", false);
  }
}
