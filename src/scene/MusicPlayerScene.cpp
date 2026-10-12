#include "../rendering/ResponsiveUi.h"
#include "../i18n/Localization.h"
#include "MusicPlayerScene.h"

#include "../audio/NativeMusicPlayer.h"
#include "../PlayOptionUtils.h"
#include "../path.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/CheckboxButtonContent.h"
#include "../view/DropdownView.h"
#include "../view/IconText.h"
#include "../view/ImageView.h"
#include "../view/OverlayPortal.h"
#include "../view/SnappedSlider.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"
#include "../view/MainMenuTheme.h"
#include "SceneManager.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <sstream>
#include <utility>

namespace {

constexpr const char *kFontPath = app_theme::font;
constexpr float kScreenPadding = 18.0f;
constexpr float kRailWidth = 350.0f;
constexpr int kTrackRowHeight = 68;
constexpr int kPlaylistRowHeight = 58;
constexpr int kNowPlayingPlaylistId = -1;
constexpr long long kRelativeSeekEndGuardMicros = 250000LL;
constexpr uint32_t kIconBackwardStep = 0xf048;
constexpr uint32_t kIconForwardStep = 0xf051;
constexpr uint32_t kIconPlay = 0xf04b;
constexpr uint32_t kIconPause = 0xf04c;
constexpr uint32_t kIconStop = 0xf04d;
constexpr uint32_t kIconBan = 0xf05e;
constexpr uint32_t kIconShuffle = 0xf074;
constexpr uint32_t kIconList = 0xf03a;
constexpr uint32_t kIconRotateLeft = 0xf2ea;
constexpr uint32_t kIconRotateRight = 0xf2f9;
constexpr uint32_t kIconRepeat = 0xf363;
constexpr uint32_t kIconVideo = 0xf03d;
constexpr uint32_t kIconXmark = 0xf00d;
constexpr uint32_t kIconOne = 0x31;

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
  insets.bottom =
      static_cast<int>(normalized.bottom * rendering::window_height);
  insets.right = static_cast<int>(normalized.right * rendering::window_width);
#endif
  return insets;
}

std::string trackTitle(const music_playlist::MusicTrack &track) {
  std::string title = track.title.empty() ? i18n::tr("music_player.untitled.label") : track.title;
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
  return i18n::tr("music_player.unknown_artist.label");
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

std::string playbackModeId(audio::PlaybackMode mode) {
  return mode == audio::PlaybackMode::TimeStretch ? "time-stretch"
                                                  : "pitch-shift";
}

std::string playbackModeLabel(audio::PlaybackMode mode) {
  return mode == audio::PlaybackMode::TimeStretch ? i18n::tr("music_player.time_stretch.label")
                                                  : i18n::tr("music_player.pitch_shift.label");
}

std::string formatSleepTimerDuration(long long micros) {
  micros = std::max(0LL, micros);
  long long totalSeconds = (micros + 999999LL) / 1000000LL;
  const long long hours = totalSeconds / 3600LL;
  totalSeconds %= 3600LL;
  const long long minutes = totalSeconds / 60LL;
  const long long seconds = totalSeconds % 60LL;

  std::ostringstream stream;
  if (hours > 0) {
    stream << hours << 'h';
    if (minutes > 0) {
      stream << ' ' << minutes << 'm';
    }
    return stream.str();
  }
  if (minutes > 0) {
    stream << minutes << 'm';
    if (seconds > 0 && minutes < 5) {
      stream << ' ' << seconds << 's';
    }
    return stream.str();
  }
  stream << std::max(1LL, seconds) << 's';
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

std::optional<long long>
parseSleepTimerDurationMicros(const std::string &rawValue,
                              std::string &errorMessage) {
  errorMessage.clear();
  const std::string text = lowercaseText(trimPlaylistName(rawValue));
  if (text.empty()) {
    errorMessage = i18n::tr("music_player.enter_sleep_timer_duration.message");
    return std::nullopt;
  }

  double totalSeconds = 0.0;
  bool consumed = false;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() &&
           std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    if (text[i] == ',') {
      ++i;
      continue;
    }

    const char *start = text.c_str() + i;
    char *end = nullptr;
    const double value = std::strtod(start, &end);
    if (end == start || !std::isfinite(value) || value < 0.0) {
      errorMessage = i18n::tr("music_player.invalid_sleep_timer_duration.message");
      return std::nullopt;
    }
    i += static_cast<std::size_t>(end - start);
    while (i < text.size() &&
           std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }

    const std::size_t unitStart = i;
    while (i < text.size() &&
           std::isalpha(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    const std::string unit = text.substr(unitStart, i - unitStart);

    double multiplier = 60.0;
    if (unit.empty() || unit == "m" || unit == "min" || unit == "mins" ||
        unit == "minute" || unit == "minutes") {
      multiplier = 60.0;
    } else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" ||
               unit == "hours") {
      multiplier = 3600.0;
    } else if (unit == "s" || unit == "sec" || unit == "secs" ||
               unit == "second" || unit == "seconds") {
      multiplier = 1.0;
    } else {
      errorMessage = i18n::tr("music_player.invalid_sleep_timer_unit.message");
      return std::nullopt;
    }
    totalSeconds += value * multiplier;
    consumed = true;
  }

  if (!consumed || totalSeconds <= 0.0) {
    errorMessage = i18n::tr("music_player.sleep_timer_duration_must_positive.message");
    return std::nullopt;
  }
  if (totalSeconds > 24.0 * 3600.0) {
    errorMessage = i18n::tr("music_player.sleep_timer_must_24_hours_less.message");
    return std::nullopt;
  }
  return static_cast<long long>(std::llround(totalSeconds * 1000000.0));
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
      meta.Artist + " " + meta.SubArtist + " " + meta.Genre + " " + meta.MD5 +
      " " + meta.SHA256 + " " + fspath_to_utf8(meta.BmsPath) + " " +
      fspath_to_utf8(meta.Folder);
  return containsText(searchable, query);
}

bool sameTrackList(const std::vector<music_playlist::MusicTrack> &a,
                   const std::vector<music_playlist::MusicTrack> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  return std::equal(a.begin(), a.end(), b.begin(),
                    music_playlist::SameTrackIdentity);
}

std::string favoriteKeyForTrack(const music_playlist::MusicTrack &track) {
  return music_playlist::ChartTrackIdForChart(track.representativeChart);
}

std::string repeatModeLabel(music_playlist::QueueRepeatMode mode) {
  switch (mode) {
  case music_playlist::QueueRepeatMode::One:
    return i18n::tr("music_player.repeat.single_track.label");
  case music_playlist::QueueRepeatMode::All:
    return i18n::tr("music_player.playlist_loop.label");
  case music_playlist::QueueRepeatMode::None:
  default:
    return i18n::tr("music_player.loop_off.label");
  }
}

uint32_t repeatModeStateIcon(music_playlist::QueueRepeatMode mode) {
  switch (mode) {
  case music_playlist::QueueRepeatMode::One:
    return kIconOne;
  case music_playlist::QueueRepeatMode::All:
    return kIconList;
  case music_playlist::QueueRepeatMode::None:
  default:
    return kIconBan;
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
  int index =
      previousNextIndex ? static_cast<int>(*previousNextIndex) : fallbackIndex;
  if (removedIndex && previousNextIndex && *removedIndex < index) {
    --index;
  }
  return std::clamp(index, 0, static_cast<int>(nextSize));
}

std::filesystem::path
artworkPathForDisplay(const music_playlist::MusicTrack &track) {
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
        ->setPadding(Edge::All, 8)
        ->setFlexDirection(FlexDirection::Row)
        ->setAlignItems(YGAlignCenter)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(12)
        ->setCornerRadius(app_theme::controlRadius())
        ->setBorderWidth(0);

    auto *rule = new View();
    rule->setPositionType(YGPositionTypeAbsolute)
        ->setPosition(Edge::Left, 0)->setPosition(Edge::Right, 0)
        ->setPosition(Edge::Bottom, 0)->setHeight(1)
        ->setThemedBackgroundColor(main_menu_theme::ruleSubtle);
    addView(rule);

    auto *artworkFrame = new View();
    artworkFrame->setWidth(48)
        ->setHeight(48)
        ->setFlexShrink(0)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setThemedBackgroundColor(app_theme::insetSurface)
        ->setThemedBorderColor(app_theme::hairlineSubtle)
        ->setBorderWidth(1)
        ->setCornerRadius(app_theme::controlRadius());

    artworkFallback = new TextView(kFontPath, 13);
    artworkFallback->setText("ART");
    artworkFallback->setHeight(22);
    artworkFallback->setAlign(TextView::CENTER);
    artworkFallback->setVAlign(TextView::MIDDLE);
    artworkFallback->setThemedColor(app_theme::textMuted);
    artworkFrame->addView(artworkFallback);

    artwork = new ImageView(0, 0, 0, 0);
    artwork->setWidth(48)
        ->setHeight(48)
        ->setPositionType(YGPositionTypeAbsolute)
        ->setPosition(Edge::Left, 0)
        ->setPosition(Edge::Top, 0)
        ->setCornerRadius(app_theme::controlRadius());
    artworkFrame->addView(artwork);
    addView(artworkFrame);

    auto *textColumn = new View();
    textColumn->setFlex(1)
        ->setMinWidth(0)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(4);

    title = new TextView(app_theme::boldFont, 19);
    title->setHeight(26);
    title->setThemedColor(app_theme::textPrimary);
    title->setOverflow(TextView::TextOverflow::Hidden);
    textColumn->addView(title);

    detail = new TextView(kFontPath, 15);
    detail->setHeight(22);
    detail->setThemedColor(app_theme::textSecondary);
    detail->setOverflow(TextView::TextOverflow::Marquee);
    textColumn->addView(detail);
    addView(textColumn);

    favoriteButton = new Button();
    favoriteButton->setWidth(48)
        ->setHeight(48)
        ->setFlexShrink(0)
        ->setCornerRadius(app_theme::controlRadius());
    favoriteButton
        ->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed)
        ->setThemedBorderColors(app_theme::hairlineSubtle,
                                app_theme::hairlineStrong,
                                app_theme::accentBorderStrong)
        ->setStyledBorderWidth(1);
    favoriteText = new TextView(kFontPath, 25);
    favoriteText->setText("☆");
    favoriteText->setAlign(TextView::CENTER);
    favoriteText->setVAlign(TextView::MIDDLE);
    favoriteText->setThemedColor(app_theme::textSecondary);
    favoriteButton->setContentView(favoriteText);
    addView(favoriteButton);
  }

  void setTrack(const music_playlist::MusicTrack &track, bool selected,
                bool favorite, std::function<void()> onFavoriteToggle) {
    title->setText(trackTitle(track));
    detail->setText(trackDetail(track));
    isFavorite_ = favorite;
    favoriteText->setText(favorite ? "★" : "☆");
    favoriteText->setThemedColor(favorite ? app_theme::amber
                                          : app_theme::textSecondary);
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
    setThemedBackgroundColor(main_menu_theme::selected);
    title->setThemedColor(main_menu_theme::onSelected);
    detail->setThemedColor(main_menu_theme::selectedSecondary);
    favoriteText->setThemedColor(isFavorite_ ? main_menu_theme::selectedAccent
                                           : main_menu_theme::onSelected);
    artworkFallback->setThemedColor(main_menu_theme::selectedSecondary);
  }

  void onUnselected() override {
    setThemedBackgroundColor(main_menu_theme::control);
    title->setThemedColor(main_menu_theme::ink);
    detail->setThemedColor(main_menu_theme::secondary);
    favoriteText->setThemedColor(isFavorite_ ? app_theme::amber
                                           : main_menu_theme::secondary);
    artworkFallback->setThemedColor(main_menu_theme::muted);
  }

private:
  bool isFavorite_ = false;
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
    setCornerRadius(std::max(0.0f, app_theme::controlRadius() - 1.0f));
  }

  void setFraction(float value) { fraction = std::clamp(value, 0.0f, 1.0f); }

protected:
  [[nodiscard]] bool requiresUiBatchBoundary() const noexcept override {
    return true;
  }

  void renderImpl(RenderContext &context) override {
    const float fillWidth =
        std::clamp(static_cast<float>(getWidth()) * fraction, 0.0f,
                   static_cast<float>(getWidth()));
    if (fillWidth <= 0.0f || getHeight() <= 0) {
      return;
    }

    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin(context.getTransformMatrix());
    const float radius =
        std::min(getCornerRadius(),
                 std::min(fillWidth, static_cast<float>(getHeight())) * 0.5f);
    batch.addRoundedRect(static_cast<float>(getX()), static_cast<float>(getY()),
                         fillWidth, static_cast<float>(getHeight()), radius,
                         app_theme::primaryAction().toABGR());
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
        ->setCornerRadius(app_theme::controlRadius())
        ->setBorderWidth(1);

    title = new TextView(kFontPath, 18);
    title->setHeight(24);
    title->setThemedColor(app_theme::textPrimary);
    title->setOverflow(TextView::TextOverflow::Hidden);
    addView(title);

    detail = new TextView(kFontPath, 14);
    detail->setHeight(20);
    detail->setThemedColor(app_theme::textSecondary);
    detail->setOverflow(TextView::TextOverflow::Hidden);
    addView(detail);
  }

  void setPlaylist(const MusicPlaylistInfo &playlist, bool selected) {
    title->setText(playlist.name.empty() ? i18n::tr("music_player.playlist.untitled_playlist.label") : playlist.name);
    detail->setText(i18n::format("music_player.playlist.track_count",
                                 {{"count", std::to_string(playlist.trackCount)}}));
    if (selected) {
      onSelected();
    } else {
      onUnselected();
    }
  }

  void onSelected() override {
    setThemedBackgroundColor(main_menu_theme::selected);
    title->setThemedColor(main_menu_theme::onSelected);
    detail->setThemedColor(main_menu_theme::selectedSecondary);
  }

  void onUnselected() override {
    setThemedBackgroundColor(main_menu_theme::control);
    title->setThemedColor(main_menu_theme::ink);
    detail->setThemedColor(main_menu_theme::secondary);
  }

private:
  TextView *title = nullptr;
  TextView *detail = nullptr;
};

} // namespace

MusicPlayerScene::~MusicPlayerScene() {
  // Unused or already-exited players must not reset another owner's BGA state.
  if (videoFullscreenActive || videoVisualsLoaded || videoRestoresVisualsEnabled) {
    cleanup();
  }
}

void MusicPlayerScene::configureTab(float topInset,
                                    std::function<void()> onClose, Scene *) {
  embeddedTab = true;
  tabClose = std::move(onClose);
  setTabTopInset(topInset);
}

void MusicPlayerScene::setTabTopInset(float topInset) {
  const float inset = std::max(0.0F, topInset);
  if (tabTopInset == inset) {
    return;
  }
  tabTopInset = inset;
  updateLayoutInsets();
}

void MusicPlayerScene::onPause() {
  exitVideoFullscreen();
  onInputQueueOverflow();
  for (auto *input : {librarySearchInput, favoritesSearchInput,
                      playlistNameInput, playlistRenameInput, sleepTimerInput}) {
    if (input != nullptr) input->endEditing();
  }
  playbackModeDropdownOpen = false;
  refreshPlaybackRateControl();
}

void MusicPlayerScene::onResume() {
  const auto &profileId = context.profileManager.activeProfile().id;
  const bool sameProfile = displayedProfileId == profileId;
  applySystemPlaybackPrivacy(false);
  if (!sameProfile) {
    pendingDeletePlaylistId = 0;
    pendingClearPlaylistId = 0;
    selectedLibraryPlaylistId = 0;
    statusMessage.clear();
  }
  // PLAY can change charts and favorites without changing the active profile.
  reloadData(sameProfile);
  displayedProfileId = profileId;
  refreshLanguageIfNeeded();
  updateLayoutInsets();
}

void MusicPlayerScene::onLanguageChanged() {
  Scene::onLanguageChanged();
  rebindFavoriteAwareTrackLists();
  for (auto *list : {libraryPlaylistList, favoritesPlaylistList,
                     playlistDirectoryList}) {
    if (list != nullptr) list->rebindVisibleItems();
  }
  refreshUi();
  refreshVideoOverlay();
}

void MusicPlayerScene::updateLayoutInsets() {
  if (rootLayout == nullptr) {
    return;
  }
  auto safe = getSafeAreaInsetsUi();
  if (embeddedTab) safe.top = 0;
  rootLayout->setSize(rendering::window_width, rendering::window_height);
  rootLayout->setPadding(Edge::Top, embeddedTab ? tabTopInset : safe.top);
  contentLayout->setPadding(Edge::Bottom, safe.bottom)
      ->setPadding(Edge::Left, safe.left)
      ->setPadding(Edge::Right, safe.right);
  const float width = rendering::window_width - safe.left - safe.right;
  const bool compactNavigation = width < 900;
  contentLayout->setFlexDirection(compactNavigation ? FlexDirection::Column
                                                    : FlexDirection::Row);
  if (auto *rail = rootLayout->findViewByName("musicNavigationRail")) {
    rail->setWidth(compactNavigation ? width
                                    : (width < 1100 ? width * 0.24F : kRailWidth));
    rail->setHeight(compactNavigation ? 76 : YGUndefined);
    rail->setFlexDirection(compactNavigation ? FlexDirection::Row
                                             : FlexDirection::Column);
    rail->setPadding(Edge::All, compactNavigation ? 8 : 24);
    for (auto *button : {libraryNavButton, favoritesNavButton,
                         playlistsNavButton, playerNavButton}) {
      button->setFlex(compactNavigation ? 1 : 0);
      button->setMinWidth(0);
    }
  }
  if (auto *spacer = rootLayout->findViewByName("musicNavigationSpacer")) {
    spacer->setDisplay(compactNavigation ? YGDisplayNone : YGDisplayFlex);
  }
  if (statusText != nullptr) {
    statusText->setDisplay(compactNavigation ? YGDisplayNone : YGDisplayFlex);
  }
  const bool phoneStack = rendering::compactMenuCanvasActive && width < 900;
  for (const auto *name : {"musicLibraryWorkspace", "musicFavoritesWorkspace"}) {
    if (auto *workspace = rootLayout->findViewByName(name)) {
      workspace->setFlexDirection(phoneStack ? FlexDirection::Column : FlexDirection::Row);
      workspace->setMinHeight(0);
    }
  }
  for (auto *list : {libraryList, favoritesList}) {
    if (list) list->setMinHeight(phoneStack ? 160 : 0);
  }
  for (const auto *name : {"musicLibraryDetail", "musicFavoritesDetail"}) {
    if (auto *detail = rootLayout->findViewByName(name)) {
      if (phoneStack) detail->setWidthPercent(100)->setHeight(260)->setFlexShrink(0);
      else detail->setWidth(width < 1100 ? 250 : 320)->setHeight(YGUndefined);

    }
  }
  for (const auto *name : {"musicLibraryArtwork", "musicFavoritesArtwork"}) {
    if (auto *art = rootLayout->findViewByName(name)) art->setDisplay(phoneStack ? YGDisplayNone : YGDisplayFlex);
  }
  const float availableHeight = rendering::window_height - safe.bottom -
      (embeddedTab ? tabTopInset : safe.top) -
      (compactNavigation ? 76 : 0) - 150;
  for (const auto *name : {"musicPlayerViewport", "musicPlaylistsViewport"}) {
    auto *viewport = dynamic_cast<ScrollView *>(rootLayout->findViewByName(name));
    if (viewport == nullptr) continue;
    auto *workspace = viewport->getContentView();
    const bool player = std::string_view(name) == "musicPlayerViewport";
    const bool stacked = width < (player ? 1250 : 1000);
    workspace->setFlexDirection(stacked ? FlexDirection::Column
                                        : FlexDirection::Row);
    workspace->setHeight(std::max(availableHeight,
                                  stacked ? (player ? 1420.0F : 1100.0F)
                                          : (player ? 780.0F : 600.0F)));
    auto *leading = workspace->findViewByName(
        player ? "musicPlayerControls" : "musicPlaylistDirectory");
    if (stacked) {
      leading->setWidthPercent(100)->setHeight(player ? 780 : 500);
    } else {
      leading->setHeight(YGUndefined);
      if (player) leading->setWidth(400);
      else leading->setWidthPercent(34);
    }
    viewport->refreshContentLayout();
  }
  rootLayout->applyYogaLayout();
  if (videoOverlayRoot != nullptr) {
    videoOverlayRoot->setPadding(Edge::Left, safe.left + 24)
        ->setPadding(Edge::Right, safe.right + 24)
        ->setPadding(Edge::Bottom, safe.bottom + 24);
  }
}

void MusicPlayerScene::init() {
  displayedProfileId = context.profileManager.activeProfile().id;
  context.jukebox.stop();
  applySystemPlaybackPrivacy(false);
  std::string clubModeStatus;
  context.musicPlayer.SetClubMode(context.settings.musicPlayerClubModeEnabled,
                                  clubModeStatus);
  std::string playbackRateError;
  context.musicPlayer.SetPlaybackRate(
      {.percent = context.settings.musicPlayerPlaybackRatePercent,
       .mode = context.settings.musicPlayerPlaybackMode},
      playbackRateError);
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
  if (event.type == SDL_EVENT_KEY_DOWN) {
    if (event.key.key == SDLK_ESCAPE) {
      goBack();
      return {};
    }
    if (event.key.key == SDLK_1) {
      switchTab(MusicPlayerTab::Library);
      return {};
    }
    if (event.key.key == SDLK_2) {
      switchTab(MusicPlayerTab::Favorites);
      return {};
    }
    if (event.key.key == SDLK_3) {
      switchTab(MusicPlayerTab::Playlists);
      return {};
    }
    if (event.key.key == SDLK_4) {
      switchTab(MusicPlayerTab::Player);
      return {};
    }
  }
  return Scene::handleEvents(event);
}

void MusicPlayerScene::update(float) {
  if (rootLayout != nullptr && (lastLayoutWidth != rendering::window_width ||
                                lastLayoutHeight != rendering::window_height)) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    updateLayoutInsets();
    if (overlayPortal != nullptr) {
      overlayPortal->setSize(rendering::window_width, rendering::window_height);
    }
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
    const bool appliedClubMode = context.musicPlayer.ClubMode();
    if (context.settings.musicPlayerClubModeEnabled != appliedClubMode) {
      context.settings.musicPlayerClubModeEnabled = appliedClubMode;
      if (!context.saveSettings()) {
        SDL_Log("Failed to save Music Player Club Beat setting");
      }
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
  contentLayout = nullptr;
  overlayPortal = nullptr;
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
  repeatModeBaseButtonText = nullptr;
  watchVideoButtonText = nullptr;
  sleepTimerSetButton = nullptr;
  sleepTimerClearButton = nullptr;
  sleepTimerInput = nullptr;
  sleepTimerSetText = nullptr;
  sleepTimerClearText = nullptr;
  sleepTimerStatusText = nullptr;
  systemPlaybackJacketButton = nullptr;
  systemPlaybackTitleButton = nullptr;
  systemPlaybackArtistButton = nullptr;
  systemPlaybackJacketText = nullptr;
  systemPlaybackTitleText = nullptr;
  systemPlaybackArtistText = nullptr;
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
  playbackModeDropdown = nullptr;
  playbackRateSlider = nullptr;
  playbackRateValueText = nullptr;
  clubModeButton = nullptr;
  clubModeButtonContent = nullptr;
  deletePlaylistButtonText = nullptr;
  clearPlaylistButtonText = nullptr;
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
  playbackModeDropdownOpen = false;
  videoControlsVisibleUntil = 0;
  videoTrackId.clear();
  videoChart.reset();
}

void MusicPlayerScene::buildView() {
  auto safe = getSafeAreaInsetsUi();
  if (embeddedTab) safe.top = 0;
  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setPadding(Edge::Top, embeddedTab ? tabTopInset : safe.top);
  rootLayout->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(rootLayout);

  overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                    rendering::window_height);
  overlayPortal->setPositionType(YGPositionTypeAbsolute);
  overlayPortal->setPosition(Edge::Left, 0);
  overlayPortal->setPosition(Edge::Top, 0);
  overlayPortal->setZIndex(900);

  statusText = new TextView(kFontPath, 17);
  statusText->setHeight(26);
  statusText->setThemedColor(app_theme::textSecondary);
  statusText->setOverflow(TextView::TextOverflow::Hidden);

  TextView *backText = nullptr;
  auto *backButton = makeButton(i18n::message("music_player.back.label"), 20, &backText);
  backButton->setWidth(118);
  backButton->setHeight(52);
  styleButton(backButton, backText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  backButton->setOnClickListener([this]() { goBack(); });
  backButton->setVisible(!embeddedTab);
  backButton->setDisplay(embeddedTab ? YGDisplayNone : YGDisplayFlex);

  auto *content = new View();
  contentLayout = content;
  content->setFlex(1)
      ->setMinHeight(0)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::Bottom, safe.bottom)
      ->setPadding(Edge::Left, safe.left)
      ->setPadding(Edge::Right, safe.right);

  auto *rail = new View();
  rail->setName("musicNavigationRail");
  rail->setWidth(kRailWidth)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(8)
      ->setPadding(Edge::All, 24);
  auto *railRule = new View();
  railRule->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Right, 0)->setPosition(Edge::Top, 0)
      ->setPosition(Edge::Bottom, 0)->setWidth(1)
      ->setThemedBackgroundColor(main_menu_theme::ruleStrong);
  rail->addView(railRule);

  libraryNavButton = makeNavButton(i18n::message("music_player.navigation.library.label"), &libraryNavText);
  libraryNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Library); });
  favoritesNavButton = makeNavButton(i18n::message("music_player.navigation.favorites.label"), &favoritesNavText);
  favoritesNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Favorites); });
  playlistsNavButton = makeNavButton(i18n::message("music_player.navigation.playlists.label"), &playlistsNavText);
  playlistsNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Playlists); });
  playerNavButton = makeNavButton(i18n::message("music_player.navigation.player.label"), &playerNavText);
  playerNavButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Player); });
  if (!embeddedTab) rail->addView(backButton);
  else delete backButton;
  rail->addView(libraryNavButton);
  rail->addView(favoritesNavButton);
  rail->addView(playlistsNavButton);
  rail->addView(playerNavButton);

  auto *railSpacer = new View();
  railSpacer->setName("musicNavigationSpacer");
  railSpacer->setFlex(1);
  rail->addView(railSpacer);
  statusText->setHeight(72);
  statusText->setWrap(true);
  rail->addView(statusText);
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
  rootLayout->addView(overlayPortal);
  refreshNavigation();
  rootLayout->applyYogaLayout();
  updateLayoutInsets();
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
  videoArtworkFallbackText->setLocalizedText(i18n::message("music_player.no_jacket_available.label"));
  videoArtworkFallbackText->setHeight(40);
  videoArtworkFallbackText->setAlign(TextView::CENTER);
  videoArtworkFallbackText->setVAlign(TextView::MIDDLE);
  videoArtworkFallbackText->setThemedColor(app_theme::textMuted);
  videoArtworkBackdrop->addView(videoArtworkFallbackText);

  videoArtworkImage = new ImageView(0, 0, 0, 0);
  videoArtworkImage->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setThemedBorderColor(app_theme::hairlineStrong)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::panelRadius());
  videoArtworkBackdrop->addView(videoArtworkImage);

  videoControlsPanel = new View();
  videoControlsPanel->setHeight(190)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setPadding(Edge::All, 14)
      ->setThemedBackgroundColor(
          [] { return app_theme::withAlpha(app_theme::panelStrong(), 232); })
      ->setThemedBorderColor(app_theme::hairlineStrong)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::panelRadius());

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
  videoTitleText->setThemedColor(app_theme::textPrimary);
  videoTitleText->setOverflow(TextView::TextOverflow::Marquee);
  titleColumn->addView(videoTitleText);

  videoDetailText = new TextView(kFontPath, 15);
  videoDetailText->setHeight(22);
  videoDetailText->setThemedColor(app_theme::textSecondary);
  videoDetailText->setOverflow(TextView::TextOverflow::Marquee);
  titleColumn->addView(videoDetailText);

  videoPlaybackText = new TextView(kFontPath, 17);
  videoPlaybackText->setWidth(132);
  videoPlaybackText->setHeight(52);
  videoPlaybackText->setAlign(TextView::RIGHT);
  videoPlaybackText->setVAlign(TextView::MIDDLE);
  videoPlaybackText->setThemedColor(app_theme::textSecondary);
  titleRow->addView(titleColumn);
  titleRow->addView(videoPlaybackText);
  videoControlsPanel->addView(titleRow);

  videoProgressTrack = new View();
  videoProgressTrack->setHeight(24)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(app_theme::insetSurface)
      ->setThemedBorderColor(app_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::controlRadius());
  videoProgressFill = new MusicSeekProgressFillView();
  videoProgressFill->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 1)
      ->setPosition(Edge::Right, 1)
      ->setPosition(Edge::Top, 1)
      ->setPosition(Edge::Bottom, 1);
  videoProgressTrack->addView(videoProgressFill);
  videoControlsPanel->addView(videoProgressTrack);

  auto *transportRow = new View();
  transportRow->setHeight(56)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(10);

  TextView *previousText = nullptr;
  auto *previousButton = makeIconButton(kIconBackwardStep, 25, &previousText);
  styleButton(previousButton, previousText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  previousButton->setOnClickListener([this]() {
    showVideoControls();
    playPrevious();
  });

  TextView *back10Text = nullptr;
  auto *back10Button = makeIconButton(kIconRotateLeft, 22, &back10Text);
  styleButton(back10Button, back10Text, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  back10Button->setOnClickListener([this]() {
    showVideoControls();
    seekRelative(-10000000LL);
  });

  auto *playPauseButton =
      makeIconButton(kIconPause, 31, &videoPlayPauseButtonText);
  playPauseButton->setWidth(92);
  styleButton(playPauseButton, videoPlayPauseButtonText, app_theme::infoAction,
              app_theme::infoActionHover, app_theme::infoActionPressed,
              app_theme::accentBorder);
  playPauseButton->setOnClickListener([this]() {
    showVideoControls();
    togglePlayback();
  });

  TextView *forward10Text = nullptr;
  auto *forward10Button = makeIconButton(kIconRotateRight, 22, &forward10Text);
  styleButton(forward10Button, forward10Text, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  forward10Button->setOnClickListener([this]() {
    showVideoControls();
    seekRelative(10000000LL);
  });

  TextView *nextText = nullptr;
  auto *nextButton = makeIconButton(kIconForwardStep, 25, &nextText);
  styleButton(nextButton, nextText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  nextButton->setOnClickListener([this]() {
    showVideoControls();
    playNext();
  });

  TextView *closeText = nullptr;
  auto *closeButton = makeIconButton(kIconXmark, 25, &closeText);
  styleButton(closeButton, closeText, app_theme::warningAction,
              app_theme::warningActionHover, app_theme::warningActionPressed,
              app_theme::accentBorder);
  closeButton->setOnClickListener([this]() { exitVideoFullscreen(); });

  transportRow->addView(previousButton);
  transportRow->addView(back10Button);
  transportRow->addView(playPauseButton);
  transportRow->addView(forward10Button);
  transportRow->addView(nextButton);
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
  auto *panel = makePanel(isLibrary ? i18n::message("music_player.page.library.label") : i18n::message("music_player.page.favorites.label"), subtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *searchRow = new View();
  searchRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10);
  auto *searchLabel = new TextView(kFontPath, 17);
  searchLabel->setLocalizedText(i18n::message("music_player.search.label"));
  searchLabel->setWidth(72);
  searchLabel->setHeight(52);
  searchLabel->setVAlign(TextView::MIDDLE);
  searchLabel->setThemedColor(app_theme::textSecondary);
  auto *searchInput = new TextInputBox(kFontPath, 18);
  if (isLibrary) {
    librarySearchInput = searchInput;
  } else {
    favoritesSearchInput = searchInput;
  }
  searchInput->setFlex(1);
  searchInput->setHeight(52);
  searchInput->setThemedBackgroundColor(app_theme::control);
  searchInput->setThemedBorderColor(app_theme::hairlineStrong);
  searchInput->setBorderWidth(1);
  searchInput->setCornerRadius(app_theme::controlRadius());
  searchInput->setThemedColor(app_theme::textPrimary);
  searchInput->setVAlign(TextView::MIDDLE);
  searchInput->setClearable(true);
  searchInput->onTextChanged([this, kind](const std::string &value) {
    trackBrowserSearchText(kind) = value;
    applyTrackBrowserFilter(kind);
    refreshUi();
  });
  searchRow->addView(searchLabel);
  searchRow->addView(searchInput);
  panel->addView(searchRow);

  auto *workspace = new View();
  workspace->setName(isLibrary ? "musicLibraryWorkspace" : "musicFavoritesWorkspace");
  workspace->setFlex(1)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  panel->addView(workspace);

  auto *trackList =
      new RecyclerView<MusicTrack>(music_playlist::SameTrackIdentity);
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

  auto *actionsScroll = new ScrollView();
  actionsScroll->setName(isLibrary ? "musicLibraryDetail" : "musicFavoritesDetail");
  actionsScroll->setWidth(320)->setFlexShrink(0)->setMinHeight(0);
  auto *actions = new View();
  actions->setWidthPercent(100)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(8)
      ->setPadding(Edge::Left, 16);
  auto *detailRule = new View();
  detailRule->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)
      ->setPosition(Edge::Bottom, 0)->setWidth(1)
      ->setThemedBackgroundColor(main_menu_theme::ruleStrong);
  actions->addView(detailRule);

  auto *libraryArtFrame = new View();
  libraryArtFrame->setWidth(210)->setHeight(210)
      ->setAlignSelf(YGAlignCenter)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(app_theme::insetSurface)
      ->setThemedBorderColor(app_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::controlRadius());
  auto *artworkFallbackText = new TextView(kFontPath, 18);
  if (isLibrary) {
    libraryArtworkFallbackText = artworkFallbackText;
  } else {
    favoritesArtworkFallbackText = artworkFallbackText;
  }
  artworkFallbackText->setLocalizedText(i18n::message("music_player.library.no_album_art.label"));
  artworkFallbackText->setHeight(36);
  artworkFallbackText->setAlign(TextView::CENTER);
  artworkFallbackText->setVAlign(TextView::MIDDLE);
  artworkFallbackText->setThemedColor(app_theme::textMuted);
  libraryArtFrame->addView(artworkFallbackText);
  auto *artworkImage = new ImageView(0, 0, 0, 0);
  if (isLibrary) {
    libraryArtworkImage = artworkImage;
  } else {
    favoritesArtworkImage = artworkImage;
  }
  artworkImage->setWidth(210)
      ->setHeight(210)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setCornerRadius(app_theme::controlRadius());
  libraryArtFrame->addView(artworkImage);
  libraryArtFrame->setName(isLibrary ? "musicLibraryArtwork" : "musicFavoritesArtwork");
  actions->addView(libraryArtFrame);

  auto *selectionTitleText = new TextView(app_theme::boldFont, 28);
  if (isLibrary) {
    librarySelectionTitleText = selectionTitleText;
  } else {
    favoritesSelectionTitleText = selectionTitleText;
  }
  selectionTitleText->setHeight(42);
  selectionTitleText->setOverflow(TextView::TextOverflow::Marquee);
  selectionTitleText->setThemedColor(app_theme::textPrimary);
  actions->addView(selectionTitleText);

  auto *selectionDetailText = new TextView(kFontPath, 16);
  if (isLibrary) {
    librarySelectionDetailText = selectionDetailText;
  } else {
    favoritesSelectionDetailText = selectionDetailText;
  }
  selectionDetailText->setHeight(32);
  selectionDetailText->setOverflow(TextView::TextOverflow::Marquee);
  selectionDetailText->setThemedColor(app_theme::textSecondary);
  actions->addView(selectionDetailText);

  auto *primaryRow = new View();
  primaryRow->setHeight(116)->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)->setGap(4);
  TextView *playTrackText = nullptr;
  auto *playTrackButton = makeButton(i18n::message("music_player.library.play.label"), 28, &playTrackText);
  playTrackButton->setHeight(64);
  styleButton(playTrackButton, playTrackText, app_theme::primaryAction,
              app_theme::primaryActionHover, app_theme::primaryActionPressed,
              app_theme::accentBorderStrong);
  playTrackButton->setOnClickListener(
      [this, kind]() { playTrackBrowserTrack(kind); });
  TextView *addText = nullptr;
  auto *addButton = makeButton(i18n::message("music_player.add.label"), 15, &addText);
  addButton->setHeight(48);
  styleButton(addButton, addText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  addButton->setOnClickListener(
      [this, kind]() { addTrackBrowserTrackToPlaylist(kind); });
  primaryRow->addView(playTrackButton);
  primaryRow->addView(addButton);
  actions->addView(primaryRow);

  auto *secondaryRow = new View();
  secondaryRow->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  TextView *randomText = nullptr;
  auto *randomButton = makeButton(i18n::message("music_player.shuffle.label"), 17, &randomText);
  randomButton->setFlex(1);
  styleButton(randomButton, randomText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  randomButton->setOnClickListener(
      [this, kind]() { playRandomTrackBrowser(kind); });
  TextView *reloadText = nullptr;
  auto *reloadButton = makeButton(i18n::message("music_player.refresh.label"), 17, &reloadText);
  reloadButton->setFlex(1);
  styleButton(reloadButton, reloadText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  reloadButton->setOnClickListener([this]() { reloadData(true); });
  secondaryRow->addView(randomButton);
  secondaryRow->addView(reloadButton);
  actions->addView(secondaryRow);

  if (isLibrary) {
    TextView *groupText = nullptr;
    auto *groupButton = makeButton(i18n::message("music_player.expand.label"), 17, &groupText);
    libraryGroupButtonText = groupText;
    groupButton->setHeight(48);
    styleButton(groupButton, groupText, app_theme::control,
                app_theme::controlHover, app_theme::controlPressed,
                app_theme::hairlineStrong);
    groupButton->setOnClickListener([this]() { toggleSelectedLibraryGroup(); });
    actions->addView(groupButton);
  }

  auto *addToHeader = new TextView(kFontPath, 18);
  addToHeader->setLocalizedText(i18n::message("music_player.page.playlists.label"));
  addToHeader->setHeight(28);
  addToHeader->setThemedColor(app_theme::textSecondary);
  actions->addView(addToHeader);

  auto *addToPlaylistList = new RecyclerView<PlaylistInfo>(
      [](const PlaylistInfo &a, const PlaylistInfo &b) {
        return a.id == b.id;
      });
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
  addToPlaylistList->onBind = [](View *view, const PlaylistInfo &playlist, int,
                                 bool selected) {
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
  addToPlaylistList->setMinHeight(120);
  actionsScroll->setContentView(actions);
  workspace->addView(actionsScroll);
}

void MusicPlayerScene::buildPlaylistsPage(View *page) {
  auto *panel = makePanel(i18n::message("music_player.playlist.playlists.label"), &playlistSubtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *viewport = new ScrollView();
  viewport->setName("musicPlaylistsViewport");
  viewport->setFlex(1)->setMinHeight(0);
  panel->addView(viewport);
  auto *workspace = new View();
  workspace->setHeight(780)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  viewport->setContentView(workspace);

  auto *directoryColumn = new View();
  directoryColumn->setName("musicPlaylistDirectory");
  directoryColumn->setWidthPercent(34)
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
  playlistNameInput->setThemedBackgroundColor(app_theme::control);
  playlistNameInput->setThemedBorderColor(app_theme::hairlineStrong);
  playlistNameInput->setBorderWidth(1);
  playlistNameInput->setCornerRadius(app_theme::controlRadius());
  playlistNameInput->setThemedColor(app_theme::textPrimary);
  playlistNameInput->setVAlign(TextView::MIDDLE);
  playlistNameInput->onSubmit(
      [this](const std::string &) { createPlaylist(); });
  TextView *createText = nullptr;
  auto *createButton = makeButton(i18n::message("music_player.playlist.create.label"), 17, &createText);
  createButton->setWidth(112);
  styleButton(createButton, createText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  createButton->setOnClickListener([this]() { createPlaylist(); });
  createPlaylistRow->addView(playlistNameInput);
  createPlaylistRow->addView(createButton);
  directoryColumn->addView(createPlaylistRow);

  TextView *saveNowPlayingText = nullptr;
  auto *saveNowPlayingButton =
      makeButton(i18n::message("music_player.playlist.save_queue.label"), 16, &saveNowPlayingText);
  saveNowPlayingButton->setHeight(52);
  styleButton(saveNowPlayingButton, saveNowPlayingText, app_theme::successAction,
              app_theme::successActionHover, app_theme::successActionPressed,
              app_theme::accentBorder);
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
  playlistRenameInput->setThemedBackgroundColor(app_theme::control);
  playlistRenameInput->setThemedBorderColor(app_theme::hairlineStrong);
  playlistRenameInput->setBorderWidth(1);
  playlistRenameInput->setCornerRadius(app_theme::controlRadius());
  playlistRenameInput->setThemedColor(app_theme::textPrimary);
  playlistRenameInput->setVAlign(TextView::MIDDLE);
  playlistRenameInput->onSubmit(
      [this](const std::string &) { renameSelectedPlaylist(); });
  TextView *renameText = nullptr;
  auto *renameButton = makeButton(i18n::message("music_player.playlist.rename.label"), 16, &renameText);
  renameButton->setWidth(112);
  styleButton(renameButton, renameText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  renameButton->setOnClickListener([this]() { renameSelectedPlaylist(); });
  renamePlaylistRow->addView(playlistRenameInput);
  renamePlaylistRow->addView(renameButton);
  directoryColumn->addView(renamePlaylistRow);

  auto *playlistManageRow = new View();
  playlistManageRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setGap(10);
  auto *deletePlaylistButton =
      makeButton(i18n::message("music_player.playlist.delete.label"), 15, &deletePlaylistButtonText);
  deletePlaylistButton->setFlex(1);
  styleButton(deletePlaylistButton, deletePlaylistButtonText,
              app_theme::warningAction, app_theme::warningActionHover,
              app_theme::warningActionPressed, app_theme::accentBorder);
  deletePlaylistButton->setOnClickListener(
      [this]() { deleteSelectedPlaylist(); });
  playlistManageRow->addView(deletePlaylistButton);
  directoryColumn->addView(playlistManageRow);

  playlistDirectoryList = new RecyclerView<PlaylistInfo>(
      [](const PlaylistInfo &a, const PlaylistInfo &b) {
        return a.id == b.id;
      });
  playlistDirectoryList->setFlex(1)->setMinHeight(174);
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
  playlistDirectoryList->onUnselected = [this](const PlaylistInfo &,
                                               int index) {
    if (auto *unselectedView = playlistDirectoryList->getViewByIndex(index)) {
      unselectedView->onUnselected();
    }
  };
  directoryColumn->addView(playlistDirectoryList);
  workspace->addView(directoryColumn);

  auto *editorColumn = new View();
  editorColumn->setName("musicPlaylistEditor");
  editorColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *selectedHeader = new TextView(kFontPath, 18);
  selectedHeader->setLocalizedText(i18n::message("music_player.playlist.tracks.label"));
  selectedHeader->setHeight(28);
  selectedHeader->setThemedColor(app_theme::textSecondary);
  editorColumn->addView(selectedHeader);

  playlistSelectionTitleText = new TextView(kFontPath, 24);
  playlistSelectionTitleText->setHeight(36);
  playlistSelectionTitleText->setOverflow(TextView::TextOverflow::Marquee);
  playlistSelectionTitleText->setThemedColor(app_theme::textPrimary);
  editorColumn->addView(playlistSelectionTitleText);

  playlistSelectionDetailText = new TextView(kFontPath, 16);
  playlistSelectionDetailText->setHeight(32);
  playlistSelectionDetailText->setOverflow(TextView::TextOverflow::Marquee);
  playlistSelectionDetailText->setThemedColor(app_theme::textSecondary);
  editorColumn->addView(playlistSelectionDetailText);

  playlistList =
      new RecyclerView<MusicTrack>(music_playlist::SameTrackIdentity);
  playlistList->setFlex(1)->setMinHeight(204);
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
  playlistRowA->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  auto *playlistRowB = new View();
  playlistRowB->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  TextView *playPlaylistText = nullptr;
  auto *playPlaylistButton = makeButton(i18n::message("music_player.playlist.play.label"), 17, &playPlaylistText);
  playPlaylistButton->setFlex(1);
  styleButton(playPlaylistButton, playPlaylistText, app_theme::primaryAction,
              app_theme::primaryActionHover, app_theme::primaryActionPressed,
              app_theme::accentBorderStrong);
  playPlaylistButton->setOnClickListener([this]() { playPlaylist(); });
  TextView *removeText = nullptr;
  auto *removeButton = makeButton(i18n::message("music_player.playlist.remove.label"), 16, &removeText);
  removeButton->setFlex(1);
  styleButton(removeButton, removeText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  removeButton->setOnClickListener([this]() { removePlaylistTrack(); });
  TextView *upText = nullptr;
  auto *upButton = makeButton(i18n::message("music_player.playlist.up.label"), 17, &upText);
  upButton->setFlex(1);
  styleButton(upButton, upText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  upButton->setOnClickListener([this]() { movePlaylistTrack(-1); });
  TextView *downText = nullptr;
  auto *downButton = makeButton(i18n::message("music_player.playlist.down.label"), 17, &downText);
  downButton->setFlex(1);
  styleButton(downButton, downText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  downButton->setOnClickListener([this]() { movePlaylistTrack(1); });
  auto *clearButton = makeButton(i18n::message("music_player.playlist.clear.label"), 17, &clearPlaylistButtonText);
  clearButton->setFlex(1);
  styleButton(clearButton, clearPlaylistButtonText, app_theme::warningAction,
              app_theme::warningActionHover, app_theme::warningActionPressed,
              app_theme::accentBorder);
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
  auto *panel = makePanel(i18n::message("music_player.page.player.label"), &playerSubtitleText);
  panel->setFlex(1);
  page->addView(panel);

  auto *viewport = new ScrollView();
  viewport->setName("musicPlayerViewport");
  viewport->setFlex(1)->setMinHeight(0);
  panel->addView(viewport);
  auto *workspace = new View();
  workspace->setHeight(780)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setGap(16);
  viewport->setContentView(workspace);

  auto *nowColumn = new View();
  nowColumn->setName("musicPlayerControls");
  nowColumn->setWidth(400)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  auto *artFrame = new View();
  artFrame->setWidth(260)->setHeight(260)
      ->setAlignSelf(YGAlignCenter)
      ->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(app_theme::insetSurface)
      ->setThemedBorderColor(app_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::controlRadius());
  artworkFallbackText = new TextView(kFontPath, 18);
  artworkFallbackText->setLocalizedText(i18n::message("music_player.player.no_album_art.label"));
  artworkFallbackText->setHeight(36);
  artworkFallbackText->setAlign(TextView::CENTER);
  artworkFallbackText->setVAlign(TextView::MIDDLE);
  artworkFallbackText->setThemedColor(app_theme::textMuted);
  artFrame->addView(artworkFallbackText);
  artworkImage = new ImageView(0, 0, 0, 0);
  artworkImage->setWidth(260)
      ->setHeight(260)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setCornerRadius(app_theme::controlRadius());
  artFrame->addView(artworkImage);
  nowColumn->addView(artFrame);

  currentTitleText = new TextView(app_theme::boldFont, 27);
  currentTitleText->setHeight(40);
  currentTitleText->setThemedColor(app_theme::textPrimary);
  currentTitleText->setOverflow(TextView::TextOverflow::Marquee);
  nowColumn->addView(currentTitleText);

  currentDetailText = new TextView(kFontPath, 17);
  currentDetailText->setHeight(32);
  currentDetailText->setOverflow(TextView::TextOverflow::Marquee);
  currentDetailText->setThemedColor(app_theme::textSecondary);
  nowColumn->addView(currentDetailText);

  playbackText = new TextView(kFontPath, 18);
  playbackText->setHeight(32);
  playbackText->setThemedColor(app_theme::textSecondary);
  nowColumn->addView(playbackText);

  seekProgressTrack = new View();
  seekProgressTrack->setHeight(24)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignStretch)
      ->setThemedBackgroundColor(app_theme::insetSurface)
      ->setThemedBorderColor(app_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(app_theme::controlRadius());
  seekProgressFill = new MusicSeekProgressFillView();
  seekProgressFill->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 1)
      ->setPosition(Edge::Right, 1)
      ->setPosition(Edge::Top, 1)
      ->setPosition(Edge::Bottom, 1);
  seekProgressTrack->addView(seekProgressFill);
  nowColumn->addView(seekProgressTrack);

  auto *transport = new View();
  transport->setHeight(308)->setFlexShrink(0)->setFlexDirection(FlexDirection::Column)->setGap(9);
  auto *transportRow = new View();
  transportRow->setHeight(64)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(10);
  auto *actionRowA = new View();
  actionRowA->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  auto *actionRowB = new View();
  actionRowB->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  auto *playbackRateRow = new View();
  playbackRateRow->setHeight(52)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10);
  auto *clubModeRow = new View();
  clubModeRow->setHeight(52)->setFlexDirection(FlexDirection::Row);

  TextView *previousText = nullptr;
  auto *previousButton = makeIconButton(kIconBackwardStep, 28, &previousText);
  styleButton(previousButton, previousText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  previousButton->setOnClickListener([this]() { playPrevious(); });

  TextView *back10Text = nullptr;
  auto *back10Button = makeIconButton(kIconRotateLeft, 23, &back10Text);
  styleButton(back10Button, back10Text, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  back10Button->setOnClickListener([this]() { seekRelative(-10000000LL); });

  auto *playPauseButton = makeIconButton(kIconPlay, 33, &playPauseButtonText);
  playPauseButton->setWidth(96);
  playPauseButton->setHeight(64);
  styleButton(playPauseButton, playPauseButtonText, app_theme::infoAction,
              app_theme::infoActionHover, app_theme::infoActionPressed,
              app_theme::accentBorder);
  playPauseButton->setOnClickListener([this]() { togglePlayback(); });

  TextView *forward10Text = nullptr;
  auto *forward10Button = makeIconButton(kIconRotateRight, 23, &forward10Text);
  styleButton(forward10Button, forward10Text, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  forward10Button->setOnClickListener([this]() { seekRelative(10000000LL); });

  TextView *nextText = nullptr;
  auto *nextButton = makeIconButton(kIconForwardStep, 28, &nextText);
  styleButton(nextButton, nextText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  nextButton->setOnClickListener([this]() { playNext(); });

  TextView *playSelectedText = nullptr;
  auto *playSelectedButton = makeButton(i18n::message("music_player.player.play.label"), 17, &playSelectedText);
  playSelectedButton->setFlex(1);
  styleButton(playSelectedButton, playSelectedText, app_theme::primaryAction,
              app_theme::primaryActionHover, app_theme::primaryActionPressed,
              app_theme::accentBorderStrong);
  playSelectedButton->setOnClickListener(
      [this]() { playSelectedQueueTrack(); });

  auto *watchVideoButton =
      makeIconButton(kIconVideo, 22, &watchVideoButtonText);
  watchVideoButton->setHeight(52)->setFlexGrow(1)->setFlexBasis(0);
  styleButton(watchVideoButton, watchVideoButtonText, app_theme::successAction,
              app_theme::successActionHover, app_theme::successActionPressed,
              app_theme::accentBorder);
  watchVideoButton->setOnClickListener([this]() { watchVideo(); });

  auto *repeatButton =
      makeDualIconButton(kIconRepeat, repeatModeStateIcon(displayedRepeatMode),
                         20, &repeatModeBaseButtonText, &repeatModeButtonText);
  repeatButton->setHeight(52)->setFlexGrow(1)->setFlexBasis(0);
  styleButton(repeatButton, repeatModeButtonText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  if (repeatModeBaseButtonText != nullptr) {
    repeatModeBaseButtonText->setThemedColor(
        [] { return app_theme::textOn(app_theme::control()); });
  }
  repeatButton->setOnClickListener([this]() { cycleRepeatMode(); });

  TextView *shuffleText = nullptr;
  auto *shuffleButton = makeIconButton(kIconShuffle, 22, &shuffleText);
  shuffleButton->setHeight(52)->setFlexGrow(1)->setFlexBasis(0);
  styleButton(shuffleButton, shuffleText, app_theme::control,
              app_theme::controlHover, app_theme::controlPressed,
              app_theme::hairlineStrong);
  shuffleButton->setOnClickListener([this]() { shuffleQueue(); });

  TextView *stopText = nullptr;
  auto *stopButton = makeIconButton(kIconStop, 22, &stopText);
  stopButton->setHeight(52)->setFlexGrow(1)->setFlexBasis(0);
  styleButton(stopButton, stopText, app_theme::warningAction,
              app_theme::warningActionHover, app_theme::warningActionPressed,
              app_theme::accentBorder);
  stopButton->setOnClickListener([this]() { stopPlayback(); });

  playbackModeDropdown = new DropdownView(
      {
          .onOpenChanged =
              [this](bool open) {
                playbackModeDropdownOpen = open;
                refreshPlaybackRateControl();
              },
          .onOptionSelected =
              [this](const std::string &id) { setPlaybackMode(id); },
      },
      overlayPortal);
  playbackModeDropdown->setWidth(150)->setFlexShrink(0.0F);

  auto *rateLabel = new TextView(kFontPath, 16);
  rateLabel->setLocalizedText(i18n::message("music_player.rate.label"));
  rateLabel->setWidth(40);
  rateLabel->setVAlign(TextView::MIDDLE);
  rateLabel->setThemedColor(app_theme::textSecondary);
  playbackRateSlider =
      new SnappedSlider([this](int percent) { setPlaybackRate(percent); });
  playbackRateSlider->setFlex(1.0F)->setMinWidth(100);
  playbackRateValueText = new TextView(kFontPath, 16);
  playbackRateValueText->setWidth(48);
  playbackRateValueText->setAlign(TextView::RIGHT);
  playbackRateValueText->setVAlign(TextView::MIDDLE);
  playbackRateValueText->setThemedColor(app_theme::textPrimary);

  clubModeButton = makeButton("", 17, nullptr);
  clubModeButtonContent = new CheckboxButtonContent(i18n::tr("music_player.club_beat.label"), 17, 16);
  clubModeButton->setContentView(clubModeButtonContent);
  clubModeButton->setFlex(1.0f);
  clubModeButton->setOnClickListener([this]() { toggleClubMode(); });

  transportRow->addView(previousButton);
  transportRow->addView(back10Button);
  transportRow->addView(playPauseButton);
  transportRow->addView(forward10Button);
  transportRow->addView(nextButton);
  actionRowA->addView(playSelectedButton);
  actionRowA->addView(watchVideoButton);
  actionRowB->addView(repeatButton);
  actionRowB->addView(shuffleButton);
  actionRowB->addView(stopButton);
  playbackRateRow->addView(playbackModeDropdown);
  playbackRateRow->addView(rateLabel);
  playbackRateRow->addView(playbackRateSlider);
  playbackRateRow->addView(playbackRateValueText);
  clubModeRow->addView(clubModeButton);
  transport->addView(transportRow);
  transport->addView(actionRowA);
  transport->addView(actionRowB);
  transport->addView(playbackRateRow);
  transport->addView(clubModeRow);
  refreshPlaybackRateControl();
  refreshClubModeControl();
  nowColumn->addView(transport);
  workspace->addView(nowColumn);

  auto *queueColumn = new View();
  queueColumn->setName("musicPlayerQueue");
  queueColumn->setFlex(1)
      ->setMinWidth(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);

  queueTitleText = new TextView(kFontPath, 18);
  queueTitleText->setLocalizedText(i18n::message("music_player.queue.label"));
  queueTitleText->setHeight(28);
  queueTitleText->setThemedColor(app_theme::textSecondary);
  queueColumn->addView(queueTitleText);

  playerQueueList =
      new RecyclerView<MusicTrack>(music_playlist::SameTrackIdentity);
  playerQueueList->setFlex(1)->setMinHeight(204);
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
  queueButtons->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  TextView *playPlaylistText = nullptr;
  auto *playPlaylistButton = makeButton(i18n::message("music_player.player.play.label"), 17, &playPlaylistText);
  playPlaylistButton->setFlex(1);
  styleButton(playPlaylistButton, playPlaylistText, app_theme::primaryAction,
              app_theme::primaryActionHover, app_theme::primaryActionPressed,
              app_theme::accentBorderStrong);
  playPlaylistButton->setOnClickListener([this]() { playPlaylist(); });
  TextView *editText = nullptr;
  auto *editButton = makeButton(i18n::message("music_player.edit.label"), 17, &editText);
  editButton->setFlex(1);
  styleButton(editButton, editText, app_theme::control, app_theme::controlHover,
              app_theme::controlPressed, app_theme::hairlineStrong);
  editButton->setOnClickListener(
      [this]() { switchTab(MusicPlayerTab::Playlists); });
  TextView *saveQueueText = nullptr;
  auto *saveQueueButton = makeButton(i18n::message("music_player.save_queue.label"), 17, &saveQueueText);
  saveQueueButton->setFlex(1);
  styleButton(saveQueueButton, saveQueueText, app_theme::successAction,
              app_theme::successActionHover, app_theme::successActionPressed,
              app_theme::accentBorder);
  saveQueueButton->setOnClickListener([this]() { saveNowPlayingAsPlaylist(); });
  queueButtons->addView(playPlaylistButton);
  queueButtons->addView(editButton);
  queueButtons->addView(saveQueueButton);
  queueColumn->addView(queueButtons);

  auto *sleepCard = new View();
  sleepCard->setHeight(112)->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setPadding(Edge::Top, 12);
  auto *sleepCardRule = new View();
  sleepCardRule->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)->setPosition(Edge::Right, 0)
      ->setPosition(Edge::Top, 0)->setHeight(1)
      ->setThemedBackgroundColor(main_menu_theme::rule);
  sleepCard->addView(sleepCardRule);
  auto *sleepHeader = new View();
  sleepHeader->setHeight(26)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10);
  auto *sleepTitle = new TextView(kFontPath, 18);
  sleepTitle->setLocalizedText(i18n::message("music_player.sleep_timer.label"));
  sleepTitle->setFlex(1);
  sleepTitle->setHeight(26);
  sleepTitle->setThemedColor(app_theme::textSecondary);
  sleepTimerStatusText = new TextView(kFontPath, 16);
  sleepTimerStatusText->setLocalizedText(i18n::message("music_player.off.label"));
  sleepTimerStatusText->setWidth(180);
  sleepTimerStatusText->setHeight(26);
  sleepTimerStatusText->setAlign(TextView::RIGHT);
  sleepTimerStatusText->setThemedColor(app_theme::textMuted);
  sleepHeader->addView(sleepTitle);
  sleepHeader->addView(sleepTimerStatusText);
  sleepCard->addView(sleepHeader);

  auto *sleepRow = new View();
  sleepRow->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  sleepTimerInput = new TextInputBox(kFontPath, 18);
  sleepTimerInput->setFlex(1);
  sleepTimerInput->setHeight(52);
  sleepTimerInput->setEditingText("30");
  sleepTimerInput->setThemedBackgroundColor(app_theme::control);
  sleepTimerInput->setThemedBorderColor(app_theme::hairlineStrong);
  sleepTimerInput->setBorderWidth(1);
  sleepTimerInput->setCornerRadius(app_theme::controlRadius());
  sleepTimerInput->setThemedColor(app_theme::textPrimary);
  sleepTimerInput->setVAlign(TextView::MIDDLE);
  sleepTimerInput->onSubmit(
      [this](const std::string &) { setSleepTimerFromInput(); });
  sleepTimerSetButton = makeButton(i18n::message("music_player.set.label"), 16, &sleepTimerSetText);
  sleepTimerSetButton->setWidth(92);
  styleButton(sleepTimerSetButton, sleepTimerSetText, app_theme::successAction,
              app_theme::successActionHover, app_theme::successActionPressed,
              app_theme::accentBorder);
  sleepTimerSetButton->setOnClickListener(
      [this]() { setSleepTimerFromInput(); });
  sleepTimerClearButton = makeButton(i18n::message("music_player.clear.label"), 16, &sleepTimerClearText);
  sleepTimerClearButton->setWidth(106);
  sleepTimerClearButton->setOnClickListener([this]() { clearSleepTimer(); });
  sleepRow->addView(sleepTimerInput);
  sleepRow->addView(sleepTimerSetButton);
  sleepRow->addView(sleepTimerClearButton);
  sleepCard->addView(sleepRow);
  queueColumn->addView(sleepCard);

  auto *privacyCard = new View();
  privacyCard->setHeight(112)->setFlexShrink(0)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setPadding(Edge::Top, 12);
  auto *privacyCardRule = new View();
  privacyCardRule->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)->setPosition(Edge::Right, 0)
      ->setPosition(Edge::Top, 0)->setHeight(1)
      ->setThemedBackgroundColor(main_menu_theme::rule);
  privacyCard->addView(privacyCardRule);
  auto *privacyTitle = new TextView(kFontPath, 18);
  privacyTitle->setLocalizedText(i18n::message("music_player.lock_screen.label"));
  privacyTitle->setHeight(26);
  privacyTitle->setThemedColor(app_theme::textSecondary);
  privacyCard->addView(privacyTitle);

  auto *privacyRow = new View();
  privacyRow->setHeight(52)->setFlexDirection(FlexDirection::Row)->setGap(10);
  systemPlaybackJacketButton =
      makeButton(i18n::message("music_player.jacket_on.label"), 16, &systemPlaybackJacketText);
  systemPlaybackJacketButton->setFlex(1);
  systemPlaybackJacketButton->setOnClickListener(
      [this]() { toggleSystemPlaybackJacket(); });
  systemPlaybackTitleButton =
      makeButton(i18n::message("music_player.title_on.label"), 16, &systemPlaybackTitleText);
  systemPlaybackTitleButton->setFlex(1);
  systemPlaybackTitleButton->setOnClickListener(
      [this]() { toggleSystemPlaybackTitle(); });
  systemPlaybackArtistButton =
      makeButton(i18n::message("music_player.artist_on.label"), 16, &systemPlaybackArtistText);
  systemPlaybackArtistButton->setFlex(1);
  systemPlaybackArtistButton->setOnClickListener(
      [this]() { toggleSystemPlaybackArtist(); });
  privacyRow->addView(systemPlaybackJacketButton);
  privacyRow->addView(systemPlaybackTitleButton);
  privacyRow->addView(systemPlaybackArtistButton);
  privacyCard->addView(privacyRow);
  queueColumn->addView(privacyCard);
  workspace->addView(queueColumn);
}

View *MusicPlayerScene::makePanel(const i18n::Text &,
                                  TextView **subtitleText) {
  auto *panel = new View();
  panel->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(10)
      ->setPadding(Edge::All, 24);

  if (subtitleText != nullptr) *subtitleText = nullptr;
  return panel;
}

Button *MusicPlayerScene::makeButton(const i18n::Text &label, int fontSize,
                                     TextView **textOut) {
  auto *button = new Button();
  button->setHeight(52);
  button->setCornerRadius(app_theme::controlRadius());

  auto *text = new TextView(fontSize >= 26 ? app_theme::boldFont : kFontPath, fontSize);
  text->setLocalizedText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Button *MusicPlayerScene::makeIconButton(uint32_t iconCodepoint, int fontSize,
                                         TextView **textOut) {
  auto *button = new Button();
  button->setHeight(56);
  button->setWidth(56);
  button->setCornerRadius(app_theme::controlRadius());

  auto *text = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  text->setText(ui_icons::textForCodepoint(iconCodepoint));
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Button *MusicPlayerScene::makeDualIconButton(uint32_t leadingIconCodepoint,
                                             uint32_t trailingIconCodepoint,
                                             int fontSize,
                                             TextView **leadingTextOut,
                                             TextView **trailingTextOut) {
  auto *button = new Button();
  button->setHeight(56);
  button->setWidth(78);
  button->setCornerRadius(app_theme::controlRadius());

  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(7);

  auto *leadingText = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  leadingText->setText(ui_icons::textForCodepoint(leadingIconCodepoint));
  leadingText->setWidth(24);
  leadingText->setHeight(56);
  leadingText->setAlign(TextView::CENTER);
  leadingText->setVAlign(TextView::MIDDLE);
  leadingText->setOverflow(TextView::TextOverflow::Hidden);
  row->addView(leadingText);

  auto *trailingText = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  trailingText->setText(ui_icons::textForCodepoint(trailingIconCodepoint));
  trailingText->setWidth(22);
  trailingText->setHeight(56);
  trailingText->setAlign(TextView::CENTER);
  trailingText->setVAlign(TextView::MIDDLE);
  trailingText->setOverflow(TextView::TextOverflow::Hidden);
  row->addView(trailingText);

  button->setContentView(row);
  if (leadingTextOut != nullptr) {
    *leadingTextOut = leadingText;
  }
  if (trailingTextOut != nullptr) {
    *trailingTextOut = trailingText;
  }
  return button;
}

Button *MusicPlayerScene::makeNavButton(const i18n::Text &label,
                                        TextView **textOut) {
  auto *button = makeButton(label, 28, textOut);
  button->setHeight(58);
  (*textOut)->setAlign(TextView::LEFT);
  (*textOut)->setAutoFitText(true);
  (*textOut)->setPadding(Edge::Left, 12);
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
    text->setThemedColor([normal] { return app_theme::textOn(normal()); });
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
  const int previousPlaylistId = selectedPlaylistId;
  const int servicePreferredPlaylistId =
      preserveSelection && selectedPlaylistId > 0 ? selectedPlaylistId : 0;

  std::string errorMessage;
  if (!context.musicPlayer.ReloadLibraryAndPlaylists(
          errorMessage, servicePreferredPlaylistId)) {
    setStatus(errorMessage);
  }
  const auto persistedState = context.musicPlayer.PlayerStateSnapshot();
  libraryTracks = context.musicPlayer.LibraryTracksSnapshot();
  favoriteTracks = context.musicPlayer.FavoriteTracksSnapshot();
  rebuildFavoriteTrackIds();
  libraryGroupTracks.clear();
  expandedLibraryGroupIds.clear();
  playlists = context.musicPlayer.PlaylistsSnapshot();
  selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
  playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
  if ((preserveSelection && isNowPlayingPlaylistId(previousPlaylistId)) ||
      (!preserveSelection &&
       isNowPlayingPlaylistId(persistedState.selectedPlaylistId))) {
    selectedPlaylistId = kNowPlayingPlaylistId;
  }

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

  refreshActiveQueueList(true);
  int playlistIndex = preserveSelection
                          ? findIndex(playlistTracks, selectedPlaylistTrackId)
                          : persistedState.playlistCursorIndex;
  if (!preserveSelection && playlistIndex < 0 &&
      isNowPlayingPlaylistId(selectedPlaylistId)) {
    playlistIndex = selectedQueueIndex;
  }
  const int preferredPlaylistId = preserveSelection
                                      ? previousPlaylistId
                                      : persistedState.selectedPlaylistId;
  applyTrackBrowserFilterForTrackId(
      TrackBrowserKind::Library, preserveSelection ? selectedLibraryId : "",
      preserveSelection);
  applyTrackBrowserFilterForTrackId(
      TrackBrowserKind::Favorites, preserveSelection ? selectedFavoriteId : "",
      preserveSelection);
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
  applyTrackBrowserFilterForTrackId(TrackBrowserKind::Library, preferredTrackId,
                                    preserveScroll, revealPreferredIfOutOfView);
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
    preferredTrackId = source[static_cast<std::size_t>(preferredIndex)].trackId;
  }

  applyTrackBrowserFilterForTrackId(kind, preferredTrackId);
}

void MusicPlayerScene::applyTrackBrowserFilterForTrackId(
    TrackBrowserKind kind, const std::string &preferredTrackId,
    bool preserveScroll, bool revealPreferredIfOutOfView) {
  auto *list = trackBrowserList(kind);
  if (list == nullptr) {
    return;
  }

  const float previousScrollOffset = preserveScroll ? list->scrollOffset : 0.0f;

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
        0.0f, static_cast<float>(std::max(1, list->size()) * list->itemHeight -
                                 list->getContentHeight()));
    list->scrollOffset =
        std::clamp(previousScrollOffset, 0.0f, maxScrollOffset);
    if (revealPreferredIfOutOfView && selectedIndex >= 0) {
      const float itemTop =
          static_cast<float>(selectedIndex * list->itemHeight);
      const float itemBottom = itemTop + static_cast<float>(list->itemHeight);
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
      preferredPlaylistId != 0
          ? preferredPlaylistId
          : (selectedLibraryPlaylistId != 0 ? selectedLibraryPlaylistId
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
  if (pendingClearPlaylistId != 0 &&
      playlistChoiceIndexForId(pendingClearPlaylistId) < 0) {
    pendingClearPlaylistId = 0;
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
  selectedPlaylistIndex = preferredIndex >= 0 && preferredIndex < trackCount
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
    const auto text = i18n::format(
        trimPlaylistName(librarySearchText).empty()
            ? "music_player.library.track_count"
            : "music_player.library.search_match_count",
        {{"visible", std::to_string(filteredLibraryTracks.size())},
         {"total", std::to_string(libraryTracks.size())}});
    librarySubtitleText->setText(text);
  }
  if (favoritesSubtitleText != nullptr) {
    const auto text = i18n::format(
        trimPlaylistName(favoritesSearchText).empty()
            ? "music_player.favorites.track_count"
            : "music_player.favorites.search_match_count",
        {{"visible", std::to_string(filteredFavoriteTracks.size())},
         {"total", std::to_string(favoriteTracks.size())}});
    favoritesSubtitleText->setText(text);
  }
  if (playlistSubtitleText != nullptr) {
    playlistSubtitleText->setText(
        i18n::format("music_player.playlist_group.summary",
                     {{"name", selectedPlaylistName()},
                      {"lists", std::to_string(playlists.size())},
                      {"tracks", std::to_string(playlistTracks.size())}}));
  }
  if (playerSubtitleText != nullptr) {
    playerSubtitleText->setText(
        queueDisplayName(displayedQueueName) + " · " +
        i18n::format("music_player.queue.track_count",
                     {{"count", std::to_string(queueTracks.size())}}) + " · " +
        repeatModeLabel(displayedRepeatMode) + " · " +
        std::to_string(context.musicPlayer.PlaybackRate().percent) + "% " +
        playbackModeLabel(context.musicPlayer.PlaybackRate().mode) +
        (context.musicPlayer.ClubMode() ? i18n::tr("music_player.club_beat.suffix") : ""));
  }
  if (queueTitleText != nullptr) {
    queueTitleText->setText(queueDisplayName(displayedQueueName));
  }
  if (deletePlaylistButtonText != nullptr) {
    deletePlaylistButtonText->setText(pendingDeletePlaylistId != 0 &&
                                              pendingDeletePlaylistId ==
                                                  selectedPlaylistId
                                          ? i18n::tr("music_player.confirm_delete.label")
                                          : i18n::tr("music_player.delete.label"));
  }
  if (clearPlaylistButtonText != nullptr) {
    clearPlaylistButtonText->setText(pendingClearPlaylistId != 0 &&
                                             pendingClearPlaylistId ==
                                                 selectedPlaylistId
                                         ? i18n::tr("music_player.confirm_clear.label")
                                         : i18n::tr("music_player.clear.label"));
  }
  if (libraryGroupButtonText != nullptr) {
    const auto track = selectedLibraryTrack();
    if (track && !track->groupId.empty() &&
        expandedLibraryGroupIds.contains(track->groupId)) {
      libraryGroupButtonText->setLocalizedText(i18n::message("music_player.collapse.label"));
    } else {
      libraryGroupButtonText->setLocalizedText(i18n::message("music_player.expand.label"));
    }
  }

  std::optional<MusicTrack> current =
      context.musicPlayer.CurrentTrackSnapshot();
  std::optional<MusicTrack> shown =
      current.has_value() ? current : displayTrack();
  if (librarySelectionTitleText != nullptr) {
    const auto track = selectedLibraryTrack();
    librarySelectionTitleText->setText(track ? trackTitle(*track)
                                             : i18n::tr("music_player.no_selection.label"));
  }
  if (librarySelectionDetailText != nullptr) {
    const auto track = selectedLibraryTrack();
    librarySelectionDetailText->setText(track ? trackDetail(*track) : "");
  }
  if (favoritesSelectionTitleText != nullptr) {
    const auto track = selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
    favoritesSelectionTitleText->setText(track ? trackTitle(*track)
                                               : i18n::tr("music_player.no_selection.label"));
  }
  if (favoritesSelectionDetailText != nullptr) {
    const auto track = selectedTrackBrowserTrack(TrackBrowserKind::Favorites);
    favoritesSelectionDetailText->setText(track ? trackDetail(*track) : "");
  }
  if (playlistSelectionTitleText != nullptr) {
    const auto track = selectedPlaylistTrack();
    playlistSelectionTitleText->setText(track ? trackTitle(*track)
                                              : i18n::tr("music_player.no_selection.label"));
  }
  if (playlistSelectionDetailText != nullptr) {
    const auto track = selectedPlaylistTrack();
    playlistSelectionDetailText->setText(track ? trackDetail(*track) : "");
  }
  if (currentTitleText != nullptr) {
    currentTitleText->setText(shown ? trackTitle(*shown) : i18n::tr("music_player.no_track_selected.label"));
  }
  if (currentDetailText != nullptr) {
    currentDetailText->setText(shown ? trackDetail(*shown)
                                     : i18n::tr("music_player.track.empty_description"));
  }
  if (playbackText != nullptr) {
    if (!playback.supported) {
      playbackText->setLocalizedText(i18n::message("music_player.native_playback_unavailable.label"));
    } else if (!playback.loaded) {
      playbackText->setLocalizedText(i18n::message("music_player.idle.label"));
    } else {
      playbackText->setText((playback.playing ? i18n::tr("music_player.playing.prefix") : i18n::tr("music_player.paused.prefix")) +
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
        ui_icons::textForCodepoint(playback.playing ? kIconPause : kIconPlay));
  }
  if (repeatModeButtonText != nullptr) {
    repeatModeButtonText->setText(
        ui_icons::textForCodepoint(repeatModeStateIcon(displayedRepeatMode)));
  }
  if (statusText != nullptr) {
    statusText->setText(statusMessage);
  }
  refreshSleepTimerUi();
  refreshSystemPlaybackPrivacyButtons();
  refreshClubModeControl();
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
    const bool selected = activeTab == tab;
    button->setThemedBackgroundColors(
        selected ? main_menu_theme::action : main_menu_theme::control,
        selected ? main_menu_theme::actionHover : main_menu_theme::hover,
        selected ? main_menu_theme::actionPressed : main_menu_theme::pressed);
    button->setStyledBorderWidth(0);
    text->setThemedColor(selected ? main_menu_theme::onAction
                                 : main_menu_theme::ink);
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

TextView *
MusicPlayerScene::trackBrowserSubtitleText(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySubtitleText
                                           : favoritesSubtitleText;
}

TextView *
MusicPlayerScene::trackBrowserSelectionTitleText(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySelectionTitleText
                                           : favoritesSelectionTitleText;
}

TextView *
MusicPlayerScene::trackBrowserSelectionDetailText(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? librarySelectionDetailText
                                           : favoritesSelectionDetailText;
}

ImageView *
MusicPlayerScene::trackBrowserArtworkImage(TrackBrowserKind kind) const {
  return kind == TrackBrowserKind::Library ? libraryArtworkImage
                                           : favoritesArtworkImage;
}

TextView *
MusicPlayerScene::trackBrowserArtworkFallbackText(TrackBrowserKind kind) const {
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
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(tracks.size())) {
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

std::optional<MusicPlayerScene::MusicTrack>
MusicPlayerScene::displayTrack() const {
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

MusicPlayerScene::PlaylistInfo
MusicPlayerScene::nowPlayingPlaylistInfo() const {
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
    return playlist->name.empty() ? i18n::tr("music_player.playlist.untitled_playlist.label") : playlist->name;
  }
  return i18n::tr("music_player.playlist.no_playlist.label");
}

std::string MusicPlayerScene::selectedPlaylistName() const {
  if (const auto playlist = selectedPlaylistInfo()) {
    return playlist->name.empty() ? i18n::tr("music_player.playlist.untitled_playlist.label") : playlist->name;
  }
  return i18n::tr("music_player.playlist.no_playlist.label");
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
  return i18n::tr("music_player.playlist.playlist.label");
}

std::string
MusicPlayerScene::uniquePlaylistName(std::string desiredName) const {
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
    playbackModeDropdownOpen = false;
    refreshPlaybackRateControl();
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
    pendingClearPlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(playlistId);
    refreshLibraryPlaylistList(playlistId);
    refreshPlaylistList(-1);
    if (playlistNameInput != nullptr) {
      playlistNameInput->setEditingText(nextPlaylistName());
    }
    setStatus("");
    refreshUi();
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::saveNowPlayingAsPlaylist() {
  const auto snapshot = context.musicPlayer.QueueSnapshot();
  if (snapshot.tracks.empty()) {
    setStatus(i18n::tr("music_player.playlist.now_playing_empty.message"));
    return;
  }

  std::string name =
      activeTab == MusicPlayerTab::Playlists && playlistNameInput != nullptr
          ? playlistNameInput->getText()
          : "";
  name = uniquePlaylistName(
      name.empty() ? music_playlist::kNowPlayingDisplayName : name);

  std::string errorMessage;
  const int playlistId = context.musicPlayer.CreatePlaylistFromTracks(
      name, snapshot.tracks, errorMessage);
  if (playlistId <= 0) {
    setStatus(errorMessage);
    return;
  }

  playlists = context.musicPlayer.PlaylistsSnapshot();
  pendingClearPlaylistId = 0;
  selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
  playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
  refreshActiveQueueList(true);
  refreshPlaylistDirectoryList(playlistId);
  refreshLibraryPlaylistList(playlistId);
  refreshPlaylistList(-1);
  if (playlistNameInput != nullptr) {
    playlistNameInput->setEditingText(nextPlaylistName());
  }
  setStatus("");
  refreshUi();
}

void MusicPlayerScene::renameSelectedPlaylist() {
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    setStatus(i18n::tr("music_player.playlist.now_playing_unable_renamed.message"));
    return;
  }

  std::string name =
      playlistRenameInput != nullptr ? playlistRenameInput->getText() : "";
  name = trimPlaylistName(name);
  if (name.empty()) {
    setStatus(i18n::tr("music_player.playlist.playlist_name_empty.message"));
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
    setStatus("");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::deleteSelectedPlaylist() {
  if (selectedPlaylistId <= 0) {
    pendingDeletePlaylistId = 0;
    setStatus(i18n::tr("music_player.playlist.select_playlist.message"));
    refreshUi();
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    pendingDeletePlaylistId = 0;
    setStatus(i18n::tr("music_player.playlist.use_clear_now_playing.message"));
    refreshUi();
    return;
  }

  const std::string playlistName = selectedPlaylistName();
  if (pendingDeletePlaylistId != selectedPlaylistId) {
    pendingDeletePlaylistId = selectedPlaylistId;
    setStatus(i18n::format("music_player.playlist.delete.title", {{"name", playlistName}}));
    refreshUi();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.DeleteSelectedPlaylist(errorMessage)) {
    pendingDeletePlaylistId = 0;
    pendingClearPlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
    refreshPlaylistList(-1);
    setStatus("");
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
  selectedIndex =
      index >= 0 && index < static_cast<int>(tracks.size()) ? index : -1;

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

void MusicPlayerScene::persistPlaylistSelection() {
  std::string ignored;
  context.musicPlayer.SavePlaylistCursor(selectedPlaylistId,
                                         selectedPlaylistIndex, ignored);
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
      nextFavorite ? toggledTrackId : fallbackFavoriteId, true, true);
  rebindFavoriteAwareTrackLists();
  refreshTrackBrowserArtwork(
      TrackBrowserKind::Favorites,
      selectedTrackBrowserTrack(TrackBrowserKind::Favorites));
  setStatus("");
  refreshUi();
}

void MusicPlayerScene::toggleSelectedLibraryGroup() {
  const auto track = selectedLibraryTrack();
  if (!track) {
    setStatus(i18n::tr("music_player.select_track.message"));
    return;
  }
  if (track->groupId.empty()) {
    setStatus(i18n::tr("music_player.no_chart_group.message"));
    return;
  }

  if (expandedLibraryGroupIds.contains(track->groupId)) {
    expandedLibraryGroupIds.erase(track->groupId);
    applyLibraryFilterForTrackId(track->groupId, true, true);
    refreshLibraryArtwork(selectedLibraryTrack());
    refreshUi();
    setStatus("");
    return;
  }

  if (!track->groupRepresentative && !track->expandedChart) {
    setStatus(i18n::tr("music_player.no_grouped_charts.message"));
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
    setStatus(i18n::tr("music_player.no_alternate_charts.message"));
    return;
  }

  expandedLibraryGroupIds.insert(track->groupId);
  applyLibraryFilterForTrackId(childrenIt->second.front().trackId, true);
  refreshLibraryArtwork(selectedLibraryTrack());
  refreshUi();
  setStatus("");
}

void MusicPlayerScene::selectPlaylist(int index) {
  if (index < 0 || index >= static_cast<int>(playlistChoices.size())) {
    setStatus(i18n::tr("music_player.playlist.select_playlist.message"));
    return;
  }
  const int playlistId = playlistChoices[static_cast<std::size_t>(index)].id;
  if (isNowPlayingPlaylistId(playlistId)) {
    pendingClearPlaylistId = 0;
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
    persistPlaylistSelection();
    setStatus("");
    refreshUi();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.SelectPlaylist(playlistId, errorMessage)) {
    pendingClearPlaylistId = 0;
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
    persistPlaylistSelection();
    setStatus("");
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
  persistPlaylistSelection();
  refreshUi();
}

void MusicPlayerScene::selectQueueTrack(int index) {
  const int previousIndex = selectedQueueIndex;
  selectedQueueIndex =
      index >= 0 && index < static_cast<int>(queueTracks.size()) ? index : -1;
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
                  ? i18n::tr("music_player.playlist.select_library_track_first.message")
                  : i18n::tr("music_player.playlist.select_favorite_track_first.message"));
    return;
  }
  const auto targetPlaylist = selectedLibraryPlaylistInfo();
  if (!targetPlaylist) {
    setStatus(i18n::tr("music_player.playlist.select_playlist.message"));
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
  const bool targetWasActiveQueue =
      !queueTracks.empty() &&
      queueDisplayName(displayedQueueName) == targetPlaylistName;
  const std::size_t preferredQueueIndex =
      selectedQueueIndex >= 0 ? static_cast<std::size_t>(selectedQueueIndex)
                              : queueTracks.size();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  std::string errorMessage;
  if (context.musicPlayer.AddChartToPlaylist(
          targetPlaylistId, track->representativeChart, errorMessage)) {
    pendingClearPlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    selectedPlaylistId = context.musicPlayer.SelectedPlaylistId();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    if (targetWasActiveQueue) {
      std::string queueError;
      if (!context.musicPlayer.AppendToQueue(*track, preferredQueueIndex,
                                             targetPlaylistName, queueError)) {
        setStatus(queueError);
        refreshUi();
        return;
      }
      refreshActiveQueueList(true);
    } else {
      syncActiveQueueAfterPlaylistEdit(wasActiveQueue, previousCurrent,
                                       selectedQueueIndex);
    }
    refreshLibraryPlaylistList(targetPlaylistId);
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(selectedPlaylistId == targetPlaylistId
                            ? static_cast<int>(playlistTracks.size()) - 1
                            : selectedPlaylistIndex);
    setStatus("");
    refreshUi();
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::addLibraryTrackToNowPlaying(const MusicTrack &track) {
  pendingClearPlaylistId = 0;
  const std::size_t preferredIndex =
      selectedQueueIndex >= 0 ? static_cast<std::size_t>(selectedQueueIndex)
                              : queueTracks.size();
  std::string errorMessage;
  if (!context.musicPlayer.AppendToQueue(track, preferredIndex,
                                         music_playlist::kNowPlayingDisplayName,
                                         errorMessage)) {
    setStatus(errorMessage);
    refreshUi();
    return;
  }
  refreshActiveQueueList(true);
  setStatus("");
  refreshUi();
}

void MusicPlayerScene::removePlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus(i18n::tr("music_player.playlist.select_track.message"));
    return;
  }
  const int nextIndex = selectedPlaylistIndex;
  const bool wasActiveQueue = selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  const auto previousQueue = context.musicPlayer.QueueSnapshot();
  const std::optional<int> previousQueueIndex =
      previousQueue.currentIndex
          ? std::optional<int>(static_cast<int>(*previousQueue.currentIndex))
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
               music_playlist::SameTrackIdentity(*previousCurrent, *track)) {
      currentTrackRemoved = true;
    } else if (previousQueue.detachedCurrentNextIndex && playback.loaded) {
      currentTrackRemoved = true;
    }

    if (currentTrackRemoved && playback.loaded) {
      pendingClearPlaylistId = 0;
      context.musicPlayer.SetPlaylistAfterCurrentRemoved(
          std::move(tracks), static_cast<std::size_t>(detachedNextIndex),
          music_playlist::kNowPlayingDisplayName);
      refreshActiveQueueList(true);
      selectPlaylistTrack(-1);
      setStatus("");
    } else {
      pendingClearPlaylistId = 0;
      replaceNowPlaying(std::move(tracks),
                        queueIndex >= 0 ? queueIndex : preferredIndex,
                        i18n::tr("music_player.playlist.removed_from_now_playing.message"));
      selectPlaylistTrack(-1);
    }
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.RemoveChartFromSelectedPlaylist(
          track->representativeChart, errorMessage, track->storedItemId)) {
    pendingClearPlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    syncActiveQueueAfterPlaylistEdit(wasActiveQueue, previousCurrent, nextIndex,
                                     previousQueueIndex, nextIndex);
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshLibraryPlaylistList(selectedLibraryPlaylistId);
    refreshPlaylistList(
        std::min(nextIndex, static_cast<int>(playlistTracks.size()) - 1));
    selectPlaylistTrack(-1);
    setStatus("");
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::movePlaylistTrack(int delta) {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus(i18n::tr("music_player.playlist.select_track.message"));
    return;
  }
  const bool wasActiveQueue = selectedPlaylistIsActiveQueue();
  const auto previousCurrent = context.musicPlayer.CurrentTrackSnapshot();
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    const int targetIndex = selectedPlaylistIndex + delta;
    if (targetIndex < 0 ||
        targetIndex >= static_cast<int>(playlistTracks.size())) {
      setStatus(delta < 0 ? i18n::tr("music_player.playlist.selected_track_already_at_top.message")
                          : i18n::tr("music_player.playlist.selected_track_already_at_bottom.message"));
      return;
    }
    pendingClearPlaylistId = 0;
    std::vector<MusicTrack> tracks = playlistTracks;
    std::swap(tracks[static_cast<std::size_t>(selectedPlaylistIndex)],
              tracks[static_cast<std::size_t>(targetIndex)]);
    replaceNowPlaying(std::move(tracks), targetIndex,
                      delta < 0 ? i18n::tr("music_player.playlist.moved_track_up.message") : i18n::tr("music_player.playlist.moved_track_down.message"));
    refreshUi();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.MoveChartInSelectedPlaylist(
          track->representativeChart, delta, errorMessage,
          track->storedItemId)) {
    pendingClearPlaylistId = 0;
    playlists = context.musicPlayer.PlaylistsSnapshot();
    playlistTracks = context.musicPlayer.SelectedPlaylistTracksSnapshot();
    syncActiveQueueAfterPlaylistEdit(
        wasActiveQueue, previousCurrent,
        std::clamp(selectedPlaylistIndex + delta, 0,
                   std::max(0, static_cast<int>(playlistTracks.size()) - 1)));
    refreshPlaylistDirectoryList(selectedPlaylistId);
    refreshPlaylistList(
        std::clamp(selectedPlaylistIndex + delta, 0,
                   std::max(0, static_cast<int>(playlistTracks.size()) - 1)));
    setStatus("");
    refreshUi();
  } else {
    setStatus(errorMessage);
  }
}

void MusicPlayerScene::clearPlaylist() {
  if (selectedPlaylistId == 0) {
    pendingClearPlaylistId = 0;
    setStatus(i18n::tr("music_player.playlist.select_playlist.message"));
    refreshUi();
    return;
  }
  if (playlistTracks.empty()) {
    pendingClearPlaylistId = 0;
    setStatus(i18n::format("music_player.playlist.clear.already_empty", {{"name", selectedPlaylistName()}}));
    refreshUi();
    return;
  }
  if (pendingClearPlaylistId != selectedPlaylistId) {
    pendingClearPlaylistId = selectedPlaylistId;
    setStatus(i18n::format("music_player.playlist.clear.title", {{"name", selectedPlaylistName()}}));
    refreshUi();
    return;
  }
  pendingClearPlaylistId = 0;

  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    std::string ignoredStatus;
    context.musicPlayer.Stop(ignoredStatus);
    replaceNowPlaying({}, -1, i18n::tr("music_player.playlist.cleared_now_playing.message"));
    refreshUi();
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
    setStatus("");
  } else {
    setStatus(errorMessage);
  }
  refreshUi();
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
      queueIndex = std::clamp(*previousQueueIndex - 1, 0,
                              static_cast<int>(playlistTracks.size()) - 1);
    } else {
      queueIndex = std::clamp(*previousQueueIndex, 0,
                              static_cast<int>(playlistTracks.size()) - 1);
    }
  }
  if (previousCurrent) {
    if (queueIndex < 0 && !currentTrackRemoved) {
      if (const auto index = music_playlist::FindTrackIndex(playlistTracks,
                                                            *previousCurrent)) {
        queueIndex = static_cast<int>(*index);
      }
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
  int startIndex =
      tracks.empty()
          ? 0
          : std::clamp(preferredIndex, 0, static_cast<int>(tracks.size()) - 1);
  int currentIndex = -1;
  if (current && playback.loaded) {
    if (const auto index = music_playlist::FindTrackIndex(tracks, *current)) {
      currentIndex = static_cast<int>(*index);
    }
  }
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
                                      const i18n::Text &successMessage) {
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
                  ? i18n::tr("music_player.select_library_track_first.message")
                  : i18n::tr("music_player.select_favorite_track_first.message"));
    return;
  }
  playNowPlaying({*track}, 0,
                 kind == TrackBrowserKind::Library
                     ? i18n::tr("music_player.select_library_track_first.message")
                     : i18n::tr("music_player.select_favorite_track_first.message"),
                 i18n::message("music_player.playing_now_playing.message"));
}

void MusicPlayerScene::playPlaylist() {
  if (playlistTracks.empty()) {
    setStatus(i18n::format("music_player.playlist.empty_notice", {{"name", selectedPlaylistName()}}));
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    const std::size_t startIndex =
        selectedPlaylistIndex >= 0
            ? static_cast<std::size_t>(selectedPlaylistIndex)
            : 0;
    playNowPlaying(playlistTracks, startIndex, i18n::tr("music_player.playlist.now_playing_empty.message"),
                   i18n::message("music_player.playlist.playing_now_playing.message"));
    return;
  }

  context.jukebox.stop();
  std::string status;
  if (!context.musicPlayer.StartSelectedPlaylist(status)) {
    setStatus(status);
    return;
  }
  context.musicPlayer.PlayCurrentAsync(
      status, i18n::message("music_player.playlist.playing_named.message",
                            {{"name", selectedPlaylistName()}}));
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playSelectedPlaylistTrack() {
  const auto track = selectedPlaylistTrack();
  if (!track) {
    setStatus(i18n::tr("music_player.playlist.select_track.message"));
    return;
  }
  if (isNowPlayingPlaylistId(selectedPlaylistId)) {
    playNowPlaying(playlistTracks,
                   static_cast<std::size_t>(selectedPlaylistIndex),
                   i18n::tr("music_player.playlist.now_playing_empty.message"), i18n::message("music_player.playlist.playing_now_playing.message"));
    return;
  }

  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(
      playlistTracks, static_cast<std::size_t>(selectedPlaylistIndex),
      selectedPlaylistName());
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, i18n::message("music_player.playlist.playing_playlist_track.message"));
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playSelectedQueueTrack() {
  if (selectedQueueIndex < 0 ||
      selectedQueueIndex >= static_cast<int>(queueTracks.size())) {
    setStatus(i18n::tr("music_player.queue.select_track.message"));
    return;
  }
  context.jukebox.stop();
  context.musicPlayer.SetPlaylist(queueTracks,
                                  static_cast<std::size_t>(selectedQueueIndex),
                                  queueDisplayName(displayedQueueName));
  std::string status;
  context.musicPlayer.PlayCurrentAsync(status, i18n::message("music_player.queue.playing_queue_track.message"));
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
                     ? i18n::tr("music_player.no_library_tracks_available.message")
                     : i18n::tr("music_player.no_favorite_tracks_available.message"),
                 i18n::message("music_player.playing_now_playing.message"));
}

void MusicPlayerScene::shuffleQueue() {
  std::string status;
  if (!context.musicPlayer.ShuffleQueue(status)) {
    setStatus(status);
    refreshUi();
    return;
  }
  refreshActiveQueueList(true);
  setStatus("");
  refreshUi();
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
    setStatus(i18n::tr("music_player.playback.no_track_error"));
    return;
  }
  long long target = std::max(0LL, playback.positionMicros + deltaMicros);
  if (playback.durationMicros > 0) {
    if (deltaMicros > 0 && target >= playback.durationMicros) {
      const long long guardedTarget =
          std::max(0LL, playback.durationMicros - kRelativeSeekEndGuardMicros);
      if (playback.positionMicros >= guardedTarget) {
        setStatus(i18n::tr("music_player.near_end_track.message"));
        return;
      }
      target = guardedTarget;
    } else {
      target = std::min(target, playback.durationMicros);
    }
  }
  std::string status;
  if (context.musicPlayer.Seek(target, status)) {
    setStatus("");
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

void MusicPlayerScene::setPlaybackRate(int percent) {
  const audio::PlaybackRate rate{
      .percent = percent, .mode = context.musicPlayer.PlaybackRate().mode};
  std::string errorMessage;
  if (!context.musicPlayer.SetPlaybackRate(rate, errorMessage)) {
    setStatus(errorMessage);
    refreshPlaybackRateControl();
    return;
  }

  context.settings.musicPlayerPlaybackRatePercent = percent;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    setStatus(i18n::tr("music_player.playback_rate_changed_but_failed_save.message"));
  } else {
    setStatus("");
  }
  refreshPlaybackRateControl();
}

void MusicPlayerScene::setPlaybackMode(const std::string &id) {
  audio::PlaybackMode mode;
  if (id == "pitch-shift") {
    mode = audio::PlaybackMode::PitchShift;
  } else if (id == "time-stretch") {
    mode = audio::PlaybackMode::TimeStretch;
  } else {
    return;
  }

  audio::PlaybackRate rate = context.musicPlayer.PlaybackRate();
  rate.mode = mode;
  std::string errorMessage;
  if (!context.musicPlayer.SetPlaybackRate(rate, errorMessage)) {
    setStatus(errorMessage);
    playbackModeDropdownOpen = false;
    refreshPlaybackRateControl();
    return;
  }

  context.settings.musicPlayerPlaybackMode = mode;
  context.settings.sanitize();
  playbackModeDropdownOpen = false;
  if (!context.saveSettings()) {
    setStatus(i18n::tr("music_player.playback_mode_changed_but_failed_save.message"));
  } else {
    setStatus("");
  }
  refreshPlaybackRateControl();
}

void MusicPlayerScene::refreshPlaybackRateControl() {
  if (playbackModeDropdown == nullptr || playbackRateSlider == nullptr ||
      playbackRateValueText == nullptr) {
    return;
  }
  const audio::PlaybackRate rate = context.musicPlayer.PlaybackRate();
  playbackModeDropdown->refresh({
      .label = i18n::tr("music_player.mode.label"),
      .selectedId = playbackModeId(rate.mode),
      .options = {{.id = "pitch-shift", .label = i18n::tr("music_player.pitch_shift.label")},
                  {.id = "time-stretch", .label = i18n::tr("music_player.time_stretch.label")}},
      .open = playbackModeDropdownOpen,
      .enabled = true,
      .maxVisibleItems = 2,
  });
  playbackRateSlider->refresh(
      {.minimum = 50, .maximum = 200, .step = 5, .value = rate.percent});
  playbackRateValueText->setText(std::to_string(rate.percent) + "%");
}

void MusicPlayerScene::toggleClubMode() {
  const bool enabled = !context.musicPlayer.ClubMode();
  std::string status;
  if (!context.musicPlayer.SetClubMode(enabled, status)) {
    setStatus(status);
    refreshClubModeControl();
    return;
  }

  setStatus(status);
  if (context.musicPlayer.ClubMode() == enabled) {
    context.settings.musicPlayerClubModeEnabled = enabled;
    if (!context.saveSettings()) {
      setStatus(i18n::tr("music_player.club_beat_changed_but_failed_save.message"));
    }
  }
  refreshClubModeControl();
}

void MusicPlayerScene::refreshClubModeControl() {
  if (clubModeButton == nullptr || clubModeButtonContent == nullptr) {
    return;
  }
  const bool enabled = context.musicPlayer.ClubMode();
  clubModeButtonContent->setChecked(enabled);
  if (enabled) {
    styleButton(clubModeButton, clubModeButtonContent->labelView(),
                app_theme::primaryAction, app_theme::primaryActionHover,
                app_theme::primaryActionPressed, app_theme::accentBorderStrong);
    clubModeButtonContent->setThemedColor(
        [] { return app_theme::textOn(app_theme::primaryAction()); });
  } else {
    styleButton(clubModeButton, clubModeButtonContent->labelView(),
                app_theme::control, app_theme::controlHover,
                app_theme::controlPressed, app_theme::hairlineStrong);
    clubModeButtonContent->setThemedColor(
        [] { return app_theme::textOn(app_theme::control()); });
  }
}

void MusicPlayerScene::setSleepTimerFromInput() {
  if (sleepTimerInput == nullptr) {
    return;
  }

  std::string errorMessage;
  const auto durationMicros =
      parseSleepTimerDurationMicros(sleepTimerInput->getText(), errorMessage);
  if (!durationMicros) {
    setStatus(errorMessage);
    return;
  }

  std::string status;
  if (context.musicPlayer.SetSleepTimer(*durationMicros, status)) {
    setStatus("");
  } else {
    setStatus(status);
  }
  refreshSleepTimerUi();
}

void MusicPlayerScene::clearSleepTimer() {
  context.musicPlayer.ClearSleepTimer();
  setStatus("");
  refreshSleepTimerUi();
}

void MusicPlayerScene::toggleSystemPlaybackJacket() {
  context.settings.systemPlaybackShowJacket =
      !context.settings.systemPlaybackShowJacket;
  applySystemPlaybackPrivacy(true);
}

void MusicPlayerScene::toggleSystemPlaybackTitle() {
  context.settings.systemPlaybackShowTitle =
      !context.settings.systemPlaybackShowTitle;
  applySystemPlaybackPrivacy(true);
}

void MusicPlayerScene::toggleSystemPlaybackArtist() {
  context.settings.systemPlaybackShowArtist =
      !context.settings.systemPlaybackShowArtist;
  applySystemPlaybackPrivacy(true);
}

void MusicPlayerScene::applySystemPlaybackPrivacy(bool persist) {
  std::string errorMessage;
  const bool applied = native_music_player::SetMetadataVisibility(
      {.showTitle = context.settings.systemPlaybackShowTitle,
       .showArtist = context.settings.systemPlaybackShowArtist,
       .showArtwork = context.settings.systemPlaybackShowJacket},
      errorMessage);
  if (persist) {
    context.settings.sanitize();
    if (!context.saveSettings()) {
      setStatus(i18n::tr("music_player.could_not_save_system_playback_privacy.message"));
    } else if (!applied && !errorMessage.empty()) {
      setStatus(errorMessage);
    } else {
      setStatus("");
    }
  }
  refreshSystemPlaybackPrivacyButtons();
}

void MusicPlayerScene::refreshSleepTimerUi() {
  const long long remainingMicros =
      context.musicPlayer.SleepTimerRemainingMicros();
  const bool active = remainingMicros > 0;
  if (sleepTimerStatusText != nullptr) {
    sleepTimerStatusText->setText(
        active ? i18n::format("music_player.sleep_timer.remaining",
                              {{"time", formatSleepTimerDuration(remainingMicros)}})
               : i18n::tr("music_player.off.label"));
    sleepTimerStatusText->setThemedColor(active ? app_theme::textPrimary
                                                : app_theme::textMuted);
  }
  if (sleepTimerSetButton != nullptr) {
    styleButton(sleepTimerSetButton, sleepTimerSetText, app_theme::successAction,
                app_theme::successActionHover, app_theme::successActionPressed,
                app_theme::accentBorder);
  }
  if (sleepTimerClearButton != nullptr) {
    styleButton(sleepTimerClearButton, sleepTimerClearText,
                active ? app_theme::warningAction : app_theme::control,
                active ? app_theme::warningActionHover : app_theme::controlHover,
                active ? app_theme::warningActionPressed
                       : app_theme::controlPressed,
                active ? app_theme::accentBorder : app_theme::hairlineStrong);
  }
}

void MusicPlayerScene::refreshSystemPlaybackPrivacyButtons() {
  auto refreshToggle = [this](Button *button, TextView *text,
                              const std::string &label, bool visible) {
    if (text != nullptr) {
      text->setText(i18n::format(visible ? "music_player.display_option.enabled" : "music_player.display_option.disabled",
                                 {{"name", label}}));
    }
    if (button != nullptr) {
      styleButton(
          button, text, visible ? app_theme::successAction : app_theme::control,
          visible ? app_theme::successActionHover : app_theme::controlHover,
          visible ? app_theme::successActionPressed : app_theme::controlPressed,
          visible ? app_theme::accentBorder : app_theme::hairlineStrong);
    }
  };
  refreshToggle(systemPlaybackJacketButton, systemPlaybackJacketText, i18n::tr("music_player.jacket.label"),
                context.settings.systemPlaybackShowJacket);
  refreshToggle(systemPlaybackTitleButton, systemPlaybackTitleText, i18n::tr("music_player.title.label"),
                context.settings.systemPlaybackShowTitle);
  refreshToggle(systemPlaybackArtistButton, systemPlaybackArtistText, i18n::tr("music_player.artist.label"),
                context.settings.systemPlaybackShowArtist);
}

void MusicPlayerScene::seekToFraction(float fraction) {
  const auto playback = context.musicPlayer.PlaybackState();
  if (!playback.supported || !playback.loaded || playback.durationMicros <= 0) {
    setStatus(i18n::tr("music_player.no_seekable_track_loaded.message"));
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

bool MusicPlayerScene::handleProgressSeekEvents(SDL_Event &event,
                                                View *progressTrack,
                                                bool &mouseDown,
                                                SDL_FingerID &activeTouchId) {
  if (event.type == SDL_EVENT_FINGER_CANCELED) {
    if (activeTouchId == -1 || event.tfinger.fingerID != activeTouchId) {
      return false;
    }
    activeTouchId = -1;
    return true;
  }
  if (progressTrack == nullptr || !progressTrack->getVisible()) {
    return false;
  }

  const auto seekAt = [this, progressTrack](float uiX) {
    const int width = std::max(1, progressTrack->getWidth());
    const float fraction = (uiX - static_cast<float>(progressTrack->getX())) /
                           static_cast<float>(width);
    seekToFraction(fraction);
  };

  switch (event.type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN: {
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
  case SDL_EVENT_MOUSE_MOTION: {
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
  case SDL_EVENT_MOUSE_BUTTON_UP: {
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
  case SDL_EVENT_FINGER_DOWN: {
    if (activeTouchId != -1) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    if (!isInsideView(progressTrack, uiX, uiY)) {
      return false;
    }
    activeTouchId = event.tfinger.fingerID;
    seekAt(uiX);
    return true;
  }
  case SDL_EVENT_FINGER_MOTION: {
    if (event.tfinger.fingerID != activeTouchId) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, uiX, uiY);
    (void)uiY;
    seekAt(uiX);
    return true;
  }
  case SDL_EVENT_FINGER_UP: {
    if (event.tfinger.fingerID != activeTouchId) {
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
  std::optional<MusicTrack> currentTrack =
      context.musicPlayer.CurrentTrackSnapshot();
  std::optional<MusicTrack> track = currentTrack;
  if (!track) {
    track = displayTrack();
  }
  if (!track) {
    setStatus(i18n::tr("music_player.select_play_track.message"));
    return;
  }

  if (!playback.loaded) {
    if (currentTrack) {
      context.jukebox.stop();
      std::string status;
      context.musicPlayer.PlayCurrentAsync(status, i18n::message("music_player.playing_current_track.message"));
      refreshActiveQueueList(true);
      setStatus(status);
    } else {
      playNowPlaying({*track}, 0, i18n::tr("music_player.select_track_first.message"),
                     i18n::message("music_player.playing_now_playing.message"));
    }
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
        setStatus(i18n::tr("music_player.video_cancelled.message"));
      } else if (!chart) {
        setStatus(i18n::tr("music_player.could_not_play_video.message"));
      } else {
        setStatus(i18n::tr("music_player.no_bga_available.message"));
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
      setStatus(i18n::tr("music_player.video_cancelled.message"));
    }
    return false;
  }

  videoChart = std::move(chart);
  videoVisualsLoaded = true;
  if (showStatusMessage) {
    setStatus("");
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
  } else if (videoShowingArtwork) {
    layoutVideoArtwork();
  }

  if (videoControlsVisible && videoControlsVisibleUntil > 0 &&
      SDL_GetTicks() > videoControlsVisibleUntil && !videoSeekMouseDown &&
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
    videoTitleText->setText(shown ? trackTitle(*shown) : i18n::tr("music_player.no_track_selected.label"));
  }
  if (videoDetailText != nullptr) {
    videoDetailText->setText(shown ? trackDetail(*shown) : i18n::tr("music_player.track.empty_description"));
  }
  if (videoPlaybackText != nullptr) {
    if (!playback.loaded) {
      videoPlaybackText->setLocalizedText(i18n::message("music_player.idle.label"));
    } else {
      videoPlaybackText->setText(formatMusicTime(playback.positionMicros) +
                                 " / " +
                                 formatMusicTime(playback.durationMicros));
    }
  }
  if (videoPlayPauseButtonText != nullptr) {
    videoPlayPauseButtonText->setText(
        ui_icons::textForCodepoint(playback.playing ? kIconPause : kIconPlay));
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
  const int size = std::clamp(
      static_cast<int>(static_cast<float>(limit) * 0.76f), 180, limit);
  int imageWidth = size;
  int imageHeight = size;
  const int naturalWidth = videoArtworkImage->imageWidth();
  const int naturalHeight = videoArtworkImage->imageHeight();
  if (naturalWidth > 0 && naturalHeight > 0) {
    const float scale =
        std::min(static_cast<float>(size) / static_cast<float>(naturalWidth),
                 static_cast<float>(size) / static_cast<float>(naturalHeight));
    imageWidth = std::clamp(
        static_cast<int>(std::round(static_cast<float>(naturalWidth) * scale)),
        1, size);
    imageHeight = std::clamp(
        static_cast<int>(std::round(static_cast<float>(naturalHeight) * scale)),
        1, size);
  }
  const int x = std::max(0, (width - imageWidth) / 2);
  const int y = std::max(0, (height - imageHeight) / 2);
  videoArtworkImage->setSize(imageWidth, imageHeight);
  videoArtworkImage->setPositionNoLayout(x, y, YGPositionTypeAbsolute);
}

void MusicPlayerScene::showVideoControls(Uint64 durationMs) {
  if (!videoFullscreenActive) {
    return;
  }
  videoControlsVisible = true;
  videoControlsVisibleUntil = SDL_GetTicks() + durationMs;
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

  if (event.type == SDL_EVENT_KEY_DOWN) {
    showVideoControls();
    switch (event.key.key) {
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
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event.button.button == SDL_BUTTON_LEFT &&
        event.button.which != SDL_TOUCH_MOUSEID) {
      int uiX = 0;
      int uiY = 0;
      mouseButtonEventToUi(event.button, uiX, uiY);
      if (!isInsideView(videoControlsPanel, uiX, uiY)) {
        hideVideoControls();
        return true;
      }
    } else if (event.type == SDL_EVENT_FINGER_DOWN) {
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
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (event.button.button == SDL_BUTTON_LEFT &&
        event.button.which != SDL_TOUCH_MOUSEID) {
      showVideoControls();
      return true;
    }
    break;
  case SDL_EVENT_FINGER_DOWN:
    showVideoControls();
    return true;
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
  case SDL_EVENT_FINGER_MOTION:
    return true;
  default:
    break;
  }
  return true;
}

void MusicPlayerScene::playNext() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayNextAsync(status, i18n::message("music_player.playing_next_track.message"));
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::playPrevious() {
  context.jukebox.stop();
  std::string status;
  context.musicPlayer.PlayPreviousAsync(status, i18n::message("music_player.playing_previous_track.message"));
  refreshActiveQueueList(true);
  setStatus(status);
}

void MusicPlayerScene::stopPlayback() {
  if (videoFullscreenActive) {
    exitVideoFullscreen();
  }
  std::string status;
  if (context.musicPlayer.Stop(status)) {
    setStatus("");
  } else {
    setStatus(status);
  }
}

void MusicPlayerScene::goBack() {
  if (videoFullscreenActive) {
    exitVideoFullscreen();
    return;
  }
  if (embeddedTab) {
    if (tabClose) tabClose();
    return;
  }
  if (context.sceneManager != nullptr) {
    (void)returnToScene(*context.sceneManager, returnTarget_);
  }
}

void MusicPlayerScene::onInputQueueOverflow() {
  Scene::onInputQueueOverflow();
  seekMouseDown = false;
  activeSeekTouchId = -1;
  videoSeekMouseDown = false;
  activeVideoSeekTouchId = -1;
}
