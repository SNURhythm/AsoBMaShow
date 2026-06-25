#include "MusicPlayerScene.h"

#include "../path.h"
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
#include <sstream>
#include <utility>

namespace {

constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr float kScreenPadding = 18.0f;
constexpr float kHeaderHeight = 82.0f;
constexpr float kRailWidth = 180.0f;
constexpr int kTrackRowHeight = 82;
constexpr int kPlaylistRowHeight = 58;

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

class MusicTrackRowView : public View {
public:
  MusicTrackRowView() {
    setHeight(kTrackRowHeight)
        ->setPadding(Edge::All, 12)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(4)
        ->setCornerRadius(ui_theme::controlRadius())
        ->setBorderWidth(1);

    title = new TextView(kFontPath, 19);
    title->setHeight(26);
    title->setThemedColor(ui_theme::textPrimary);
    title->setOverflow(TextView::TextOverflow::Hidden);
    addView(title);

    detail = new TextView(kFontPath, 15);
    detail->setHeight(22);
    detail->setThemedColor(ui_theme::textSecondary);
    detail->setOverflow(TextView::TextOverflow::Hidden);
    addView(detail);
  }

  void setTrack(const music_playlist::MusicTrack &track, bool selected) {
    title->setText(trackTitle(track));
    detail->setText(trackDetail(track));
    if (selected) {
      setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
      setThemedBorderColor(ui_theme::accentBorderStrong);
    } else {
      setThemedBackgroundColor(ui_theme::mainMenuItem);
      setThemedBorderColor(ui_theme::hairlineSubtle);
    }
  }

private:
  TextView *title = nullptr;
  TextView *detail = nullptr;
};

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
      setThemedBackgroundColor(ui_theme::mainMenuItemSelected);
      setThemedBorderColor(ui_theme::accentBorderStrong);
    } else {
      setThemedBackgroundColor(ui_theme::mainMenuItem);
      setThemedBorderColor(ui_theme::hairlineSubtle);
    }
  }

private:
  TextView *title = nullptr;
  TextView *detail = nullptr;
};

} // namespace

void MusicPlayerScene::init() {
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  buildView();
  reloadData(false);
}

EventHandleResult MusicPlayerScene::handleEvents(SDL_Event &event) {
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
  if (context.musicPlayer.ProcessNativeControlEvents(nativeStatus) &&
      !nativeStatus.empty()) {
    setStatus(nativeStatus);
  }
  if (context.musicPlayer.ConsumeNativeControlStatus(nativeStatus) &&
      !nativeStatus.empty()) {
    setStatus(nativeStatus);
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
  playlistSelectionTitleText = nullptr;
  playlistSelectionDetailText = nullptr;
  currentTitleText = nullptr;
  currentDetailText = nullptr;
  playbackText = nullptr;
  artworkFallbackText = nullptr;
  artworkImage = nullptr;
  libraryList = nullptr;
  playlistDirectoryList = nullptr;
  playlistList = nullptr;
  playerQueueList = nullptr;
  playlistNameInput = nullptr;
  playPauseButtonText = nullptr;
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
    selectedLibraryIndex = index;
    refreshUi();
  };
  workspace->addView(libraryList);

  auto *actions = new View();
  actions->setWidth(350)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

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
  librarySelectionDetailText->setHeight(78);
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
  auto *addButton = makeButton("Add", 17, &addText);
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

  TextView *openPlayerText = nullptr;
  auto *openPlayerButton = makeButton("Open Player", 17, &openPlayerText);
  styleButton(openPlayerButton, openPlayerText, ui_theme::infoAction,
              ui_theme::infoActionHover, ui_theme::infoActionPressed,
              ui_theme::accentBorder);
  openPlayerButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Player); });
  actions->addView(openPlayerButton);

  auto *actionsSpacer = new View();
  actionsSpacer->setFlex(1);
  actions->addView(actionsSpacer);
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
    selectPlaylist(index);
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
  TextView *playerText = nullptr;
  auto *playerButton = makeButton("Player", 17, &playerText);
  playerButton->setFlex(1);
  styleButton(playerButton, playerText, ui_theme::infoAction,
              ui_theme::infoActionHover, ui_theme::infoActionPressed,
              ui_theme::accentBorder);
  playerButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Player); });
  playlistRowA->addView(playPlaylistButton);
  playlistRowA->addView(removeButton);
  playlistRowA->addView(playerButton);
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
  artworkImage = new ImageView(0, 0, 0, 0);
  artworkImage->setFlex(1);
  artworkFallbackText = new TextView(kFontPath, 18);
  artworkFallbackText->setText("No album art");
  artworkFallbackText->setHeight(36);
  artworkFallbackText->setAlign(TextView::CENTER);
  artworkFallbackText->setVAlign(TextView::MIDDLE);
  artworkFallbackText->setThemedColor(ui_theme::textMuted);
  artFrame->addView(artworkImage);
  artFrame->addView(artworkFallbackText);
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
  playSelectedButton->setOnClickListener(
      [this]() { playSelectedPlaylistTrack(); });

  transportRowA->addView(previousButton);
  transportRowA->addView(playPauseButton);
  transportRowA->addView(nextButton);
  transportRowB->addView(back10Button);
  transportRowB->addView(forward10Button);
  transportRowB->addView(stopButton);
  transportRowC->addView(playSelectedButton);
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

  auto *queueTitle = new TextView(kFontPath, 18);
  queueTitle->setText("Selected Playlist Queue");
  queueTitle->setHeight(28);
  queueTitle->setThemedColor(ui_theme::textSecondary);
  queueColumn->addView(queueTitle);

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
    selectPlaylistTrack(index);
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
  refreshLibraryList(libraryIndex);
  refreshPlaylistDirectoryList(preferredPlaylistId);
  refreshPlaylistList(playlistIndex);
  refreshUi();
}

void MusicPlayerScene::refreshLibraryList(int preferredIndex) {
  if (libraryList == nullptr) {
    return;
  }
  libraryList->setItems(libraryTracks);
  const int trackCount = static_cast<int>(libraryTracks.size());
  selectedLibraryIndex =
      preferredIndex >= 0 && preferredIndex < trackCount
          ? preferredIndex
          : (libraryTracks.empty() ? -1 : 0);
  libraryList->selectedIndex = selectedLibraryIndex;
  libraryList->rebindVisibleItems();
}

void MusicPlayerScene::refreshPlaylistDirectoryList(int preferredPlaylistId) {
  if (playlistDirectoryList == nullptr) {
    return;
  }
  playlistDirectoryList->setItems(playlists);
  int playlistIndex = -1;
  const int targetPlaylistId =
      preferredPlaylistId > 0 ? preferredPlaylistId : selectedPlaylistId;
  for (std::size_t i = 0; i < playlists.size(); ++i) {
    if (playlists[i].id == targetPlaylistId) {
      playlistIndex = static_cast<int>(i);
      break;
    }
  }
  if (playlistIndex < 0 && !playlists.empty()) {
    playlistIndex = 0;
    selectedPlaylistId = playlists.front().id;
  }
  selectedPlaylistDirectoryIndex = playlistIndex;
  playlistDirectoryList->selectedIndex = selectedPlaylistDirectoryIndex;
  playlistDirectoryList->rebindVisibleItems();
  if (playlistNameInput != nullptr &&
      trimPlaylistName(playlistNameInput->getText()).empty()) {
    playlistNameInput->setEditingText(nextPlaylistName());
  }
}

void MusicPlayerScene::refreshPlaylistList(int preferredIndex) {
  const int trackCount = static_cast<int>(playlistTracks.size());
  selectedPlaylistIndex =
      preferredIndex >= 0 && preferredIndex < trackCount
          ? preferredIndex
          : (playlistTracks.empty() ? -1 : 0);
  if (playlistList != nullptr) {
    playlistList->setItems(playlistTracks);
  }
  if (playerQueueList != nullptr) {
    playerQueueList->setItems(playlistTracks);
  }
  refreshPlaylistSelectionViews();
}

void MusicPlayerScene::refreshPlaylistSelectionViews() {
  if (playlistList != nullptr) {
    playlistList->selectedIndex = selectedPlaylistIndex;
    playlistList->rebindVisibleItems();
  }
  if (playerQueueList != nullptr) {
    playerQueueList->selectedIndex = selectedPlaylistIndex;
    playerQueueList->rebindVisibleItems();
  }
}

void MusicPlayerScene::refreshUi() {
  const auto playback = context.musicPlayer.PlaybackState();
  if (librarySubtitleText != nullptr) {
    librarySubtitleText->setText(std::to_string(libraryTracks.size()) +
                                 " music tracks");
  }
  if (playlistSubtitleText != nullptr) {
    playlistSubtitleText->setText(
        selectedPlaylistName() + " | " + std::to_string(playlists.size()) +
        " playlists | " + std::to_string(playlistTracks.size()) + " tracks");
  }
  if (playerSubtitleText != nullptr) {
    playerSubtitleText->setText(selectedPlaylistName() + " queue | " +
                                std::to_string(playlistTracks.size()) +
                                " tracks");
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
  if (playPauseButtonText != nullptr) {
    playPauseButtonText->setText(
        playback.playing ? "Pause" : (playback.loaded ? "Resume" : "Play"));
  }
  if (statusText != nullptr) {
    statusText->setText(statusMessage.empty()
                            ? "Ready."
                            : statusMessage);
  }
  refreshNavigation();
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
      track ? track->artworkPath : std::filesystem::path{};
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
  artworkFallbackText->setVisible(false);
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
      selectedLibraryIndex >= static_cast<int>(libraryTracks.size())) {
    return std::nullopt;
  }
  return libraryTracks[static_cast<std::size_t>(selectedLibraryIndex)];
}

std::optional<MusicPlayerScene::PlaylistInfo>
MusicPlayerScene::selectedPlaylistInfo() const {
  if (selectedPlaylistDirectoryIndex < 0 ||
      selectedPlaylistDirectoryIndex >= static_cast<int>(playlists.size())) {
    return std::nullopt;
  }
  return playlists[static_cast<std::size_t>(selectedPlaylistDirectoryIndex)];
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
  if (const auto playlistTrack = selectedPlaylistTrack()) {
    return playlistTrack;
  }
  return selectedLibraryTrack();
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
    refreshPlaylistList(-1);
    if (playlistNameInput != nullptr) {
      playlistNameInput->setEditingText(nextPlaylistName());
    }
    setStatus("Created " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::selectPlaylist(int index) {
  if (index < 0 || index >= static_cast<int>(playlists.size())) {
    setStatus("Select a playlist first.");
    return;
  }
  const int playlistId = playlists[static_cast<std::size_t>(index)].id;
  std::string errorMessage;
  if (context.musicPlayer.SelectPlaylist(playlistId, errorMessage)) {
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(-1);
    setStatus("Selected " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::selectPlaylistTrack(int index) {
  if (index < 0 || index >= static_cast<int>(playlistTracks.size())) {
    selectedPlaylistIndex = -1;
  } else {
    selectedPlaylistIndex = index;
  }
  refreshPlaylistSelectionViews();
  refreshUi();
}

void MusicPlayerScene::addLibraryTrackToPlaylist() {
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus("Select a library track first.");
    return;
  }
  std::string errorMessage;
  if (context.musicPlayer.AddChartToSelectedPlaylist(
          track->representativeChart, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(static_cast<int>(playlistTracks.size()) - 1);
    setStatus("Added to " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::removePlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus("Select a playlist track first.");
    return;
  }
  const int nextIndex = selectedPlaylistIndex;
  std::string errorMessage;
  if (context.musicPlayer.RemoveChartFromSelectedPlaylist(
          track->representativeChart, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
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
  std::string errorMessage;
  if (context.musicPlayer.MoveChartInSelectedPlaylist(
          track->representativeChart, delta, errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
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
  std::string errorMessage;
  if (context.musicPlayer.ClearSelectedPlaylist(errorMessage)) {
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks.clear();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(-1);
    setStatus("Cleared " + selectedPlaylistName() + ".");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::playLibraryTrack() {
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus("Select a library track first.");
    return;
  }
  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(libraryTracks,
                                  static_cast<std::size_t>(selectedLibraryIndex));
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, "Playing selected track.");
  setStatus(status);
}

void MusicPlayerScene::playPlaylist() {
  if (playlistTracks.empty()) {
    setStatus(selectedPlaylistName() + " is empty.");
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
  setStatus(status);
}

void MusicPlayerScene::playSelectedPlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus("Select a playlist track first.");
    return;
  }
  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(playlistTracks,
                                  static_cast<std::size_t>(selectedPlaylistIndex));
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, "Playing playlist track.");
  setStatus(status);
}

void MusicPlayerScene::playRandomLibrary() {
  context.jukebox.stop();
  std::string status;
  if (!context.musicPlayer.ReloadLibrary(status) ||
      !context.musicPlayer.StartRandomLibrary(status)) {
    setStatus(status);
    return;
  }
  libraryTracks = context.musicPlayer.LibraryTracksSnapshot();
  refreshLibraryList(selectedLibraryIndex);
  context.musicPlayer.PlayCurrentAsync(status, "Playing random library.");
  setStatus(status);
}

void MusicPlayerScene::togglePlayback() {
  std::string status;
  const auto playback = context.musicPlayer.PlaybackState();
  if (playback.playing) {
    context.musicPlayer.Pause(status);
  } else if (playback.loaded) {
    context.musicPlayer.Resume(status);
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

void MusicPlayerScene::playNext() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayNextAsync(status, "Playing next track.");
  setStatus(status);
}

void MusicPlayerScene::playPrevious() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayPreviousAsync(status, "Playing previous track.");
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
