#include "MusicTrackRowView.h"
#include "AppTheme.h"
#include "MainMenuTheme.h"
#include "Button.h"
#include "ImageView.h"
#include "TextView.h"
#include "../i18n/Localization.h"
#include "../path.h"
namespace {
constexpr const char *kFontPath = app_theme::font;
constexpr int kTrackRowHeight = 68;
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
    detail += "  " + i18n::format("music_player.group.chart_count",
        {{"count", std::to_string(track.chartCount)}});
  } else if (track.expandedChart) {
    detail += std::string("  ") + i18n::tr("music_player.group.chart");
  }
  return detail;
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

MusicTrackRowView::MusicTrackRowView() {
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

    trailingButton = new Button();
    trailingButton->setWidth(48)
        ->setHeight(48)
        ->setFlexShrink(0)
        ->setCornerRadius(app_theme::controlRadius());
    trailingButton->setThemedBackgroundColors(
        [] { return Color(0, 0, 0, 0); },
        [this] {
          auto color = isSelected_ ? main_menu_theme::onSelected() : main_menu_theme::ink();
          color.a = 18;
          return color;
        },
        [this] {
          auto color = isSelected_ ? main_menu_theme::onSelected() : main_menu_theme::ink();
          color.a = 34;
          return color;
        });
    trailingButton->setStyledBorderWidth(0);
    trailingText = new TextView(kFontPath, 25);
    trailingText->setText("☆");
    trailingText->setAlign(TextView::CENTER);
    trailingText->setVAlign(TextView::MIDDLE);
    trailingText->setThemedColor(main_menu_theme::secondary);
    trailingButton->setContentView(trailingText);
    addView(trailingButton);
  }

void MusicTrackRowView::setTrailingActionSize(float size) {
  trailingButton->setWidth(size)->setHeight(size);
}

void MusicTrackRowView::setTrack(const music_playlist::MusicTrack &track, bool selected,
                TrailingAction action, std::string detailOverride) {
    title->setText(trackTitle(track));
    detail->setText(detailOverride.empty() ? trackDetail(track) : std::move(detailOverride));
    highlighted_ = action.highlighted;
    trailingText->setText(std::move(action.text));
    trailingText->setThemedColor(action.highlighted ? main_menu_theme::accent
                                          : main_menu_theme::secondary);
    trailingButton->setOnClickListener(std::move(action.onClick));
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

void MusicTrackRowView::onSelected() {
    isSelected_ = true;
    trailingButton->propagateThemeChange();
    setThemedBackgroundColor(main_menu_theme::selected);
    title->setThemedColor(main_menu_theme::onSelected);
    detail->setThemedColor(main_menu_theme::selectedSecondary);
    trailingText->setThemedColor(highlighted_ ? main_menu_theme::selectedAccent
                                           : main_menu_theme::onSelected);
    artworkFallback->setThemedColor(main_menu_theme::selectedSecondary);
  }

void MusicTrackRowView::onUnselected() {
    isSelected_ = false;
    trailingButton->propagateThemeChange();
    setThemedBackgroundColor(main_menu_theme::control);
    title->setThemedColor(main_menu_theme::ink);
    detail->setThemedColor(main_menu_theme::secondary);
    trailingText->setThemedColor(highlighted_ ? main_menu_theme::accent
                                           : main_menu_theme::secondary);
    artworkFallback->setThemedColor(main_menu_theme::muted);
  }
