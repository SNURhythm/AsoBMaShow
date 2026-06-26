#include "MusicPlayerScene.h"

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
  if (track.chartCount > 1) {
    detail += "  " + std::to_string(track.chartCount) + " charts";
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
    detail->setOverflow(TextView::TextOverflow::Hidden);
    textColumn->addView(detail);
    addView(textColumn);
  }

  void setTrack(const music_playlist::MusicTrack &track, bool selected) {
    title->setText(trackTitle(track));
    detail->setText(trackDetail(track));
    if (artwork != nullptr) {
      const auto path = artworkPathForDisplay(track);
      if (path.empty()) {
        artwork->freeImage();
      } else {
        artwork->setImageAsync(fspath_to_path_t(path));
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
      switchTab(MusicPlayerTab::Playlists);
      return {};
    }
    if (event.key.keysym.sym == SDLK_3) {
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
  refreshUi();
}

void MusicPlayerScene::renderScene() {}

void MusicPlayerScene::cleanupScene() {
  rootLayout = nullptr;
  libraryPage = nullptr;
  playlistsPage = nullptr;
  playerPage = nullptr;
  libraryNavButton = nullptr;
  playlistsNavButton = nullptr;
  playerNavButton = nullptr;
  libraryNavText = nullptr;
  playlistsNavText = nullptr;
  playerNavText = nullptr;
  railSummaryText = nullptr;
  statusText = nullptr;
  librarySubtitleText = nullptr;
  playlistSubtitleText = nullptr;
  playerSubtitleText = nullptr;
  librarySelectionTitleText = nullptr;
  librarySelectionDetailText = nullptr;
  libraryArtworkFallbackText = nullptr;
  playlistSelectionTitleText = nullptr;
  playlistSelectionDetailText = nullptr;
  currentTitleText = nullptr;
  currentDetailText = nullptr;
  playbackText = nullptr;
  queueTitleText = nullptr;
  repeatModeButtonText = nullptr;
  artworkFallbackText = nullptr;
  artworkImage = nullptr;
  libraryArtworkImage = nullptr;
  libraryList = nullptr;
  libraryPlaylistList = nullptr;
  playlistDirectoryList = nullptr;
  playlistList = nullptr;
  playerQueueList = nullptr;
  librarySearchInput = nullptr;
  playlistNameInput = nullptr;
  playlistRenameInput = nullptr;
  playPauseButtonText = nullptr;
  seekProgressTrack = nullptr;
  seekProgressFill = nullptr;
  displayedLibraryArtworkPath.clear();
  displayedArtworkPath.clear();
  displayedQueueName.clear();
  seekMouseDown = false;
  activeSeekTouchId = -1;
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
  playlistsNavButton = makeNavButton("Playlists", &playlistsNavText);
  playlistsNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Playlists); });
  playerNavButton = makeNavButton("Player", &playerNavText);
  playerNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Player); });
  rail->addView(libraryNavButton);
  rail->addView(playlistsNavButton);
  rail->addView(playerNavButton);

  auto *railSpacer = new View();
  railSpacer->setFlex(1);
  rail->addView(railSpacer);

  railSummaryText = new TextView(kFontPath, 14);
  railSummaryText->setHeight(74);
  railSummaryText->setWrap(true);
  railSummaryText->setThemedColor(ui_theme::textSecondary);
  rail->addView(railSummaryText);
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
  playlistsPage = makePage();
  buildPlaylistsPage(playlistsPage);
  playerPage = makePage();
  buildPlayerPage(playerPage);
  pageStack->addView(libraryPage);
  pageStack->addView(playlistsPage);
  pageStack->addView(playerPage);
  content->addView(pageStack);

  rootLayout->addView(content);
  refreshNavigation();
  rootLayout->applyYogaLayout();
}

void MusicPlayerScene::buildLibraryPage(View *page) {
  auto *panel = makePanel("Library", &librarySubtitleText);
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
  librarySearchInput = new TextInputBox(kFontPath, 18);
  librarySearchInput->setFlex(1);
  librarySearchInput->setHeight(52);
  librarySearchInput->setThemedBackgroundColor(ui_theme::control);
  librarySearchInput->setThemedBorderColor(ui_theme::hairlineStrong);
  librarySearchInput->setBorderWidth(1);
  librarySearchInput->setCornerRadius(ui_theme::controlRadius());
  librarySearchInput->setThemedColor(ui_theme::textPrimary);
  librarySearchInput->setVAlign(TextView::MIDDLE);
  librarySearchInput->onTextChanged([this](const std::string &value) {
    librarySearchText = value;
    applyLibraryFilter();
    refreshUi();
  });
  TextView *clearSearchText = nullptr;
  auto *clearSearchButton = makeButton("Clear", 17, &clearSearchText);
  clearSearchButton->setWidth(104);
  styleButton(clearSearchButton, clearSearchText, ui_theme::control,
              ui_theme::controlHover, ui_theme::controlPressed,
              ui_theme::hairlineStrong);
  clearSearchButton->setOnClickListener([this]() {
    librarySearchText.clear();
    if (librarySearchInput != nullptr) {
      librarySearchInput->setEditingText("");
    }
    applyLibraryFilter();
    refreshUi();
  });
  searchRow->addView(searchLabel);
  searchRow->addView(librarySearchInput);
  searchRow->addView(clearSearchButton);
  panel->addView(searchRow);

  auto *workspace = new View();
  workspace->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  panel->addView(workspace);

  libraryList = new RecyclerView<MusicTrack>(
      [](const MusicTrack &a, const MusicTrack &b) {
        return a.trackId == b.trackId && a.chartId == b.chartId;
      });
  libraryList->setFlex(1);
  libraryList->itemHeight = kTrackRowHeight;
  libraryList->reserveScrollbarGutter = true;
  libraryList->onCreateView = [](const MusicTrack &) {
    return new MusicTrackRowView();
  };
  libraryList->onBind = [](View *view, const MusicTrack &track, int,
                           bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected);
    }
  };
  libraryList->onSelected = [this](const MusicTrack &, int index) {
    if (auto *selectedView = libraryList->getViewByIndex(index)) {
      selectedView->onSelected();
    }
    selectedLibraryIndex = index;
    refreshUi();
  };
  libraryList->onUnselected = [this](const MusicTrack &, int index) {
    if (auto *unselectedView = libraryList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  workspace->addView(libraryList);

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
  libraryArtworkFallbackText = new TextView(kFontPath, 18);
  libraryArtworkFallbackText->setText("No album art");
  libraryArtworkFallbackText->setHeight(36);
  libraryArtworkFallbackText->setAlign(TextView::CENTER);
  libraryArtworkFallbackText->setVAlign(TextView::MIDDLE);
  libraryArtworkFallbackText->setThemedColor(ui_theme::textMuted);
  libraryArtFrame->addView(libraryArtworkFallbackText);
  libraryArtworkImage = new ImageView(0, 0, 0, 0);
  libraryArtworkImage->setWidth(388)
      ->setHeight(248)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setCornerRadius(ui_theme::controlRadius());
  libraryArtFrame->addView(libraryArtworkImage);
  actions->addView(libraryArtFrame);

  auto *selectionTitle = new TextView(kFontPath, 18);
  selectionTitle->setText("Selected Track");
  selectionTitle->setHeight(28);
  selectionTitle->setThemedColor(ui_theme::textSecondary);
  actions->addView(selectionTitle);

  librarySelectionTitleText = new TextView(kFontPath, 24);
  librarySelectionTitleText->setHeight(42);
  librarySelectionTitleText->setOverflow(TextView::TextOverflow::Marquee);
  librarySelectionTitleText->setThemedColor(ui_theme::textPrimary);
  actions->addView(librarySelectionTitleText);

  librarySelectionDetailText = new TextView(kFontPath, 16);
  librarySelectionDetailText->setHeight(62);
  librarySelectionDetailText->setWrap(true);
  librarySelectionDetailText->setThemedColor(ui_theme::textSecondary);
  actions->addView(librarySelectionDetailText);

  auto *primaryRow = new View();
  primaryRow->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  TextView *playTrackText = nullptr;
  auto *playTrackButton = makeButton("Play Track", 17, &playTrackText);
  playTrackButton->setFlex(1);
  styleButton(playTrackButton, playTrackText, ui_theme::primaryAction,
              ui_theme::primaryActionHover, ui_theme::primaryActionPressed,
              ui_theme::accentBorderStrong);
  playTrackButton->setOnClickListener([this]() { playLibraryTrack(); });
  TextView *addText = nullptr;
  auto *addButton = makeButton("Add to playlist", 15, &addText);
  addButton->setFlex(1);
  styleButton(addButton, addText, ui_theme::control, ui_theme::controlHover,
              ui_theme::controlPressed, ui_theme::hairlineStrong);
  addButton->setOnClickListener([this]() { addLibraryTrackToPlaylist(); });
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
  randomButton->setOnClickListener([this]() { playRandomLibrary(); });
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

  auto *addToHeader = new TextView(kFontPath, 18);
  addToHeader->setText("Add To");
  addToHeader->setHeight(28);
  addToHeader->setThemedColor(ui_theme::textSecondary);
  actions->addView(addToHeader);

  libraryPlaylistList = new RecyclerView<PlaylistInfo>(
      [](const PlaylistInfo &a, const PlaylistInfo &b) { return a.id == b.id; });
  libraryPlaylistList->setFlex(1);
  libraryPlaylistList->itemHeight = kPlaylistRowHeight;
  libraryPlaylistList->reserveScrollbarGutter = true;
  libraryPlaylistList->onCreateView = [](const PlaylistInfo &) {
    return new PlaylistRowView();
  };
  libraryPlaylistList->onBind = [](View *view, const PlaylistInfo &playlist,
                                   int, bool selected) {
    if (auto *row = dynamic_cast<PlaylistRowView *>(view)) {
      row->setPlaylist(playlist, selected);
    }
  };
  libraryPlaylistList->onSelected = [this](const PlaylistInfo &, int index) {
    selectLibraryPlaylist(index);
  };
  libraryPlaylistList->onUnselected = [this](const PlaylistInfo &, int index) {
    if (auto *unselectedView = libraryPlaylistList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  actions->addView(libraryPlaylistList);
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
  playlistSelectionDetailText->setOverflow(TextView::TextOverflow::Hidden);
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
  playlistList->onBind = [](View *view, const MusicTrack &track, int,
                            bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected);
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
  currentDetailText->setHeight(56);
  currentDetailText->setWrap(true);
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

  transportRowA->addView(previousButton);
  transportRowA->addView(playPauseButton);
  transportRowA->addView(nextButton);
  transportRowB->addView(back10Button);
  transportRowB->addView(forward10Button);
  transportRowB->addView(stopButton);
  transportRowC->addView(playSelectedButton);
  transportRowC->addView(repeatButton);
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
  playerQueueList->onBind = [](View *view, const MusicTrack &track, int,
                               bool selected) {
    if (auto *row = dynamic_cast<MusicTrackRowView *>(view)) {
      row->setTrack(track, selected);
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
  queueButtons->addView(playPlaylistButton);
  queueButtons->addView(editButton);
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
  const auto selectedPlaylist = selectedPlaylistTrack();
  const std::string selectedLibraryId =
      selectedLibrary ? selectedLibrary->trackId : "";
  const std::string selectedPlaylistTrackId =
      selectedPlaylist ? selectedPlaylist->trackId : "";
  const int preferredPlaylistId =
      preserveSelection ? selectedPlaylistId : 0;

  std::string errorMessage;
  if (!context.musicPlayer.ReloadLibrary(errorMessage)) {
    setStatus(errorMessage);
  }
  if (!context.musicPlayer.ReloadPlaylists(errorMessage)) {
    setStatus(errorMessage);
  }
  libraryTracks = context.musicPlayer.LibraryTracksSnapshot();
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
  const int playlistIndex =
      preserveSelection ? findIndex(playlistTracks, selectedPlaylistTrackId)
                        : -1;
  refreshActiveQueueList(true);
  refreshLibraryList(libraryIndex);
  refreshLibraryPlaylistList(preserveSelection ? selectedLibraryPlaylistId : 0);
  refreshPlaylistDirectoryList(preferredPlaylistId);
  refreshPlaylistList(playlistIndex);
  refreshUi();
}

void MusicPlayerScene::refreshLibraryList(int preferredIndex) {
  applyLibraryFilter(preferredIndex);
}

void MusicPlayerScene::applyLibraryFilter(int preferredIndex) {
  if (libraryList == nullptr) {
    return;
  }

  std::string preferredTrackId;
  if (preferredIndex >= 0 &&
      preferredIndex < static_cast<int>(filteredLibraryTracks.size())) {
    preferredTrackId =
        filteredLibraryTracks[static_cast<std::size_t>(preferredIndex)].trackId;
  } else if (preferredIndex >= 0 &&
             preferredIndex < static_cast<int>(libraryTracks.size())) {
    preferredTrackId =
        libraryTracks[static_cast<std::size_t>(preferredIndex)].trackId;
  }

  filteredLibraryTracks.clear();
  const std::string query = lowercaseText(trimPlaylistName(librarySearchText));
  std::copy_if(libraryTracks.begin(), libraryTracks.end(),
               std::back_inserter(filteredLibraryTracks),
               [&query](const MusicTrack &track) {
                 return trackMatchesSearch(track, query);
               });

  selectedLibraryIndex = -1;
  if (!preferredTrackId.empty()) {
    for (std::size_t i = 0; i < filteredLibraryTracks.size(); ++i) {
      if (filteredLibraryTracks[i].trackId == preferredTrackId) {
        selectedLibraryIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (selectedLibraryIndex < 0 && !filteredLibraryTracks.empty()) {
    selectedLibraryIndex = 0;
  }

  libraryList->setItems(filteredLibraryTracks);
  libraryList->selectedIndex = selectedLibraryIndex;
  libraryList->rebindVisibleItems();
}

void MusicPlayerScene::rebuildPlaylistChoices() {
  playlistChoices.clear();
  playlistChoices.reserve(playlists.size() + 1);
  playlistChoices.push_back(nowPlayingPlaylistInfo());
  playlistChoices.insert(playlistChoices.end(), playlists.begin(),
                         playlists.end());
}

void MusicPlayerScene::refreshLibraryPlaylistList(int preferredPlaylistId) {
  if (libraryPlaylistList == nullptr) {
    return;
  }
  rebuildPlaylistChoices();
  libraryPlaylistList->setItems(playlistChoices);

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
  libraryPlaylistList->selectedIndex = selectedLibraryPlaylistIndex;
  libraryPlaylistList->rebindVisibleItems();
}

void MusicPlayerScene::refreshPlaylistDirectoryList(int preferredPlaylistId) {
  if (playlistDirectoryList == nullptr) {
    return;
  }
  rebuildPlaylistChoices();
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
  if (railSummaryText != nullptr) {
    railSummaryText->setText(std::to_string(libraryTracks.size()) +
                             " library tracks\n" +
                             std::to_string(playlists.size()) +
                             " playlists");
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
  styleNav(playlistsNavButton, playlistsNavText, MusicPlayerTab::Playlists);
  styleNav(playerNavButton, playerNavText, MusicPlayerTab::Player);

  if (libraryPage != nullptr) {
    libraryPage->setVisible(activeTab == MusicPlayerTab::Library);
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
  const std::filesystem::path path =
      track ? artworkPathForDisplay(*track) : std::filesystem::path{};
  if (path == displayedLibraryArtworkPath) {
    return;
  }
  displayedLibraryArtworkPath = path;
  if (libraryArtworkImage == nullptr || libraryArtworkFallbackText == nullptr) {
    return;
  }
  if (path.empty()) {
    libraryArtworkImage->freeImage();
    libraryArtworkFallbackText->setVisible(true);
    return;
  }
  libraryArtworkImage->setImageAsync(fspath_to_path_t(path), true);
  libraryArtworkFallbackText->setVisible(true);
}

void MusicPlayerScene::setStatus(std::string message) {
  statusMessage = std::move(message);
  if (statusText != nullptr) {
    statusText->setText(statusMessage);
  }
}

std::optional<MusicPlayerScene::MusicTrack>
MusicPlayerScene::selectedLibraryTrack() const {
  if (selectedLibraryIndex < 0 ||
      selectedLibraryIndex >= static_cast<int>(filteredLibraryTracks.size())) {
    return std::nullopt;
  }
  return filteredLibraryTracks[static_cast<std::size_t>(selectedLibraryIndex)];
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

void MusicPlayerScene::switchTab(MusicPlayerTab tab) {
  activeTab = tab;
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

  if (libraryPlaylistList != nullptr) {
    libraryPlaylistList->selectedIndex = selectedLibraryPlaylistIndex;
    if (previousIndex >= 0 && previousIndex != selectedLibraryPlaylistIndex) {
      if (auto *previousView =
              libraryPlaylistList->getViewByIndex(previousIndex)) {
        previousView->onUnselected();
      }
    }
    if (selectedLibraryPlaylistIndex >= 0) {
      if (auto *selectedView =
              libraryPlaylistList->getViewByIndex(selectedLibraryPlaylistIndex)) {
        selectedView->onSelected();
      }
    }
  }
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
  } else {
    setStatus(errorMessage);
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
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus("Select a library track first.");
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
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus("Select a library track first.");
    return;
  }
  playNowPlaying({*track}, 0, "Select a library track first.",
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
  std::vector<MusicTrack> tracks =
      trimPlaylistName(librarySearchText).empty() ? libraryTracks
                                                  : filteredLibraryTracks;
  playNowPlaying(music_playlist::ShuffledTracks(std::move(tracks)), 0,
                 "No library tracks available.", "Playing Now Playing.");
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
  } else if (!status.empty()) {
    setStatus(status);
  }
}

bool MusicPlayerScene::handleSeekEvents(SDL_Event &event) {
  if (seekProgressTrack == nullptr || !seekProgressTrack->getVisible()) {
    return false;
  }

  const auto seekAt = [this](float uiX) {
    const int width = std::max(1, seekProgressTrack->getWidth());
    const float fraction =
        (uiX - static_cast<float>(seekProgressTrack->getX())) /
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
    if (!isInsideView(seekProgressTrack, uiX, uiY)) {
      return false;
    }
    seekMouseDown = true;
    seekAt(static_cast<float>(uiX));
    return true;
  }
  case SDL_MOUSEMOTION: {
    if (!seekMouseDown || event.motion.which == SDL_TOUCH_MOUSEID) {
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
    if (!seekMouseDown || event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return false;
    }
    seekMouseDown = false;
    int uiX = 0;
    int uiY = 0;
    mouseButtonEventToUi(event.button, uiX, uiY);
    (void)uiY;
    seekAt(static_cast<float>(uiX));
    return true;
  }
  case SDL_FINGERDOWN: {
    if (activeSeekTouchId != -1) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    if (!isInsideView(seekProgressTrack, uiX, uiY)) {
      return false;
    }
    activeSeekTouchId = event.tfinger.fingerId;
    seekAt(uiX);
    return true;
  }
  case SDL_FINGERMOTION: {
    if (event.tfinger.fingerId != activeSeekTouchId) {
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
    if (event.tfinger.fingerId != activeSeekTouchId) {
      return false;
    }
    activeSeekTouchId = -1;
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
  std::string status;
  if (context.musicPlayer.Stop(status)) {
    setStatus("Stopped.");
  } else {
    setStatus(status);
  }
}

void MusicPlayerScene::goBack() {
  if (context.sceneManager != nullptr) {
    context.sceneManager->changeScene("MainMenu", false);
  }
}
