#include "../i18n/Localization.h"
#include "MainMenuScene.h"
#include "NewcomerTutorialView.h"
#include "ResultRecordsLoader.h"
#include "ChartRecordActions.h"
#include "RecordsIrActions.h"
#include "RecordsDiagnostics.h"
#include "MainMenuLibrary.h"
#include "../ArchiveFile.h"
#include "../BmsChartFile.h"
#include "../CourseConstraintUtils.h"
#include "../LongNoteModeUtils.h"
#include "../audio/MusicPlaylist.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../repositories/ReplayRepository.h"
#include "../ReplayAutoPlay.h"
#include "../ReplayResultStateBuilder.h"
#include "../ReplayVideoExporter.h"
#include "../ModernResultRecallBuilder.h"
#include "../PlayOptionUtils.h"
#include "../ResultContracts.h"
#include "../ProfileDatabaseActivity.h"
#include "../PlatformDocumentHandoff.h"
#include "../PlatformOpen.h"
#include "../RAII.h"
#include "../repositories/ScoreCacheQueries.h"
#include "../repositories/SqliteRAII.h"
#include "../ir/tachi/TachiBatchManual.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrSavedResultUpload.h"
#include "../ir/IrSubmissionService.h"
#include "../path.h"
#include "../replay/ChartReplayConsumer.h"
#include "../replay/CourseReplayConsumer.h"
#include "../replay/ReplayFileActionSelection.h"
#include "../replay/ReplayFileActionService.h"
#include "../view/ChartListItemView.h"
#include "../view/ChartDetailsView.h"
#include "../view/IconText.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/OverlayPortal.h"
#include "MainMenuPreviewController.h"
#include "DecideLoadingOverlay.h"
#include "../view/PlayOptionsPanelView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ModalViewHelpers.h"
#include "../view/BlockingOverlayView.h"
#include "ChartViewerScene.h"
#include "CourseGameplaySessionBuilder.h"
#include "FindBmsProgressPresentation.h"
#include "IrUploadsScene.h"
#include "MusicPlayerScene.h"
#include "RemoteResultRecallController.h"
#include "ResultScene.h"
#include "SettingsScene.h"
#include "MusicSelectScene.h"
#include "MusicSelectSkinErrorScene.h"
#include "../music_select/MusicSelectLaunchPolicy.h"
#include "../skin/GameplaySkinLifecycle.h"
#include "play/GamePlayScene.h"
#include "play/GameplayGaugeRules.h"
#include "play/Pacemaker.h"
#include "../view/ClearLampColors.h"
#include "../view/DropdownView.h"
#include "../view/ResultRecordListView.h"
#include "../view/ScrollView.h"
#include "../view/UiTheme.h"
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#include "../iOSNatives.hpp"
// define something for iphone
#include <dirent.h>
#include <sys/stat.h>
#else
// define something for OSX
#include <dirent.h>
#include <sys/stat.h>
#endif
#elif defined(__ANDROID__)
#include "../AndroidNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#elif __linux
// linux
#include <dirent.h>
#include <sys/stat.h>
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
namespace {
using modal_view::makeModalButton;
using modal_view::modalPanelBorder;
using modal_view::styleThemedActionButton;


constexpr int kRootPadding = 28;
constexpr int kLibraryPanelWidth = 320;
constexpr int kDetailsPanelWidth = 500;
constexpr int kDetailsContentWidth = 460;
// 84 design units give a 44.8-point target at 1024-point iPad width.
constexpr int kMenuActionHeight = 84;
constexpr int kPortraitMenuActionHeight = 64;
int currentMenuActionHeight() {
  return rendering::window_height > rendering::window_width
             ? kPortraitMenuActionHeight : kMenuActionHeight;
}
constexpr int kLibraryPanelPadding = 14;
constexpr int kLibraryControlWidth =
    kLibraryPanelWidth - (kLibraryPanelPadding * 2);
constexpr auto kPreviewDebounceDelay = std::chrono::milliseconds(100);
// Keep this below the modal's nominal width because row padding and the
// scrollbar gutter reduce the usable text area.
constexpr size_t kParseLogRowMaxColumns = 88;
constexpr int kParseLogRowHeight = 48;
constexpr uint32_t kIconXmark = 0xf00d;
constexpr uint32_t kIconFilter = 0xf0b0;
constexpr uint32_t kIconSort = 0xf0dc;
constexpr uint32_t kIconShare = 0xf1e0;
constexpr uint32_t kIconTrash = 0xf1f8;

std::string formatGaugeTotal(const bms_parser::ChartMeta &meta,
                             GameplayRuleset ruleset) {
  const double total = resolveEffectiveGaugeTotal(ruleset, meta);
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(2) << total;
  std::string value = stream.str();
  while (!value.empty() && value.back() == '0') value.pop_back();
  if (!value.empty() && value.back() == '.') value.pop_back();
  return value;
}

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct ClearMarkFilterDefinition {
  const char *label;
  int rank;
};

constexpr ClearMarkFilterDefinition kDifficultyClearMarkFilters[] = {
    {"FULL COMBO", kClearTypeFullComboRank},
    {"EXH-CLEAR", kClearTypeExHardClearRank},
    {"H-CLEAR", kClearTypeHardClearRank},
    {"CLEAR", kClearTypeNormalClearRank},
    {"E-CLEAR", kClearTypeEasyClearRank},
    {"LIGHT ASSIST", kClearTypeLightAssistedEasyClearRank},
    {"A-CLEAR", kClearTypeAssistedEasyClearRank},
    {"FAILED", kClearTypeFailedRank},
    {"NO PLAY", kNoClearTypeRank},
};

std::string trimAsciiWhitespace(std::string text) {
  const auto first = std::find_if_not(
      text.begin(), text.end(),
      [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto last = std::find_if_not(
      text.rbegin(), text.rend(),
      [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::optional<double> parseOptionalBpmFilter(const std::string &text) {
  const std::string trimmed = trimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  char *end = nullptr;
  const double value = std::strtod(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || end == nullptr || *end != '\0' ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return std::max(0.0, value);
}

void normalizeDifficultyFilterRange(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels) {
  chart_record_filters::normalizeDifficultyRange(filters, levels);
}

std::string longNoteModeOptionFromCourseConstraint(CourseLongNoteMode mode) {
  return long_note_mode::idFromValue(courseLongNoteModeToChartMetaValue(mode),
                                     AppSettings::kDefaultLnMode);
}

std::string clearMarkFolderKey(const std::string &parentKey, int clearRank) {
  return parentKey + ":clear:" + std::to_string(clearRank);
}

class ParseLogRowView : public View {
public:
  ParseLogRowView() : View() {
    setFlexDirection(FlexDirection::Column);
    setJustifyContent(YGJustifyCenter);
    setPadding(Edge::Left, 10);
    setPadding(Edge::Right, 14);

    label = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    label->setThemedColor(ui_theme::textSecondary);
    label->setWrap(true);
    label->setOverflow(TextView::TextOverflow::Hidden);
    label->setVAlign(TextView::MIDDLE);
    label->setFlex(1);
    addView(label);
  }

  void setRow(const MainMenuParseLogRow &row) {
    if (label != nullptr) {
      label->setText(row.text);
    }
  }

private:
  TextView *label = nullptr;
};

size_t utf8CodepointLengthAt(const std::string &text, size_t offset,
                             char32_t *codepoint = nullptr) {
  if (offset >= text.size()) {
    return 0;
  }
  const unsigned char ch = static_cast<unsigned char>(text[offset]);
  size_t length = 1;
  char32_t decoded = ch;
  if ((ch & 0x80) == 0x00) {
    length = 1;
  } else if ((ch & 0xE0) == 0xC0) {
    length = 2;
    decoded = static_cast<char32_t>(ch & 0x1F);
  } else if ((ch & 0xF0) == 0xE0) {
    length = 3;
    decoded = static_cast<char32_t>(ch & 0x0F);
  } else if ((ch & 0xF8) == 0xF0) {
    length = 4;
    decoded = static_cast<char32_t>(ch & 0x07);
  }
  if (offset + length > text.size()) {
    if (codepoint != nullptr) {
      *codepoint = ch;
    }
    return 1;
  }
  for (size_t i = 1; i < length; ++i) {
    const unsigned char continuation =
        static_cast<unsigned char>(text[offset + i]);
    if ((continuation & 0xC0) != 0x80) {
      if (codepoint != nullptr) {
        *codepoint = ch;
      }
      return 1;
    }
    decoded = (decoded << 6) | static_cast<char32_t>(continuation & 0x3F);
  }
  if (codepoint != nullptr) {
    *codepoint = decoded;
  }
  return length;
}

bool isWideLogCodepoint(char32_t codepoint) {
  return (codepoint >= 0x1100 && codepoint <= 0x115F) ||
         (codepoint >= 0x2329 && codepoint <= 0x232A) ||
         (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
         (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
         (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
         (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
         (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
         (codepoint >= 0xFFE0 && codepoint <= 0xFFE6);
}

size_t parseLogCodepointColumns(char32_t codepoint) {
  if (codepoint == '\t') {
    return 4;
  }
  if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
    return 1;
  }
  return isWideLogCodepoint(codepoint) ? 2 : 1;
}

bool isLogLineBreak(char ch) { return ch == '\n' || ch == '\r'; }

void appendParseLogRowsForLine(std::vector<MainMenuParseLogRow> &rows,
                               const std::string &line, std::uint64_t &rowId) {
  if (line.empty()) {
    rows.push_back({rowId++, " "});
    return;
  }

  size_t offset = 0;
  bool continuation = false;
  while (offset < line.size()) {
    if (isLogLineBreak(line[offset])) {
      if (line[offset] == '\r' && offset + 1 < line.size() &&
          line[offset + 1] == '\n') {
        offset += 2;
      } else {
        ++offset;
      }
      continuation = false;
      continue;
    }

    const size_t chunkStart = offset;
    size_t columns = 0;
    const size_t columnLimit =
        std::max<size_t>(1, kParseLogRowMaxColumns - (continuation ? 2 : 0));
    while (offset < line.size() && !isLogLineBreak(line[offset])) {
      char32_t codepoint = 0;
      const size_t length = utf8CodepointLengthAt(line, offset, &codepoint);
      if (length == 0) {
        break;
      }
      const size_t codepointColumns = parseLogCodepointColumns(codepoint);
      if (columns > 0 && columns + codepointColumns > columnLimit) {
        break;
      }
      offset += length;
      columns += codepointColumns;
    }

    std::string rowText = line.substr(chunkStart, offset - chunkStart);
    if (rowText.empty()) {
      rowText = " ";
    } else if (continuation) {
      rowText.insert(0, "  ");
    }
    rows.push_back({rowId++, std::move(rowText)});
    continuation = offset < line.size() && !isLogLineBreak(line[offset]);
  }
}

std::vector<MainMenuParseLogRow>
parseLogRowsFromLines(const std::vector<std::string> &lines) {
  std::vector<MainMenuParseLogRow> rows;
  if (lines.empty()) {
    rows.push_back({0, i18n::tr("menu.no_parsing_logs_yet.message")});
    return rows;
  }

  std::uint64_t rowId = 0;
  for (const std::string &line : lines) {
    appendParseLogRowsForLine(rows, line, rowId);
  }
  return rows;
}

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  insets.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  insets.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
  insets.bottom = static_cast<int>(std::lround(
      normalized.bottom * static_cast<float>(rendering::window_height)));
#endif
  return insets;
}

using main_menu_library::folderKeyForCourse;
using main_menu_library::folderKeyForCourseGroup;
using main_menu_library::folderKeyForCourseTable;
using main_menu_library::folderKeyForLevel;
using main_menu_library::folderKeyForTable;

int clearRankForGaugeType(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return kClearTypeAssistedEasyClearRank;
  case GaugeType::Easy:
    return kClearTypeEasyClearRank;
  case GaugeType::Hard:
    return kClearTypeHardClearRank;
  case GaugeType::ExHard:
    return kClearTypeExHardClearRank;
  case GaugeType::Hazard:
    return kClearTypeFullComboRank;
  case GaugeType::Normal:
  default:
    return kClearTypeNormalClearRank;
  }
}

std::string gaugeButtonLabel(GaugeType gaugeType,
                             GaugeAutoShiftMode autoShift) {
  if (gaugeAutoShiftEnabled(autoShift)) {
    return gaugeAutoShiftMenuLabel(autoShift);
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "A-EASY";
  case GaugeType::Easy:
    return "EASY";
  case GaugeType::Normal:
    return "NORMAL";
  case GaugeType::Hard:
    return "HARD";
  case GaugeType::ExHard:
    return "EX-HARD";
  case GaugeType::Hazard:
    return "HAZARD";
  default:
    return "NORMAL";
  }
}

SDL_Color readyGaugeTextColor(GaugeType gaugeType,
                              GaugeAutoShiftMode autoShift) {
  if (gaugeAutoShiftEnabled(autoShift)) {
    return SDL_Color{255, 205, 37, 255};
  }

  const Color color = clearLampColorForRank(clearRankForGaugeType(gaugeType));
  return SDL_Color{color.r, color.g, color.b, 255};
}

void styleOptionButton(Button *button, TextView *text, bool selected) {
  if (selected) {
    styleThemedActionButton(button, text, true, ui_theme::primaryAction,
                            ui_theme::primaryActionHover,
                            ui_theme::primaryActionPressed,
                            ui_theme::accentBorderStrong);
  } else {
    styleThemedActionButton(button, text, true, ui_theme::control,
                            ui_theme::controlHover, ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
  }
}

TextView *makeModalLabel(const i18n::Text &text) {
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setLocalizedText(text);
  label->setThemedColor(ui_theme::textSecondary);
  label->setHeight(28);
  return label;
}

View *makeModalOptionRow(float height = 58.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(12);
  row->setHeight(height);
  return row;
}

Button *makeModalIconButton(uint32_t iconCodepoint, int fontSize,
                            TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 54, 54);
  auto *text = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  text->setText(ui_icons::textForCodepoint(iconCodepoint));
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

const char *chartScanProgressStageText(ChartScanProgressStage stage) {
  switch (stage) {
  case ChartScanProgressStage::Preparing:
    return i18n::tr("menu.preparing_library_scan.label");
  case ChartScanProgressStage::ScanningRoots:
    return i18n::tr("menu.scanning_folders.label");
  case ChartScanProgressStage::IndexingArchives:
    return i18n::tr("menu.indexing_archives.label");
  case ChartScanProgressStage::PreparingUpdates:
    return i18n::tr("menu.preparing_chart_updates.label");
  case ChartScanProgressStage::RemovingDeleted:
    return i18n::tr("menu.removing_deleted_charts.label");
  case ChartScanProgressStage::ParsingCharts:
    return i18n::tr("menu.parsing_charts.label");
  case ChartScanProgressStage::ReadingArchive:
    return i18n::tr("menu.reading_archive_entries.label");
  }
  return i18n::tr("menu.refreshing_library.label");
}

std::string formatMusicTime(long long micros) {
  if (micros < 0) {
    return "--:--";
  }
  const long long totalSeconds = micros / 1000000LL;
  const long long minutes = totalSeconds / 60LL;
  const long long seconds = totalSeconds % 60LL;
  std::ostringstream stream;
  stream << minutes << ":" << std::setw(2) << std::setfill('0') << seconds;
  return stream.str();
}

std::string musicTrackDisplayName(const music_playlist::MusicTrack *track) {
  if (track == nullptr) {
    return i18n::tr("menu.no_track_selected.label");
  }
  std::string title = track->title.empty() ? i18n::tr("menu.untitled.label") : track->title;
  if (!track->artist.empty()) {
    title += " / " + track->artist;
  }
  return title;
}

std::string musicPlaylistTextSnapshot(
    const std::vector<music_playlist::MusicTrack> &tracks) {
  if (tracks.empty()) {
    return i18n::tr("menu.music_player.playlist.empty_summary");
  }

  constexpr std::size_t kVisibleTrackCount = 5;
  std::ostringstream text;
  text << i18n::tr("menu.music_player.my_playlist.label");
  const std::size_t visibleCount =
      std::min(kVisibleTrackCount, tracks.size());
  for (std::size_t i = 0; i < visibleCount; ++i) {
    text << "\n" << (i + 1) << ". " << musicTrackDisplayName(&tracks[i]);
  }
  if (tracks.size() > visibleCount) {
    text << "\n" << i18n::format("menu.music_player.playlist.more",
        {{"count", std::to_string(tracks.size() - visibleCount)}});
  }
  return text.str();
}

} // namespace

void MainMenuScene::ChartListPageCache::reset(
    ChartRepository::Session &chartSession, const ChartMetaQuery &chartQuery,
    int count, std::optional<ChartMetaRecord> leading) {
  ChartMetaQuery query = chartQuery;
  query.limit = 0;
  query.offset = 0;
  leadingRecord = std::move(leading);
  totalCount = std::max(0, count) + (leadingRecord.has_value() ? 1 : 0);
  pageCache.reset(static_cast<std::size_t>(std::max(0, count)),
                  [&chartSession, query](std::size_t offset, std::size_t limit) {
                    ChartMetaQuery pageQuery = query;
                    pageQuery.limit = static_cast<int>(limit);
                    pageQuery.offset = static_cast<int>(offset);
                    std::vector<ChartMetaRecord> records;
                    records.reserve(limit);
                    chartSession.QueryChartMeta(pageQuery, records);
                    return records;
                  });
}

void MainMenuScene::ChartListPageCache::releasePages() {
  pageCache.releasePages();
}

void MainMenuScene::ChartListPageCache::clear() {
  totalCount = 0;
  leadingRecord.reset();
  pageCache.clear();
}

const ChartMetaRecord &MainMenuScene::ChartListPageCache::get(int index) const {
  if (index < 0 || index >= totalCount) {
    return pageCache.get(pageCache.size());
  }
  if (leadingRecord.has_value()) {
    if (index == 0) {
      return *leadingRecord;
    }
    index--;
  }
  return pageCache.get(static_cast<std::size_t>(index));
}

EventHandleResult MainMenuScene::handleEvents(SDL_Event &event) {
  if (tutorial_ != nullptr && tutorial_->getVisible()) {
    (void)tutorial_->handleEvents(event);
    return {};
  }
  if (archiveUnzipModal_ != nullptr &&
      !archiveUnzipModal_->handleEvents(event)) {
    return {};
  }
  if (findBmsModal_ != nullptr && !findBmsModal_->handleEvents(event)) {
    return {};
  }
  // While a chart is launching, the decide overlay blocks all input so the
  // user cannot start another chart or mutate the library mid-launch.
  if (willStart.load(std::memory_order_acquire) && decideOverlay_ != nullptr &&
      decideOverlay_->getVisible()) {
    (void)decideOverlay_->handleEvents(event);
    return {};
  }
  if (recordsModal_ != nullptr && recordsModal_->isVisible()) {
    (void)recordsModal_->handleEvents(event);
    return {};
  }
#if TARGET_OS_ANDROID
  if (fileActionsModalRoot_ != nullptr && fileActionsModalRoot_->getVisible() &&
      event.type == SDL_KEYUP &&
      (event.key.keysym.sym == SDLK_ESCAPE ||
       event.key.keysym.sym == SDLK_AC_BACK)) {
    if (folderImportPanel_->getVisible()) {
      showFileActionsModal();
    } else {
      fileActionsModalRoot_->setVisible(false);
    }
    return {};
  }
#endif
  return Scene::handleEvents(event);
}

MainMenuScene::MainMenuScene(ApplicationContext &context, bool showTutorial)
    : Scene(context), showTutorial_(showTutorial) {}

MainMenuScene::~MainMenuScene() {
  stopReplayAndPreviewWork();
  if (findBmsModal_ != nullptr) findBmsModal_->cancelAndWait();
}

void MainMenuScene::stopReplayAndPreviewWork() {
  // Preparation workers can join the preview worker themselves. Join those
  // owners first, while all callbacks still have live scene dependencies.
  stopReplayLoadWorker();
  replayExportJob_.cancelAndWait();
  if (previewWorker_ != nullptr) previewWorker_->stop();
}

void MainMenuScene::init() {
  // Initialize the scene
  chartSession =
      context.chartRepository.OpenSession(&context.scoreRepository);
  auto profileOperationBlocker = [this]() -> std::optional<std::string> {
    if (replayExportJob_.inProgress()) {
      return i18n::tr("menu.replay_export_active.message");
    }
    if (archiveUnzipInProgress() ||
        (findBmsModal_ != nullptr && findBmsModal_->inProgress())) {
      return i18n::tr("menu.chart_archive_operation_active.message");
    }
    if (willStart.load(std::memory_order_acquire)) {
      return i18n::tr("menu.chart_replay_transition_active.message");
    }
    return std::nullopt;
  };
  context.profileSwitchBlockers.background = profileOperationBlocker;
  context.profileSwitchBlockers.scene = std::move(profileOperationBlocker);
  context.refreshProfileCaches = [this]() {
    // Profile switching happens while Main Menu is paused. Defer score DB
    // attachment and view work until onResume(), when the active profile is
    // fully committed and the scene owns its database dependencies again.
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    observedIrReconciliationRevision = 0;
    observedIrAccountEvidenceRevision = 0;
    replayIrObservedRevisions.clear();
  };
  initView(context);
  if (showTutorial_ || !context.applicationUiState.newcomerTutorialCompleted) {
    buildTutorial();
    showTutorial_ = false;
  }
  SDL_Log("Main Menu Scene Initialized");
}

void MainMenuScene::onPause() {
  if (replayLoadTask_.active()) {
    stopReplayLoadWorker();
    willStart.store(false);
  }
  onApplicationBackgroundChanged(true);
  if (revealContextMenu != nullptr) {
    revealContextMenu->dismiss();
  }
  if (rankingsModal) {
    rankingsModal->close();
  }
  chartListCache.releasePages();
}

void MainMenuScene::onApplicationBackgroundChanged(bool background) {
  if (background && archiveUnzipModal_ != nullptr) {
    archiveUnzipModal_->cancelAndWait();
  }
}

void MainMenuScene::onResume() {
  context.profileSwitchBlockers.scene =
      context.profileSwitchBlockers.background;
  replayIrObservedRevisions.clear();
  applyThemeChange();
  const bool scoreQueryReady = !prepareScoreQueryDatabase().has_value();
  // Gameplay selections belong to the committed profile even when its score
  // attachment is temporarily unavailable.
  reloadProfileSelectionsFromSettings();
  if (scoreQueryReady) {
    if (refreshScoreClearRankViews().has_value()) {
      scoreClearRanks = {};
      scoreBestScores = {};
      folderClearData = {};
      scoreClearRanksRevision = 0;
      refreshLongNoteModeClearRankViews();
    }
  } else {
    // Never render the previous profile's score-derived state while attachment
    // retries continue from update().
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    refreshLongNoteModeClearRankViews();
  }
  refreshLibraryIfNeeded();
  reselectCurrentChart();
  queueSelectedSkinHandoff();
}

void MainMenuScene::queueSelectedSkinHandoff() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  // Settings retains the built-in selector. Re-evaluate type 5 after resume
  // unwinds, so changing the scene cannot clean up views inside onResume().
  defer([this]() {
    if (context.gameplaySkinLifecycle &&
        !context.gameplaySkinLifecycle->presentationReady()) {
      presentationSkinRefreshPending = true;
      return true;
    }
    presentationSkinRefreshPending = false;
    skin::GameplaySkinAcquisition acquisition;
    if (context.gameplaySkinLifecycle) {
      acquisition =
          context.gameplaySkinLifecycle->acquireForSkinType(5, false);
    } else if (context.settings.presentation().skin.selectedSkinEntries.contains(5)) {
      acquisition.disposition =
          skin::GameplaySkinAcquisitionDisposition::Failed;
      acquisition.failure = skin::GameplaySkinAcquisitionFailure{
          .diagnostic = skin::SkinDiagnostic{
              .code = "skin.music_select.lifecycle_unavailable",
              .message =
                  i18n::tr("menu.selected_music_select_skin_service_unavailable.message")}};
    }
    auto decision = decideMusicSelectLaunch(std::move(acquisition));
    if (decision.kind == MusicSelectLaunchKind::SelectedSkin &&
        decision.request) {
      context.sceneManager->changeScene(std::make_unique<MusicSelectScene>(
          context, std::move(*decision.request)));
      return false;
    }
    if (decision.kind == MusicSelectLaunchKind::Error) {
      context.sceneManager->changeScene(
          std::make_unique<MusicSelectSkinErrorScene>(
              context, std::move(decision.selectedSkinPath),
              std::move(decision.diagnostics)));
      return false;
    }
    return true;
  }, 0, true);
#endif
}

void MainMenuScene::onPresentationOrientationChanged() {
  updatePanelLayout();
  presentationSkinRefreshPending = true;
}

void MainMenuScene::onLanguageChanged() {
  Scene::onLanguageChanged();
  // Rebind presentation only: reinitializing the scene would cancel workers,
  // discard selection, and replace the user's filter and lane-order input.
  if (folderRecyclerView != nullptr) folderRecyclerView->rebindVisibleItems();
  if (recyclerView != nullptr) recyclerView->rebindVisibleItems();
  refreshReadySettingsSummary();
  refreshTasksButton();
  refreshTasksModal(true);
  refreshMusicModal();
  refreshFindBmsModal(false);
  if (rootLayout != nullptr) rootLayout->applyYogaLayout();
}

void MainMenuScene::reloadProfileSelectionsFromSettings() {
  const std::string previousLongNoteMode = profileSelections.longNoteMode;
  const bool refreshLongNoteQueries =
      profileSelectionsInitialized &&
      previousLongNoteMode != context.settings.selectedLnMode;
  profileSelections.reload(context.settings);
  profileSelectionsInitialized = true;

  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshPacemakerTargetButtons();
  refreshSelectedChartActionState();
  if (refreshLongNoteQueries) {
    refreshLongNoteModeClearRankViews();
  }
}

void MainMenuScene::applyThemeChange() {
  const ui_theme::ThemeMode activeMode = ui_theme::activeMode();
  if (appliedUiThemeMode == activeMode) {
    return;
  }

  appliedUiThemeMode = activeMode;
  for (auto *view : views) {
    if (view != nullptr) {
      view->propagateThemeChange();
    }
  }
  if (revealContextMenu != nullptr && !revealContextMenu->isOpen()) {
    revealContextMenu->propagateThemeChange();
  }
  if (folderRecyclerView != nullptr) {
    folderRecyclerView->rebindVisibleItems();
  }
  if (recyclerView != nullptr) {
    recyclerView->rebindVisibleItems();
  }
  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshAssistOptionButtons();
  refreshChartFilterButtons();
  if (recordsModal_ != nullptr) recordsModal_->refresh();
  refreshFindBmsModal();
  refreshMusicModal();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::enqueueLibraryRefreshTask(
    const i18n::Text &title, const std::filesystem::path &folderToAdd,
    const std::string &iosBookmark, bool rebuildLibraryMetadata) {
  if (!context.chartLibraryTasks) {
    return;
  }
  context.chartLibraryTasks->enqueue({
      .kind = chart_library_tasks::TaskKind::RefreshLibrary,
      .title = title,
      .folderToAdd = folderToAdd,
      .iosBookmark = iosBookmark,
      .rebuildLibraryMetadata = rebuildLibraryMetadata,
  });
}

void MainMenuScene::enqueueDownloadedPathIndexTask(
    const std::filesystem::path &path,
    const main_menu_library::FindBmsChartIdentity &targetIdentity,
    std::uint64_t selectionGeneration,
    std::vector<std::filesystem::path> removedPaths) {
  if (path.empty() || !context.chartLibraryTasks) {
    return;
  }
  context.chartLibraryTasks->enqueue({
      .kind = chart_library_tasks::TaskKind::IndexDownloadedPath,
      .title = i18n::message("menu.index_downloaded_bms.label"),
      .downloadedPath = path,
      .downloadedRemovedPaths = std::move(removedPaths),
      .downloadedTargetIdentity = targetIdentity,
      .downloadedSelectionGeneration = selectionGeneration,
  });
}

MainMenuScene::LibraryTaskProgressSnapshot
MainMenuScene::readLibraryTaskProgress() const {
  return context.chartLibraryTasks
             ? context.chartLibraryTasks->snapshot().progress
             : LibraryTaskProgressSnapshot{};
}

int MainMenuScene::activeLibraryTaskCount() {
  return context.chartLibraryTasks
             ? context.chartLibraryTasks->snapshot().activeCount
             : 0;
}

void MainMenuScene::requestLibraryScanFlush() {
  if (activeLibraryTaskCount() > 0) {
    context.chartLibraryScanFlushRequested.fetch_add(
        1, std::memory_order_release);
  }
  requestLibraryReload(true);
}

void MainMenuScene::refreshTasksButton() {
  if (tasksButtonText == nullptr) {
    return;
  }
  const int count = activeLibraryTaskCount();
  tasksButtonText->setLocalizedText(i18n::message(
      count == 1 ? "menu.task_count.one" : "menu.task_count.other",
      {{"count", std::to_string(count)}}));
}

void MainMenuScene::initView(ApplicationContext &context) {
  tutorial_ = nullptr;
  addFolderButton_ = nullptr;
  tutorialRightScroll_ = nullptr;
  detailsContent_ = nullptr;
  detailsControlsContent_ = nullptr;
  detailsControlsScroll_ = nullptr;
  findBmsAvailableWithoutTutorial_ = false;
  archiveUnzipModal_.reset();
  findBmsModal_.reset();
  // Initialize the view
  revealContextMenu.reset();
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  overlayPortal = nullptr;
  revealButton = nullptr;
  temporaryChartFolder.reset();
  jacketView = nullptr;
  chartDetailsView_ = nullptr;
  searchBox = nullptr;
  chartFilterPanel = nullptr;
  chartSortPanel = nullptr;
  selectedChartRecord.reset();
  chartFilterButton = nullptr;
  chartFilterButtonText = nullptr;
  chartSortButton = nullptr;
  chartSortButtonText = nullptr;
  startButton = nullptr;
  chartActionsRow = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  findBmsButtonSlot = nullptr;
  findBmsButton = nullptr;
  findBmsButtonText = nullptr;
  unzipButtonSlot = nullptr;
  unzipButton = nullptr;
  unzipButtonText = nullptr;
  parseLogButton = nullptr;
  parseLogButtonText = nullptr;
  musicButton = nullptr;
  musicButtonText = nullptr;
  irUploadsButton = nullptr;
  irUploadsButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  recordsModal_.reset();
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  playOptionsPanel = nullptr;
  playOptionsModal.reset();
  musicModalRoot = nullptr;
  parseLogModalRoot = nullptr;
  tasksModalRoot = nullptr;
#if TARGET_OS_ANDROID
  fileActionsModalRoot_ = nullptr;
  fileActionsPanel_ = nullptr;
  folderImportPanel_ = nullptr;
  computerImportPanel_ = nullptr;
#endif
  parseLogRecyclerView = nullptr;
  parseLogExportStatusText = nullptr;
  parseLogExportButton = nullptr;
  parseLogExportButtonText = nullptr;
  parseLogCloseButton = nullptr;
  parseLogCloseButtonText = nullptr;
  musicTrackText = nullptr;
  musicStatusText = nullptr;
  musicPlaylistText = nullptr;
  musicSelectedButton = nullptr;
  musicAddSelectedButton = nullptr;
  musicRemoveSelectedButton = nullptr;
  musicPlaylistButton = nullptr;
  musicClearPlaylistButton = nullptr;
  musicRandomButton = nullptr;
  musicPreviousButton = nullptr;
  musicSeekBackwardButton = nullptr;
  musicPlayPauseButton = nullptr;
  musicSeekForwardButton = nullptr;
  musicNextButton = nullptr;
  musicStopButton = nullptr;
  musicCloseButton = nullptr;
  musicSelectedButtonText = nullptr;
  musicAddSelectedButtonText = nullptr;
  musicRemoveSelectedButtonText = nullptr;
  musicPlaylistButtonText = nullptr;
  musicClearPlaylistButtonText = nullptr;
  musicRandomButtonText = nullptr;
  musicPreviousButtonText = nullptr;
  musicSeekBackwardButtonText = nullptr;
  musicPlayPauseButtonText = nullptr;
  musicSeekForwardButtonText = nullptr;
  musicNextButtonText = nullptr;
  musicStopButtonText = nullptr;
  musicCloseButtonText = nullptr;
  tasksScrollView = nullptr;
  tasksContent = nullptr;
  tasksText = nullptr;
  tasksRefreshButton = nullptr;
  tasksRefreshButtonText = nullptr;
  tasksCloseButton = nullptr;
  tasksCloseButtonText = nullptr;
  readyGaugeText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  readyPacemakerText = nullptr;
  readyPlayOptionsButton = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayExportJob_.reset();
  pendingSelectChartPath.reset();
  {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    pendingFindBmsSelectionHandoff.reset();
  }
  suppressPreviewForChartPath.reset();
  chartSelectionGeneration = 0;
  findBmsSelectionGenerationAtDownloadStart = 0;
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
  replayIrObservedRevisions.clear();
  tasksModalOpenRequested = false;
  musicStatusMessage = {};
  chartRecordFilters = {};
  chartFilterPanelVisible = false;
  chartSortPanelVisible = false;
  chartBpmMinText.clear();
  chartBpmMaxText.clear();
  chartClearMarkDropdownOpen = false;
  chartScoreRankDropdownOpen = false;
  chartDifficultyMinDropdownOpen = false;
  chartDifficultyMaxDropdownOpen = false;
  publishedResultRecordDiagnostic.clear();

  appliedUiThemeMode = ui_theme::activeMode();

  recyclerView = new RecyclerView<ChartMetaRecord>(
      [](const ChartMetaRecord &a, const ChartMetaRecord &b) {
        return a.meta.SHA256 == b.meta.SHA256 && a.meta.MD5 == b.meta.MD5 &&
               a.meta.BmsPath == b.meta.BmsPath &&
               a.difficultyTableLabels == b.difficultyTableLabels &&
               a.courseStart == b.courseStart &&
               a.unzipAll == b.unzipAll &&
               a.unavailable == b.unavailable &&
               a.solidArchive == b.solidArchive &&
               a.archiveSize == b.archiveSize &&
               a.archiveUncompressedSize == b.archiveUncompressedSize &&
               a.archiveFileCount == b.archiveFileCount &&
               a.favorite == b.favorite;
      });
  folderRecyclerView = new RecyclerView<LibraryFolderItem>(
      [](const LibraryFolderItem &a, const LibraryFolderItem &b) {
        return a.key == b.key;
      });
  if (!chartSession.has_value()) {
    SDL_Log("Chart repository session is unavailable");
    return;
  }
  chartSession->EnsureSchema();
  chart_library_platform::refreshFolderAccess(
      chartSession->SelectEffectiveEntries());

  static constexpr int kChartListItemHeight = 108;
  recyclerView->onCreateView = [this](const ChartMetaRecord &item) {
    return new ChartListItemView(0, 0, rendering::window_width,
                                 kChartListItemHeight, item);
  };
  recyclerView->itemHeight = kChartListItemHeight;
  recyclerView->topMargin = 8;
  recyclerView->bottomMargin = 8;
  recyclerView->onBind = [this](View *view, const ChartMetaRecord &item,
                                int idx, bool isSelected) {
    auto *chartListItemView = dynamic_cast<ChartListItemView *>(view);
    chartListItemView->setMeta(item, prioritizeVisibleArtworkBindings);
    chartListItemView->setClearRank(clearRankForChart(item));
    if (!item.courseStart && !item.solidArchive && !item.unavailable &&
        !item.meta.BmsPath.empty()) {
      const auto bestScore = scoreBestScores.bestFor(
          item.meta,
          long_note_mode::valueFromId(profileSelections.longNoteMode));
      if (bestScore.has_value()) {
        const int fallbackMaxScore =
            result_contract::maximumScoreForNotes(item.meta.TotalNotes)
                .value_or(0);
        chartListItemView->setBestScoreRank(
            bestScore->score,
            bestScore->maxScore > 0 ? bestScore->maxScore : fallbackMaxScore);
      } else {
        chartListItemView->setBestScoreRank(0, 0);
      }
    } else {
      chartListItemView->setBestScoreRank(0, 0);
    }
    chartListItemView->setFavoriteToggleHandler(
        [this](const ChartMetaRecord &record, bool favorite) {
          return toggleChartFavorite(record, favorite);
        });
    if (isSelected) {
      chartListItemView->onSelected();
    } else {
      chartListItemView->onUnselected();
    }
  };

  jacketView = new ImageView(0, 0, 0, 0);
  recyclerView->onSelected = [this, &context](const ChartMetaRecord &record,
                                              int idx) {
    if (willStart.load())
      return;
    const ChartMetaRecord item = record;
    chartSelectionGeneration =
        main_menu_library::chartSelectionGenerationAfter(
            chartSelectionGeneration, selectedChartRecord, item);
    selectedChartRecord = item;
    refreshRankingsButton();
    const auto &meta = item.meta;
    auto selectedView = recyclerView->getViewByIndex(idx);
    if (selectedView) {
      selectedView->onSelected();
    }
    refreshReplayAvailability(&item);
    refreshPlayOptionButtons();
    refreshLongNoteModeButtons();
    refreshAssistOptionButtons();
    if (item.courseStart) {
      setPlayableChartActionsVisible(true, false);
      refreshUnzipButtonForSelection(nullptr);
      setFindBmsButtonVisible(false);
      if (previewWorker_ != nullptr) {
        previewWorker_->cancelAndReleaseWhenIdle();
      }
      clearSelectedChart();
      jacketView->freeImage();
      refreshStartButtonForActiveFolder();
      return;
    }
    setPlayableChartActionsVisible(!item.unavailable && !item.solidArchive &&
                                   !meta.BmsPath.empty());
    refreshUnzipButtonForSelection(&item);
    setFindBmsButtonVisible(
        item.unavailable && !item.solidArchive &&
        (!meta.SHA256.empty() || !meta.MD5.empty() || !meta.Title.empty()));
    refreshStartButtonForActiveFolder();
    if (previewWorker_ != nullptr) {
      previewWorker_->cancelAndReleaseWhenIdle();
    }
    clearSelectedChart();
    if (item.unavailable || meta.BmsPath.empty()) {
      jacketView->freeImage();
      return;
    }
    if (item.solidArchive) {
      jacketView->freeImage();
      archive_file::appendDebugLogLine(
          "Solid archive selected without chart probing: " +
          fspath_to_utf8(meta.BmsPath) +
          " files=" + std::to_string(item.archiveFileCount) +
          " estimatedUnpacked=" + std::to_string(item.archiveUncompressedSize));
      return;
    }
    bool archiveVirtualPath = archive_file::isVirtualPath(meta.BmsPath);
#if TARGET_OS_ANDROID
    if (archiveVirtualPath && IsAndroidTreePath(meta.BmsPath)) {
      archiveVirtualPath = false;
    }
#endif
    if (archiveVirtualPath && !context.settings.archiveChartPreviewEnabled) {
      jacketView->freeImage();
      archive_file::appendDebugLogLine(
          "Preview skipped by archive chart preview setting: " +
          fspath_to_utf8(meta.BmsPath));
      return;
    }
    bool suppressPreview = false;
    if (suppressPreviewForChartPath.has_value()) {
      const path_t suppressPath =
          fspath_to_path_t(suppressPreviewForChartPath.value());
      suppressPreview = suppressPath == fspath_to_path_t(meta.BmsPath);
    }
    if (suppressPreview) {
      suppressPreviewForChartPath.reset();
    }
    if (!meta.StageFile.empty()) {
      jacketView->setImageAsync(meta.Folder / meta.StageFile, true);
    } else {
      jacketView->freeImage();
    }
    if (suppressPreview) {
      archive_file::appendDebugLogLine(
          "Preview suppressed for auto-selected unzipped chart: " +
          fspath_to_utf8(meta.BmsPath));
      return;
    }
    std::string musicStopError;
    context.musicPlayer.Stop(musicStopError);
    if (previewWorker_ != nullptr) {
      ChartMetaRecord previewRecord;
      previewRecord.meta = std::move(meta);
      previewWorker_->request(std::move(previewRecord));
    }
  };
  recyclerView->onUnselected = [this](const ChartMetaRecord &item, int idx) {
    auto unselectedView = recyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  static constexpr int kFolderListItemHeight = 50;
  folderRecyclerView->onCreateView = [](const LibraryFolderItem &item) {
    return new LibraryFolderItemView(0, 0, kLibraryControlWidth,
                                     kFolderListItemHeight);
  };
  folderRecyclerView->itemHeight = kFolderListItemHeight;
  folderRecyclerView->onBind = [this](View *view, const LibraryFolderItem &item,
                                      int idx, bool isSelected) {
    auto *folderView = dynamic_cast<LibraryFolderItemView *>(view);
    if (folderView != nullptr) {
      // Folder metadata stays raw; only application-owned folder kinds have
      // translated labels. Rebinding preserves the list's selection and scroll.
      std::string label = item.label;
      switch (item.type) {
      case LibraryFolderItem::Type::AllSongs:
        label = i18n::tr("library.folders.all_songs.label");
        break;
      case LibraryFolderItem::Type::Favorites:
        label = i18n::tr("library.folders.favorites.label");
        break;
      case LibraryFolderItem::Type::SolidArchives:
        label = i18n::tr("library.folders.solid_archive.label");
        break;
      case LibraryFolderItem::Type::CoursesRoot:
        label = i18n::tr("library.folders.courses.label");
        break;
      case LibraryFolderItem::Type::CourseGroup:
        if (item.courseGroupName.empty()) {
          label = i18n::tr("library.folders.ungrouped.label");
        }
        break;
      default:
        break;
      }
      folderView->setItem(label, item.depth, item.count, isSelected,
                          item.clearMarkFolder ? item.clearMarkRank
                                               : clearRankForFolder(item.key),
                          item.clearMarkFolder, item.expandable,
                          item.expanded);
    }
  };
  folderRecyclerView->onSelected = [this](const LibraryFolderItem &item,
                                          int idx) {
    auto selectedView = folderRecyclerView->getViewByIndex(idx);
    if (selectedView) {
      selectedView->onSelected();
    }
    selectFolder(item);
  };
  folderRecyclerView->onUnselected = [this](const LibraryFolderItem &item,
                                            int idx) {
    auto unselectedView = folderRecyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  rootLayout->setFlexDirection(FlexDirection::Row);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setGap(24);
  rootLayout->setPadding(Edge::Top, safe.top + kRootPadding);
  rootLayout->setPadding(Edge::Left, safe.left + kRootPadding);
  rootLayout->setPadding(Edge::Right, safe.right + kRootPadding);
  rootLayout->setPadding(Edge::Bottom, safe.bottom + kRootPadding);
  rootLayout->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  auto *browser = new View();
  browser->setName("mainMenuBrowser");
  browser->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignStretch);
  browser->setFlex(1)->setMinWidth(0)->setMinHeight(0)->setGap(24);
  rootLayout->addView(browser);

overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                     rendering::window_height);
  overlayPortal->setPositionType(YGPositionTypeAbsolute);
  overlayPortal->setPosition(Edge::Left, 0);
  overlayPortal->setPosition(Edge::Top, 0);
  overlayPortal->setZIndex(2000);
  decideOverlay_ = new DecideLoadingOverlay(
      0, 0, rendering::window_width, rendering::window_height, {});
  decideOverlay_->setVisible(false);
  overlayPortal->present(decideOverlay_);
  previewWorker_ = std::make_unique<MainMenuPreviewController>(
      [this](const ChartMetaRecord &request, std::atomic_bool &cancelled) {
        const auto &meta = request.meta;
        const auto isCancelled = [&cancelled, this, &meta]() {
          return cancelled.load(std::memory_order_relaxed) ||
                 previewWorker_->superseded(
                     fspath_to_utf8(meta.BmsPath));
        };
        SDL_Log("Previewing %s", fspath_to_utf8(meta.BmsPath).c_str());
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          if (isCancelled()) {
            return;
          }
          this->context.jukebox.stop();
        }
        SDL_Log("Parsing %s", fspath_to_utf8(meta.BmsPath).c_str());
        std::unique_ptr<bms_parser::Chart> chart;
        try {
          chart = play_options::parseChart(meta.BmsPath, cancelled, "preview");
        } catch (const std::exception &e) {
          SDL_Log("Preview parse failed %s: %s",
                  fspath_to_utf8(meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Preview parse exception: " + fspath_to_utf8(meta.BmsPath) +
              ": " + e.what());
          return;
        }
        if (isCancelled()) {
          return;
        }
        SDL_Log("Parsed %s", fspath_to_utf8(meta.BmsPath).c_str());
        if (chart == nullptr) {
          SDL_Log("Chart is null");
          archive_file::appendDebugLogLine("Preview chart is null: " +
                                           fspath_to_utf8(meta.BmsPath));
          return;
        }
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          if (isCancelled()) {
            return;
          }
          if (this->context.jukebox.hasLoadedResources()) {
            this->context.jukebox.reloadChartResources(*chart, true, cancelled);
          } else {
            this->context.jukebox.loadChart(*chart, true, cancelled);
          }
          if (isCancelled()) {
            return;
          }
          setSelectedChart(std::move(chart), true);
          if (isCancelled()) {
            clearSelectedChart();
            return;
          }
          if (!willStart.load()) {
            this->context.jukebox.play();
          }
        }
      }, [this] { stopAndClearSelectedChart(); }, kPreviewDebounceDelay);

  auto nav = new View();
  nav->setName("mainMenuLibrary");
  nav->setFlexDirection(FlexDirection::Column);
  nav->setAlignItems(YGAlignStretch);
  nav->setWidth(kLibraryPanelWidth);
  nav->setGap(12);
  nav->setPadding(Edge::All, kLibraryPanelPadding);
  nav->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  nav->setCornerRadius(ui_theme::panelRadius());
  nav->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  nav->setThemedBorderColor(ui_theme::hairline);
  nav->setBorderWidth(1);

  auto *libraryActions = new View();
  libraryActions->setName("mainMenuLibraryActions");
  libraryActions->setFlexDirection(FlexDirection::Column)->setGap(12);
  libraryActions->setFlexShrink(0);
  nav->addView(libraryActions);

  auto *addFolderButton = new Button(0, 0, kLibraryControlWidth, kMenuActionHeight);
  addFolderButton_ = addFolderButton;
  auto *addFolderText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
#if TARGET_OS_ANDROID
  addFolderText->setLocalizedText(i18n::message("menu.manage_files.label"));
#else
  addFolderText->setLocalizedText(i18n::message("menu.add_folder.label"));
#endif
  addFolderText->setAlign(TextView::CENTER);
  addFolderText->setVAlign(TextView::MIDDLE);
  addFolderButton->setContentView(addFolderText);
  styleThemedActionButton(addFolderButton, addFolderText, true,
                          ui_theme::primaryAction,
                          ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  addFolderButton->setCornerRadius(ui_theme::controlRadius());
  addFolderButton->setStyledBorderWidth(1);
  addFolderButton->setOnClickListener([this]() {
#if TARGET_OS_ANDROID
    showFileActionsModal();
#else
    if (this->context.requestAddChartFolderFromFiles) {
      this->context.requestAddChartFolderFromFiles();
    }
#endif
  });
  libraryActions->addView(addFolderButton);

  folderRecyclerView->setFlex(1);
  folderRecyclerView->clearBackgroundColor();
  folderRecyclerView->setBorderWidth(0);
  folderRecyclerView->setCornerRadius(ui_theme::controlRadius());
  nav->addView(folderRecyclerView);
  browser->addView(nav);

  auto left = new View();
  left->setName("mainMenuSongs");
  left->setMinWidth(0)->setMinHeight(0);
  left->setFlexDirection(FlexDirection::Column);
  left->setAlignItems(YGAlignStretch);
  left->setFlex(1);
  left->setGap(14);
  left->setPadding(Edge::All, 16);
  left->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  left->setCornerRadius(ui_theme::panelRadius());
  left->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  left->setThemedBorderColor(ui_theme::hairline);
  left->setBorderWidth(1);

  auto *libraryHeader = new View();
  libraryHeader->setName("mainMenuToolbar");
  libraryHeader->setFlexDirection(FlexDirection::Row);
  libraryHeader->setAlignItems(YGAlignCenter);
  libraryHeader->setGap(12);
  libraryHeader->setHeight(kMenuActionHeight);

  parseLogButton = makeModalButton(i18n::message("menu.log.label"), 20, &parseLogButtonText);
  parseLogButton->setWidth(112);
  parseLogButton->setHeight(kMenuActionHeight);
  parseLogButton->setOnClickListener([this]() { showParseLogModal(); });
  styleThemedActionButton(parseLogButton, parseLogButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  libraryHeader->addView(parseLogButton);

  musicButton = makeModalButton(i18n::message("menu.music.label"), 20, &musicButtonText);
  musicButton->setWidth(122);
  musicButton->setHeight(kMenuActionHeight);
  musicButton->setOnClickListener([this, &context]() {
    if (context.sceneManager != nullptr) {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      stopAndClearSelectedChart();
      context.sceneManager->changeScene(
          std::make_unique<MusicPlayerScene>(
              context, SceneReturnTarget::Retained(this)),
          true);
    }
  });
  styleThemedActionButton(musicButton, musicButtonText, true, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(musicButton);

  irUploadsButton =
      makeModalButton(i18n::message("menu.ir_uploads.label"), 20, &irUploadsButtonText);
  irUploadsButton->setWidth(154);
  irUploadsButton->setHeight(kMenuActionHeight);
  irUploadsButton->setOnClickListener([this, &context]() {
    if (context.sceneManager != nullptr) {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      stopAndClearSelectedChart();
      context.sceneManager->changeScene(
          std::make_unique<IrUploadsScene>(
              context, SceneReturnTarget::Retained(this)),
          true);
    }
  });
  styleThemedActionButton(irUploadsButton, irUploadsButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(irUploadsButton);

  tasksButton = makeModalButton(i18n::message("menu.task_count.empty"), 20, &tasksButtonText);
  tasksButton->setWidth(142);
  tasksButton->setHeight(kMenuActionHeight);
  tasksButton->setOnClickListener([this]() { showTasksModal(); });
  styleThemedActionButton(tasksButton, tasksButtonText, true, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(tasksButton);
  left->addView(libraryHeader);

  auto *filterRow = new View();
  filterRow->setFlexDirection(FlexDirection::Row);
  filterRow->setAlignItems(YGAlignStretch);
  filterRow->setGap(10);

  searchBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  searchBox->setEditingText(searchText);
  searchBox->setClearable(true);
  searchBox->setHeight(kMenuActionHeight);
  searchBox->setFlex(1);
  searchBox->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  searchBox->setCornerRadius(ui_theme::controlRadius());
  searchBox->setThemedBorderColor(ui_theme::hairlineSubtle);
  searchBox->setBorderWidth(1);
  searchBox->setVAlign(TextView::MIDDLE);
  searchBox->setThemedColor(ui_theme::textPrimary);
  auto onSearchChanged = [this](const std::string &text) {
    searchText = text;
    reloadChartList();
  };
  searchBox->onTextChanged(onSearchChanged);
  searchBox->onSubmit(onSearchChanged);
  filterRow->addView(searchBox);

  chartFilterButton =
      makeModalIconButton(kIconFilter, 20, &chartFilterButtonText);
  chartFilterButton->setWidth(kMenuActionHeight);
  chartFilterButton->setHeight(kMenuActionHeight);
  chartFilterButton->setFlexShrink(0.0f);
  chartFilterButton->setOnClickListener([this]() {
    setChartFilterPanelVisible(!chartFilterPanelVisible);
  });
  filterRow->addView(chartFilterButton);

  chartSortButton =
      makeModalIconButton(kIconSort, 20, &chartSortButtonText);
  chartSortButton->setWidth(kMenuActionHeight);
  chartSortButton->setHeight(kMenuActionHeight);
  chartSortButton->setFlexShrink(0.0f);
  chartSortButton->setOnClickListener([this]() {
    setChartSortPanelVisible(!chartSortPanelVisible);
  });
  filterRow->addView(chartSortButton);

  auto *filterLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  filterLabel->setLocalizedText(i18n::message("menu.search.label"));
  filterLabel->setThemedColor(ui_theme::textSecondary);
  left->addView(filterLabel);
  left->addView(filterRow);

  chartFilterPanel = new ChartFilterPanelView({
      .onClearMarkChanged =
          [this](std::optional<int> rank) { setChartClearFilter(rank); },
      .onScoreRankChanged = [this](std::optional<std::string> rank) {
        setChartScoreRankFilter(std::move(rank));
      },
      .onBpmMinChanged =
          [this](const std::string &text) { setChartBpmMinFilter(text); },
      .onBpmMaxChanged =
          [this](const std::string &text) { setChartBpmMaxFilter(text); },
      .onDifficultyMinChanged = [this](std::optional<std::string> level) {
        setChartDifficultyMinFilter(std::move(level));
      },
      .onDifficultyMaxChanged = [this](std::optional<std::string> level) {
        setChartDifficultyMaxFilter(std::move(level));
      },
      .onClearMarkDropdownChanged = [this](bool open) {
        setChartClearMarkDropdownOpen(open);
      },
      .onScoreRankDropdownChanged = [this](bool open) {
        setChartScoreRankDropdownOpen(open);
      },
      .onClearMarkRangeChanged = [this](bool orAbove, bool orBelow) {
        setChartClearMarkRange(orAbove, orBelow);
      },
      .onScoreRankRangeChanged = [this](bool orAbove, bool orBelow) {
        setChartScoreRankRange(orAbove, orBelow);
      },
      .onDifficultyDropdownChanged = [this](bool minLevel, bool open) {
        setChartDifficultyDropdownOpen(minLevel, open);
      },
  });

  chartSortPanel = new ChartSortPanelView({
      .onSortChanged = [this](ChartRecordSortCriterion criterion) {
        setChartSortCriterion(criterion);
      },
  });

  left->addView(chartFilterPanel);
  left->addView(chartSortPanel);
  refreshChartFilterPanel();

  recyclerView->setFlex(1);
  recyclerView->clearBackgroundColor();
  recyclerView->setBorderWidth(0);
  recyclerView->setCornerRadius(ui_theme::controlRadius());
  left->addView(recyclerView);
  browser->addView(left);

  auto right = new View();
  right->setName("mainMenuDetails");
  right->setFlexDirection(FlexDirection::Column);
  right->setAlignItems(YGAlignCenter);
  right->setWidth(kDetailsPanelWidth);
  right->setFlexShrink(0);
  right->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  right->setCornerRadius(ui_theme::panelRadius());
  right->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  right->setThemedBorderColor(ui_theme::hairline);
  right->setBorderWidth(1);
  right->setGap(12);
  right->setPadding(Edge::Bottom, 16);

  auto *rightScroll = new ScrollView();
  rightScroll->setName("mainMenuDetailsScroll");
  rightScroll->setMinWidth(0)->setMinHeight(0);
  tutorialRightScroll_ = rightScroll;
  rightScroll->setWidth(kDetailsPanelWidth - 20);
  rightScroll->setFlex(1);
  rightScroll->setFlexShrink(1);
  rightScroll->clearBackgroundColor();
  auto *rightContent = new View();
  detailsContent_ = rightContent;
  rightContent->setWidth(kDetailsPanelWidth - 22);
  rightContent->setFlexDirection(FlexDirection::Column);
  rightContent->setAlignItems(YGAlignCenter);
  rightContent->setPadding(Edge::Top, 16);
  rightContent->setPadding(Edge::Bottom, 16);
  rightContent->setPadding(Edge::Left, 9);
  rightContent->setPadding(Edge::Right, 9);
  rightContent->setGap(10);

  chartDetailsView_ = new ChartDetailsView(jacketView);
  chartDetailsView_->setWidth(kDetailsContentWidth);
  rightContent->addView(chartDetailsView_);

  auto *readySettings = new View();
  readySettings->setFlexDirection(FlexDirection::Column);
  readySettings->setAlignItems(YGAlignStretch);
  readySettings->setPadding(Edge::Top, 10);
  readySettings->setPadding(Edge::Bottom, 10);
  readySettings->setPadding(Edge::Left, 12);
  readySettings->setPadding(Edge::Right, 12);
  readySettings->setGap(6);

  auto makeReadyStatusText = []() {
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    text->setHeight(28);
    text->setThemedColor(ui_theme::textPrimary);
    text->setOverflow(TextView::TextOverflow::Marquee);
    return text;
  };
  auto *readyGaugeRow = new View();
  readyGaugeRow->setFlexDirection(FlexDirection::Row);
  readyGaugeRow->setAlignItems(YGAlignCenter);
  readyGaugeRow->setGap(6);
  readyGaugeRow->setHeight(28);
  auto *readyGaugeLabelText = makeReadyStatusText();
  readyGaugeLabelText->setLocalizedText(i18n::message("menu.gauge.label"));
  readyGaugeLabelText->setThemedColor(ui_theme::textSecondary);
  readyGaugeLabelText->setWidth(70);
  readyGaugeText = makeReadyStatusText();
  readyGaugeText->setFlex(1);
  readyGaugeRow->addView(readyGaugeLabelText);
  readyGaugeRow->addView(readyGaugeText);
  readyPlayOptionText = makeReadyStatusText();
  readyAssistOptionText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  readyAssistOptionText->setHeight(28);
  readyAssistOptionText->setThemedColor(ui_theme::textPrimary);
  readyAssistOptionText->setOverflow(TextView::TextOverflow::Hidden);
  readyPacemakerText = makeReadyStatusText();
  auto *readyStatusRow = new View();
  readyStatusRow->setFlexDirection(FlexDirection::Row)->setGap(12);
  readyStatusRow->setHeight(28);
  readyGaugeRow->setFlex(1)->setMinWidth(0);
  readyPacemakerText->setFlex(1)->setMinWidth(0);
  readyPacemakerText->setAlign(TextView::RIGHT);
  readyStatusRow->addView(readyGaugeRow);
  readyStatusRow->addView(readyPacemakerText);
  readySettings->addView(readyStatusRow);
  readySettings->addView(readyPlayOptionText);
  readySettings->addView(readyAssistOptionText);

  readyPlayOptionsButton = new Button(0, 0, kDetailsContentWidth, 122);
  readyPlayOptionsButton->setWidth(kDetailsContentWidth);
  readyPlayOptionsButton->setHeight(122);
  readyPlayOptionsButton->setFlexShrink(0);
  readyPlayOptionsButton->setCornerRadius(ui_theme::controlRadius());
  readyPlayOptionsButton->setStyledBorderWidth(1);
  readyPlayOptionsButton->setThemedBackgroundColors(
      ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed);
  readyPlayOptionsButton->setThemedBorderColors(
      ui_theme::hairlineStrong, ui_theme::accentBorderStrong,
      ui_theme::accentBorderStrong);
  readyPlayOptionsButton->setContentView(readySettings);
  readyPlayOptionsButton->setOnClickListener(
      [this]() { showPlayOptionsModal(); });
  rightContent->addView(readyPlayOptionsButton);
  refreshPlaybackSelectionControls();

  startButton = new Button(0, 0, kDetailsContentWidth, 88);
  startButton->setName("mainMenuStart");
  startButton->setFlexShrink(0);
  auto buttonText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  startButtonText = buttonText;
  buttonText->setLocalizedText(i18n::message("menu.start.label"));
  buttonText->setAlign(TextView::CENTER);
  buttonText->setVAlign(TextView::MIDDLE);
  startButton->setContentView(buttonText);
  styleThemedActionButton(startButton, buttonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  startButton->setOnClickListener([this]() {
    if (willStart.load()) {
      return;
    }
    const auto selectedRecord = selectedRecordSnapshot();
    if (activeFolder.type == LibraryFolderItem::Type::Course &&
        (!selectedRecord.has_value() || selectedRecord->courseStart)) {
      startSelectedCourse();
      return;
    }
    if (!selectedRecord.has_value() || selectedRecord->solidArchive ||
        selectedRecord->unavailable || selectedRecord->meta.BmsPath.empty()) {
      return;
    }
    startSelectedChart();
  });
  replayButtonSlot = new View();
  replayButtonSlot->setFlex(1)->setMinWidth(0)->setHeight(kMenuActionHeight);
  replayButtonSlot->setAlignItems(YGAlignStretch);

  replayButton = new Button(0, 0, 224, kMenuActionHeight);
  replayButton->setWidthPercent(100);
  replayButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  replayButtonText->setLocalizedText(i18n::message("menu.records.label"));
  replayButtonText->setAlign(TextView::CENTER);
  replayButtonText->setVAlign(TextView::MIDDLE);
  replayButton->setContentView(replayButtonText);
  styleThemedActionButton(replayButton, replayButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);
  replayButton->setOnClickListener([this]() {
    if (willStart.load() || replayExportJob_.inProgress()) {
      return;
    }
    const auto selectedMeta = selectedRecordSnapshot();
    if (!selectedMeta.has_value()) {
      return;
    }
    const bool courseStartReplay =
        selectedMeta->courseStart &&
        activeFolder.type == LibraryFolderItem::Type::Course &&
        activeFolder.courseId > 0;
    if (selectedMeta->solidArchive || selectedMeta->unavailable ||
        (!courseStartReplay && selectedMeta->meta.BmsPath.empty())) {
      return;
    }

    openReplayRecordsForSelection();
  });
  replayButtonSlot->addView(replayButton);

  findBmsButtonSlot = new View();
  findBmsButtonSlot->setWidth(kDetailsContentWidth)->setHeight(0)->setFlexShrink(0);
  findBmsButtonSlot->setVisible(false);
  findBmsButtonSlot->setDisplay(YGDisplayNone);
  findBmsButtonSlot->setAlignItems(YGAlignStretch);

  findBmsButton = new Button(0, 0, kDetailsContentWidth, 88);
  findBmsButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  findBmsButtonText->setLocalizedText(i18n::message("menu.find_bms.label"));
  findBmsButtonText->setAlign(TextView::CENTER);
  findBmsButtonText->setVAlign(TextView::MIDDLE);
  findBmsButton->setContentView(findBmsButtonText);
  styleThemedActionButton(findBmsButton, findBmsButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  findBmsButton->setOnClickListener([this]() { openFindBmsForSelection(); });
  findBmsButtonSlot->addView(findBmsButton);

  unzipButtonSlot = new View();
  unzipButtonSlot->setWidth(kDetailsContentWidth)->setHeight(0);
  unzipButtonSlot->setVisible(false);
  unzipButtonSlot->setAlignItems(YGAlignStretch);

  unzipButton = new Button(0, 0, kDetailsContentWidth, kMenuActionHeight);
  unzipButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  unzipButtonText->setLocalizedText(i18n::message("menu.unzip.label"));
  unzipButtonText->setAlign(TextView::CENTER);
  unzipButtonText->setVAlign(TextView::MIDDLE);
  unzipButton->setContentView(unzipButtonText);
  styleThemedActionButton(unzipButton, unzipButtonText, true,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  unzipButton->setOnClickListener(
      [this]() { startUnzipSelectedArchiveFolder(); });
  unzipButtonSlot->addView(unzipButton);

  rankingsButton = new Button(0, 0, 224, kMenuActionHeight);
  rankingsButton->setFlex(1)->setMinWidth(0);
  rankingsButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  rankingsButtonText->setLocalizedText(i18n::message("menu.rankings.label"));
  rankingsButtonText->setAlign(TextView::CENTER);
  rankingsButtonText->setVAlign(TextView::MIDDLE);
  rankingsButton->setContentView(rankingsButtonText);
  styleThemedActionButton(rankingsButton, rankingsButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed,
                          ui_theme::accentBorder);
  rankingsButton->setOnClickListener(
      [this]() { openRankingsForSelection(); });
  rankingsButton->setEnabled(false);
  auto *recordActions = new View();
  recordActions->setName("mainMenuRecordActions");
  recordActions->setWidth(kDetailsContentWidth)->setFlexShrink(0);
  recordActions->setFlexDirection(FlexDirection::Row)->setGap(12);
  recordActions->addView(replayButtonSlot);
  recordActions->addView(rankingsButton);

  chartActionsRow = new View();
  chartActionsRow->setFlexDirection(FlexDirection::Row);
  chartActionsRow->setAlignItems(YGAlignStretch);
  chartActionsRow->setWidth(kDetailsContentWidth);
  chartActionsRow->setHeight(kMenuActionHeight);
  chartActionsRow->setGap(10);

  auto *viewerButton = new Button(0, 0, 224, kMenuActionHeight);
  viewerButton->setFlex(1);
  auto *viewerButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  viewerButtonText->setLocalizedText(i18n::message("menu.viewer.label"));
  viewerButtonText->setAlign(TextView::CENTER);
  viewerButtonText->setVAlign(TextView::MIDDLE);
  viewerButton->setContentView(viewerButtonText);
  styleThemedActionButton(viewerButton, viewerButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  viewerButton->setOnClickListener([this]() { openChartViewerForSelection(); });
  chartActionsRow->addView(viewerButton);

  revealButton = new Button(0, 0, 224, kMenuActionHeight);
  revealButton->setFlex(1);
  auto *revealButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  revealButtonText->setLocalizedText(i18n::message("menu.reveal.label"));
  revealButtonText->setAlign(TextView::CENTER);
  revealButtonText->setVAlign(TextView::MIDDLE);
  revealButton->setContentView(revealButtonText);
  styleThemedActionButton(revealButton, revealButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  revealButton->setOnClickListener([this]() { toggleRevealContextMenu(); });
  chartActionsRow->addView(revealButton);
  rightContent->addView(chartActionsRow);

  ContextMenuView::Callbacks revealMenuCallbacks;
  revealMenuCallbacks.onActionSelected = [this](const std::string &actionId) {
    if (actionId == "show-same-folder") {
      showSelectedChartFolder();
    } else if (actionId == "reveal-file") {
      revealSelectedChartInFileManager();
    }
  };
  revealContextMenu = std::make_unique<ContextMenuView>(
      overlayPortal, std::move(revealMenuCallbacks));

  rightContent->addView(unzipButtonSlot);

  auto *settingsButton = new Button(0, 0, kDetailsContentWidth, kMenuActionHeight);
  settingsButton->setName("mainMenuSettings");
  auto *settingsText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  settingsText->setLocalizedText(i18n::message("menu.settings.label"));
  settingsText->setAlign(TextView::CENTER);
  settingsText->setVAlign(TextView::MIDDLE);
  settingsButton->setContentView(settingsText);
  styleThemedActionButton(settingsButton, settingsText, true,
                          ui_theme::dangerAction, ui_theme::dangerActionHover,
                          ui_theme::dangerActionPressed,
                          ui_theme::accentBorder);
  settingsButton->setOnClickListener([this, &context]() {
    if (willStart.load() || replayExportJob_.inProgress()) {
      return;
    }
    if (previewWorker_ != nullptr) {
      previewWorker_->stop();
    }
    stopAndClearSelectedChart();
    context.sceneManager->changeScene(
        std::make_unique<SettingsScene>(
            context, SettingsDestination::Profile,
            SceneReturnTarget::Retained(this)),
        true);
  });
  rightScroll->setContentView(rightContent);
  right->addView(rightScroll);
  // Primary actions stay reachable while chart details and tools scroll.
  auto *primaryActions = new View();
  primaryActions->setName("mainMenuPrimaryActions");
  primaryActions->setWidth(kDetailsContentWidth)->setFlexShrink(0);
  primaryActions->setFlexDirection(FlexDirection::Column)->setGap(12);
  primaryActions->addView(startButton);
  primaryActions->addView(findBmsButtonSlot);
  primaryActions->addView(recordActions);
  primaryActions->addView(settingsButton);
  right->addView(primaryActions);
  auto *controls = new View();
  controls->setName("mainMenuControls");
  controls->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
  controls->setFlex(1)->setMinWidth(0)->setMinHeight(0)->setGap(8);
  controls->setDisplay(YGDisplayNone);
  detailsControlsScroll_ = new ScrollView();
  detailsControlsScroll_->setWidthPercent(100)->setFlex(1)->setMinHeight(0);
  detailsControlsScroll_->clearBackgroundColor();
  detailsControlsContent_ = new View();
  detailsControlsContent_->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
  detailsControlsContent_->setGap(8)->setPadding(Edge::Bottom, 4);
  detailsControlsScroll_->setContentView(detailsControlsContent_);
  controls->addView(detailsControlsScroll_);
  right->addView(controls);
  rootLayout->addView(right);
  buildPlayOptionsModal();
  recordsModal_ = ReplayRecordsModal::Create(rootLayout, makeRecordsModalCallbacks());
  buildParseLogModal();
  buildTasksModal();
#if TARGET_OS_ANDROID
  buildFileActionsModal();
#endif
  buildFindBmsModal();
  buildUnzipProgressModal();
  rootLayout->addView(overlayPortal);
  reloadProfileSelectionsFromSettings();
  reloadScoreClearRanks();
  reloadFolderItems();
  reloadChartList();
  libraryRevision = context.chartRepository.GetLibraryRevision();
  updatePanelLayout();
  rootLayout->applyYogaLayout();
}

float MainMenuScene::portraitDetailsHeight(float availableHeight) const {
  const auto *primary = rootLayout->findViewByName("mainMenuPrimaryActions");
  // Use the natural minimum, not the stretched left column's previous height,
  // so hiding controls on the right can shrink the panel again.
  const float detailsHeight = ChartDetailsView::minimumChartHeight();
  const float controlsHeight = detailsControlsContent_ ? detailsControlsContent_->getHeight() : 0;
  const float actionsHeight = primary ? primary->getHeight() : 0;
  // Match the two scroll columns, the action gap, and the panel's 16-unit
  // top/bottom padding plus border. Tall content stays scrollable while the
  // browser retains room for its toolbar and song rows.
  const float contentHeight = std::max(detailsHeight, controlsHeight + actionsHeight + 8);
  return std::clamp(contentHeight + 34, 0.0F, std::max(0.0F, availableHeight - 320));
}

void MainMenuScene::updatePanelLayout() {
  if (!rootLayout) return;
  View::LayoutBatchScope batch;
  const bool portrait = rendering::window_height > rendering::window_width;
  const auto safe = getSafeAreaInsetsUi();
  const float contentWidth = std::max(0, rendering::window_width - safe.left - safe.right - 2 * kRootPadding);
  const float contentHeight = std::max(0, rendering::window_height - safe.top - safe.bottom - 2 * kRootPadding - 24);
  const float detailsHeight = portraitDetailsHeight(contentHeight);
  auto *browser = rootLayout->findViewByName("mainMenuBrowser");
  auto *library = rootLayout->findViewByName("mainMenuLibrary");
  auto *actions = rootLayout->findViewByName("mainMenuLibraryActions");
  auto *details = rootLayout->findViewByName("mainMenuDetails");
  auto *detailsScroll = rootLayout->findViewByName("mainMenuDetailsScroll");
  auto *controls = rootLayout->findViewByName("mainMenuControls");
  rootLayout->setFlexDirection(portrait ? FlexDirection::Column : FlexDirection::Row);
  if (browser) {
    browser->setFlex(portrait ? 0.0F : 1.0F);
    browser->setWidth(portrait ? contentWidth : YGUndefined);
    browser->setHeight(portrait ? contentHeight - detailsHeight : YGUndefined);
  }
  if (library) {
    library->setWidth(portrait ? std::max(0.0F, contentWidth - 24) * 0.3F : kLibraryPanelWidth);
    library->setHeight(YGUndefined)->setMinHeight(0);
    library->setFlexShrink(0);
  }
  if (actions) {
    actions->setFlexDirection(FlexDirection::Column);
    for (auto *action : actions->getChildren()) action->setWidthPercent(100);
  }
  if (details) {
    details->setWidth(portrait ? YGUndefined : kDetailsPanelWidth);
    details->setHeight(portrait ? detailsHeight : YGUndefined)->setMinHeight(0);
    details->setFlexDirection(portrait ? FlexDirection::Row : FlexDirection::Column);
    details->setAlignItems(portrait ? YGAlignStretch : YGAlignCenter);
    details->setPadding(Edge::Left, portrait ? 12.0F : 0.0F);
    details->setPadding(Edge::Right, portrait ? 12.0F : 0.0F);
    details->setPadding(Edge::Top, portrait ? 16.0F : 0.0F);
  }
  if (detailsScroll) {
    detailsScroll->setWidth(portrait ? YGUndefined : kDetailsPanelWidth - 20);
    detailsScroll->setHeight(portrait ? std::max(0.0F, detailsHeight - 34) : YGUndefined);
  }
  if (controls) {
    controls->setWidth(YGUndefined);
    controls->setHeight(portrait ? std::max(0.0F, detailsHeight - 34) : YGUndefined);
  }
  chartDetailsView_->setMinHeight(portrait ? std::max(0.0F, detailsHeight - 34) : 0);
  detailsContent_->setPadding(Edge::Top, portrait ? 0 : 16);
  detailsContent_->setPadding(Edge::Bottom, portrait ? 0 : 16);
  updateMenuPresentation(portrait);
}

void MainMenuScene::updateMenuPresentation(bool portrait) {
  if (!detailsContent_ || !detailsControlsContent_) return;
  auto *details = rootLayout->findViewByName("mainMenuDetails");
  auto *controls = rootLayout->findViewByName("mainMenuControls");
  auto *primary = rootLayout->findViewByName("mainMenuPrimaryActions");
  auto *records = rootLayout->findViewByName("mainMenuRecordActions");
  auto *settings = static_cast<Button *>(rootLayout->findViewByName("mainMenuSettings"));
  auto *detailsScroll = static_cast<ScrollView *>(rootLayout->findViewByName("mainMenuDetailsScroll"));
  auto *target = portrait ? detailsControlsContent_ : detailsContent_;
  chartDetailsView_->setScoreContainer(portrait ? detailsControlsContent_ : nullptr);
  for (auto *view : std::array<View *, 3>{readyPlayOptionsButton, chartActionsRow,
           unzipButtonSlot}) {
    view->moveTo(*target);
  }
  settings->moveTo(portrait ? *records : *primary);
  primary->moveTo(portrait ? *controls : *details);
  controls->setDisplay(portrait ? YGDisplayFlex : YGDisplayNone);
  controls->setVisible(portrait);
  tutorialRightScroll_ = portrait ? detailsControlsScroll_ : detailsScroll;
  primary->setGap(portrait ? 8 : 12);
  records->setGap(portrait ? 8 : 12);
  settings->setFlex(portrait ? 1 : 0)->setMinWidth(0);
  settings->setWidth(portrait ? YGUndefined : kDetailsContentWidth);
  settings->getContentView()->setAutoFitText(portrait);
  replayButtonText->setAutoFitText(portrait);
  rankingsButtonText->setAutoFitText(portrait);
  for (auto *view : std::array<View *, 7>{primary, records, startButton,
           readyPlayOptionsButton, chartActionsRow, unzipButtonSlot, findBmsButtonSlot}) {
    if (portrait) view->setWidthPercent(100);
    else view->setWidth(kDetailsContentWidth);
  }
  for (auto *button : {unzipButton, findBmsButton}) {
    if (portrait) button->setWidthPercent(100);
    else button->setWidth(kDetailsContentWidth);
  }
  const int height = portrait ? kPortraitMenuActionHeight : kMenuActionHeight;
  const int primaryHeight = portrait ? height : 88;
  startButton->setHeight(primaryHeight);
  findBmsButton->setHeight(primaryHeight);
  findBmsButtonSlot->setHeight(findBmsButtonSlot->getVisible() ? primaryHeight : 0);
  for (auto *view : std::array<View *, 7>{replayButtonSlot, replayButton, rankingsButton,
           settings, chartActionsRow, unzipButton, searchBox}) view->setHeight(height);
  for (auto *view : chartActionsRow->getChildren()) view->setHeight(height);
  unzipButtonSlot->setHeight(unzipButtonSlot->getVisible() ? height : 0);
  for (auto *button : {chartFilterButton, chartSortButton})
    button->setWidth(height)->setHeight(height);
  auto *toolbar = rootLayout->findViewByName("mainMenuToolbar");
  toolbar->setHeight(height);
  for (auto *view : toolbar->getChildren()) view->setHeight(height);
  for (auto *view : rootLayout->findViewByName("mainMenuLibraryActions")->getChildren())
    view->setHeight(height);
  readyPlayOptionsButton->setHeight(portrait ? 96 : 122);
  auto *summary = readyPlayOptionsButton->getContentView();
  summary->setPadding(Edge::Top, portrait ? 8 : 10);
  summary->setPadding(Edge::Bottom, portrait ? 8 : 10);
  summary->setGap(portrait ? 4 : 6);
  const int rowHeight = portrait ? 24 : 28;
  for (auto *row : summary->getChildren()) {
    row->setHeight(rowHeight);
    for (auto *child : row->getChildren()) {
      child->setHeight(rowHeight);
      for (auto *label : child->getChildren()) label->setHeight(rowHeight);
    }
  }
  detailsScroll->refreshContentLayout();
  detailsControlsScroll_->refreshContentLayout();
}

void MainMenuScene::reloadFolderItems(bool preserveViewState) {
  if (folderRecyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? folderRecyclerView->scrollOffset : 0.0f;
  const std::uint64_t currentLibraryRevision =
      context.chartRepository.GetLibraryRevision();
  if (!folderMetadataCache.valid ||
      folderMetadataCache.libraryRevision != currentLibraryRevision) {
    folderMetadataCache = LibraryFolderMetadataCache{};
    folderMetadataCache.libraryRevision = currentLibraryRevision;
    folderMetadataCache.allSongCount = chartSession->CountAllChartMeta();
    folderMetadataCache.favoriteCount = chartSession->CountFavoriteCharts();
    folderMetadataCache.solidArchiveCount = chartSession->CountSolidArchives();
    folderMetadataCache.tables = chartSession->SelectDifficultyTables();
    folderMetadataCache.courseTables =
        chartSession->SelectDifficultyCourseTables();
    folderMetadataCache.valid = true;
  }
  std::vector<LibraryFolderItem> folders;

  const int allSongCount = folderMetadataCache.allSongCount;
  const int favoriteCount = folderMetadataCache.favoriteCount;

  auto isExpanded = [this](const std::string &key) {
    return expandedLibraryFolders.find(key) != expandedLibraryFolders.end();
  };
  auto appendClearMarkFilters = [&](const LibraryFolderItem &parent,
                                    int childDepth) {
    for (const auto &filter : kDifficultyClearMarkFilters) {
      const int count = clearMarkCountForFolder(parent.key, filter.rank);
      if (count <= 0) {
        continue;
      }
      folders.push_back({
          .key = clearMarkFolderKey(parent.key, filter.rank),
          .label = filter.label,
          .type = LibraryFolderItem::Type::DifficultyClearMark,
          .depth = childDepth,
          .count = count,
          .tableId = parent.tableId,
          .tableLevel = parent.tableLevel,
          .clearRank = filter.rank,
          .clearMarkRank = filter.rank,
          .clearMarkFolder = true,
      });
    }
  };

  const LibraryFolderItem allSongsItem{
      .key = "all",
      .label = i18n::tr("library.folders.all_songs.label"),
      .type = LibraryFolderItem::Type::AllSongs,
      .depth = 0,
      .count = allSongCount,
      .expandable = true,
      .expanded = isExpanded("all"),
  };
  folders.push_back(allSongsItem);
  if (allSongsItem.expanded) {
    appendClearMarkFilters(allSongsItem, 1);
  }

  folders.push_back({
      .key = "favorites",
      .label = i18n::tr("library.folders.favorites.label"),
      .type = LibraryFolderItem::Type::Favorites,
      .depth = 0,
      .count = favoriteCount,
  });

  const int solidArchiveCount = folderMetadataCache.solidArchiveCount;
  if (solidArchiveCount > 0) {
    folders.push_back({
        .key = "solid-archives",
        .label = i18n::tr("library.folders.solid_archive.label"),
        .type = LibraryFolderItem::Type::SolidArchives,
        .depth = 0,
        .count = solidArchiveCount,
    });
  }

  for (const auto &table : folderMetadataCache.tables) {
    const std::string tableKey = folderKeyForTable(table.id);
    const LibraryFolderItem tableItem{
        .key = tableKey,
        .label = table.name,
        .type = LibraryFolderItem::Type::DifficultyTable,
        .depth = 0,
        .count = table.chartCount,
        .tableId = table.id,
        .expandable = true,
        .expanded = isExpanded(tableKey),
    };
    folders.push_back(tableItem);
    if (!tableItem.expanded) {
      continue;
    }

    appendClearMarkFilters(tableItem, 1);

    auto levelsIt = folderMetadataCache.levelsByTable.find(table.id);
    if (levelsIt == folderMetadataCache.levelsByTable.end()) {
      levelsIt =
          folderMetadataCache.levelsByTable
              .emplace(table.id,
                       chartSession->SelectDifficultyLevels(table.id))
              .first;
    }
    const auto &levels = levelsIt->second;
    for (const auto &level : levels) {
      const std::string levelKey =
          folderKeyForLevel(level.tableId, level.level);
      const LibraryFolderItem levelItem{
          .key = levelKey,
          .label = level.tableSymbol + level.level,
          .type = LibraryFolderItem::Type::DifficultyLevel,
          .depth = 1,
          .count = level.chartCount,
          .tableId = level.tableId,
          .tableLevel = level.level,
          .expandable = true,
          .expanded = isExpanded(levelKey),
      };
      folders.push_back(levelItem);
      if (levelItem.expanded) {
        appendClearMarkFilters(levelItem, 2);
      }
    }
  }

  const auto &courseTables = folderMetadataCache.courseTables;
  if (!courseTables.empty()) {
    const LibraryFolderItem coursesRootItem{
        .key = "courses",
        .label = i18n::tr("library.folders.courses.label"),
        .type = LibraryFolderItem::Type::CoursesRoot,
        .depth = 0,
        .count = -1,
        .expandable = true,
        .expanded = isExpanded("courses"),
    };
    folders.push_back(coursesRootItem);
    if (coursesRootItem.expanded) {
      const auto makeCourseItem =
          [](int courseId, const std::string &courseKey, int tableId,
             const std::string &groupName, const std::string &level,
             const std::string &name, const std::string &constraintJson,
             int depth) {
        const std::string courseLabel = level.empty() ? name : level;
        return LibraryFolderItem{
            .key = folderKeyForCourse(courseId),
            .label = courseLabel,
            .type = LibraryFolderItem::Type::Course,
            .depth = depth,
            .count = -1,
            .courseId = courseId,
            .courseKey = courseKey,
            .courseTableId = tableId,
            .courseGroupName = groupName,
            .courseConstraintJson = constraintJson,
        };
      };
      const auto makeCourseInfoItem = [&](const DifficultyCourseInfo &course,
                                          int depth) {
        return makeCourseItem(course.id, course.courseKey, course.tableId,
                              course.groupName, course.level, course.name,
                              course.constraintJson, depth);
      };

      for (const auto &table : courseTables) {
        const std::string tableKey = folderKeyForCourseTable(table.tableId);
        const LibraryFolderItem tableItem{
            .key = tableKey,
            .label = table.tableName,
            .type = LibraryFolderItem::Type::CourseTable,
            .depth = 1,
            .count = -1,
            .courseTableId = table.tableId,
            .expandable = true,
            .expanded = isExpanded(tableKey),
        };
        folders.push_back(tableItem);
        if (!tableItem.expanded) {
          continue;
        }

        auto groupsIt =
            folderMetadataCache.courseGroupsByTable.find(table.tableId);
        if (groupsIt == folderMetadataCache.courseGroupsByTable.end()) {
          groupsIt = folderMetadataCache.courseGroupsByTable
                         .emplace(table.tableId,
                              chartSession->SelectDifficultyCourseGroups(
                                  table.tableId))
                         .first;
        }
        for (const auto &group : groupsIt->second) {
          const std::string label =
              group.groupName.empty() ? i18n::tr("library.folders.ungrouped.label") : group.groupName;
          const std::string groupKey =
              folderKeyForCourseGroup(group.tableId, group.groupName);
          const bool duplicateSingletonGroup =
              group.courseCount == 1 && group.singletonCourseId > 0 &&
              (group.groupName.empty() || group.singletonCourseName == label ||
               group.singletonCourseLevel == label);
          if (duplicateSingletonGroup) {
            folders.push_back(makeCourseItem(
                group.singletonCourseId, group.singletonCourseKey,
                group.tableId, group.groupName, group.singletonCourseLevel,
                group.singletonCourseName,
                group.singletonCourseConstraintJson, 2));
            continue;
          }

          const LibraryFolderItem groupItem{
              .key = groupKey,
              .label = label,
              .type = LibraryFolderItem::Type::CourseGroup,
              .depth = 2,
              .count = -1,
              .courseTableId = group.tableId,
              .courseGroupName = group.groupName,
              .expandable = true,
              .expanded = isExpanded(groupKey),
          };
          folders.push_back(groupItem);
          if (!groupItem.expanded) {
            continue;
          }

          auto coursesIt = folderMetadataCache.coursesByGroup.find(groupKey);
          if (coursesIt == folderMetadataCache.coursesByGroup.end()) {
            coursesIt = folderMetadataCache.coursesByGroup
                            .emplace(groupKey,
                                     chartSession->SelectDifficultyCourses(
                                         group.tableId, group.groupName))
                            .first;
          }
          for (const auto &course : coursesIt->second) {
            folders.push_back(makeCourseInfoItem(course, 3));
          }
        }
      }
    }
  }

  for (auto &folder : folders) {
    folder.clearRank =
        folder.clearMarkFolder ? folder.clearMarkRank
                               : clearRankForFolder(folder.key);
  }

  if (activeFolder.key.empty()) {
    activeFolder = folders.front();
  }

  bool activeStillExists = false;
  int activeIndex = 0;
  for (int i = 0; i < folders.size(); i++) {
    const auto &folder = folders[i];
    if (folder.key == activeFolder.key) {
      activeFolder = folder;
      activeStillExists = true;
      activeIndex = i;
      break;
    }
  }
  if (!activeStillExists) {
    activeFolder = folders.front();
    activeIndex = 0;
  }
  refreshChartFilterPanel();

  const int folderCount = static_cast<int>(folders.size());
  folderRecyclerView->setItems(std::move(folders));
  const bool sidebarFolderSelected = !temporaryChartFolder.has_value();
  folderRecyclerView->selectedIndex = sidebarFolderSelected ? activeIndex : -1;
  if (preserveViewState) {
    const float maxOffset =
        std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                              folderRecyclerView->itemHeight -
                                          folderRecyclerView->getHeight()));
    folderRecyclerView->scrollOffset =
        std::clamp(previousScrollOffset, 0.0f, maxOffset);
    folderRecyclerView->rebindVisibleItems();
  }
  if (sidebarFolderSelected) {
    auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
    if (selectedView != nullptr) {
      selectedView->onSelected();
    }
  }
}

void MainMenuScene::refreshFavoriteFolderCount() {
  if (folderRecyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  std::vector<LibraryFolderItem> folders = folderRecyclerView->getItems();
  if (folders.empty()) {
    reloadFolderItems(true);
    return;
  }

  const int favoriteCount = chartSession->CountFavoriteCharts();
  if (folderMetadataCache.valid) {
    folderMetadataCache.favoriteCount = favoriteCount;
    folderMetadataCache.libraryRevision =
        context.chartRepository.GetLibraryRevision();
  }
  const float previousScrollOffset = folderRecyclerView->scrollOffset;
  int activeIndex = std::clamp(folderRecyclerView->selectedIndex, 0,
                               static_cast<int>(folders.size()) - 1);
  bool foundFavorites = false;

  for (int i = 0; i < static_cast<int>(folders.size()); ++i) {
    auto &folder = folders[static_cast<std::size_t>(i)];
    if (folder.key == "favorites") {
      folder.count = favoriteCount;
      foundFavorites = true;
      if (activeFolder.key == folder.key) {
        activeFolder = folder;
      }
    }
    if (folder.key == activeFolder.key) {
      activeIndex = i;
    }
  }

  if (!foundFavorites) {
    reloadFolderItems(true);
    return;
  }

  const int folderCount = static_cast<int>(folders.size());
  folderRecyclerView->setItems(std::move(folders));
  const bool sidebarFolderSelected = !temporaryChartFolder.has_value();
  folderRecyclerView->selectedIndex = sidebarFolderSelected ? activeIndex : -1;
  const float maxOffset =
      std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                            folderRecyclerView->itemHeight -
                                        folderRecyclerView->getHeight()));
  folderRecyclerView->scrollOffset =
      std::clamp(previousScrollOffset, 0.0f, maxOffset);
  folderRecyclerView->rebindVisibleItems();
  if (sidebarFolderSelected) {
    auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
    if (selectedView != nullptr) {
      selectedView->onSelected();
    }
  }
}

ChartMetaQuery MainMenuScene::chartQueryForActiveFolder() const {
  const int selectedLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  if (temporaryChartFolder.has_value()) {
    return main_menu_library::chartQueryForSameFolder(
        *temporaryChartFolder, searchText, chartRecordFilters,
        selectedLongNoteMode);
  }

  ChartMetaQuery query;
  query.keyword = searchText;
  query.selectedLongNoteMode = selectedLongNoteMode;

  switch (activeFolder.type) {
  case LibraryFolderItem::Type::SolidArchives:
    query.solidArchivesOnly = true;
    break;
  case LibraryFolderItem::Type::Favorites:
    query.favoritesOnly = true;
    break;
  case LibraryFolderItem::Type::DifficultyTable:
    query.tableId = activeFolder.tableId;
    break;
  case LibraryFolderItem::Type::DifficultyLevel:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    break;
  case LibraryFolderItem::Type::DifficultyClearMark:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    query.clearMarkFilter = true;
    query.clearMarkRank = activeFolder.clearMarkRank;
    break;
  case LibraryFolderItem::Type::CoursesRoot:
    query.coursesOnly = true;
    break;
  case LibraryFolderItem::Type::CourseTable:
    query.courseTableId = activeFolder.courseTableId;
    break;
  case LibraryFolderItem::Type::CourseGroup:
    query.courseTableId = activeFolder.courseTableId;
    query.courseGroupName = activeFolder.courseGroupName;
    break;
  case LibraryFolderItem::Type::Course:
    query.courseId = activeFolder.courseId;
    break;
  case LibraryFolderItem::Type::AllSongs:
  default:
    break;
  }
  chart_record_filters::applyToQuery(query, chartRecordFilters,
                                     chartDifficultyRangeEnabled());
  return query;
}

bool MainMenuScene::chartDifficultyRangeEnabled() const {
  if (temporaryChartFolder.has_value()) {
    return false;
  }
  return main_menu_library::difficultyRangeEnabledForFolder(
      activeFolder.type == LibraryFolderItem::Type::DifficultyTable,
      activeFolder.clearMarkFolder, activeFolder.tableId,
      activeFolder.tableLevel);
}

std::vector<DifficultyLevelInfo>
MainMenuScene::chartFilterDifficultyLevels() const {
  if (!chartDifficultyRangeEnabled()) {
    return {};
  }
  const auto tableIt = folderMetadataCache.levelsByTable.find(
      activeFolder.tableId);
  if (tableIt == folderMetadataCache.levelsByTable.end()) {
    return {};
  }
  return tableIt->second;
}

void MainMenuScene::setChartFilterPanelVisible(bool visible) {
  chartFilterPanelVisible = visible;
  if (visible) {
    chartSortPanelVisible = false;
  } else {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartSortPanelVisible(bool visible) {
  chartSortPanelVisible = visible;
  if (visible) {
    chartFilterPanelVisible = false;
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::refreshChartFilterPanel() {
  const bool sameFolderScope = temporaryChartFolder.has_value();
  const bool difficultyRangeEnabled = chartDifficultyRangeEnabled();
  const auto levels = chartFilterDifficultyLevels();
  const std::optional<int> folderClearMarkRank =
      !sameFolderScope && activeFolder.clearMarkFolder
          ? std::optional<int>(activeFolder.clearMarkRank)
          : std::nullopt;
  if (!sameFolderScope && activeFolder.clearMarkFolder) {
    chartRecordFilters.clearMarkRank.reset();
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
    chartClearMarkDropdownOpen = false;
  }
  chart_record_filters::normalizeSelection(chartRecordFilters,
                                           folderClearMarkRank);
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : folderClearMarkRank;
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    chartScoreRankDropdownOpen = false;
  }
  if (!chartFilterPanelVisible) {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  if (difficultyRangeEnabled) {
    const bool difficultyTableChanged =
        chart_record_filters::resetDifficultyRangeOnTableChange(
            chartRecordFilters, chartDifficultyRangeTableId,
            activeFolder.tableId);
    if (difficultyTableChanged) {
      chartDifficultyMinDropdownOpen = false;
      chartDifficultyMaxDropdownOpen = false;
    }
    normalizeDifficultyFilterRange(chartRecordFilters, levels);
  } else {
    chartDifficultyRangeTableId.reset();
    chartRecordFilters.difficultyMinLevel.reset();
    chartRecordFilters.difficultyMaxLevel.reset();
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
    if (!sameFolderScope &&
        chartRecordFilters.sort.criterion ==
            ChartRecordSortCriterion::Difficulty) {
      chartRecordFilters.sort = {};
    }
  }

  if (chartFilterPanel != nullptr) {
    chartFilterPanel->refresh({
        .filters = chartRecordFilters,
        .bpmMinText = chartBpmMinText,
        .bpmMaxText = chartBpmMaxText,
        .clearMarkFilterVisible =
            sameFolderScope || !activeFolder.clearMarkFolder,
        .effectiveClearMarkRank = effectiveClearMarkRank,
        .clearMarkDropdownOpen = chartClearMarkDropdownOpen,
        .scoreRankDropdownOpen = chartScoreRankDropdownOpen,
        .difficultyRangeEnabled = difficultyRangeEnabled,
        .difficultyMinDropdownOpen = chartDifficultyMinDropdownOpen,
        .difficultyMaxDropdownOpen = chartDifficultyMaxDropdownOpen,
        .difficultyLevels = levels,
    }, chartFilterPanelVisible);
  }
  if (chartSortPanel != nullptr) {
    chartSortPanel->refresh({
        .sort = chartRecordFilters.sort,
        .difficultySortEnabled = difficultyRangeEnabled || sameFolderScope,
    }, chartSortPanelVisible);
  }
  refreshChartFilterButtons();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::refreshChartFilterButtons() {
  const bool filterActive =
      chartFilterPanelVisible || chartRecordFilters.clearMarkRank.has_value() ||
      chartRecordFilters.scoreRank.has_value() ||
      chartRecordFilters.bpmMin.has_value() ||
      chartRecordFilters.bpmMax.has_value() ||
      chartRecordFilters.difficultyMinLevel.has_value() ||
      chartRecordFilters.difficultyMaxLevel.has_value();
  styleThemedActionButton(
      chartFilterButton, chartFilterButtonText, true,
      filterActive ? ui_theme::primaryAction : ui_theme::control,
      filterActive ? ui_theme::primaryActionHover : ui_theme::controlHover,
      filterActive ? ui_theme::primaryActionPressed : ui_theme::controlPressed,
      filterActive ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong);

  const bool sortActive =
      chartSortPanelVisible ||
      chartRecordFilters.sort.criterion != ChartRecordSortCriterion::Default;
  styleThemedActionButton(
      chartSortButton, chartSortButtonText, true,
      sortActive ? ui_theme::primaryAction : ui_theme::control,
      sortActive ? ui_theme::primaryActionHover : ui_theme::controlHover,
      sortActive ? ui_theme::primaryActionPressed : ui_theme::controlPressed,
      sortActive ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong);
}

void MainMenuScene::setChartClearFilter(std::optional<int> rank) {
  if (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder) {
    rank.reset();
  }
  chartRecordFilters.clearMarkRank = rank;
  if (!rank.has_value()) {
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
  }
  chart_record_filters::normalizeSelection(chartRecordFilters);
  chartClearMarkDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankFilter(std::optional<std::string> rank) {
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder
                 ? std::optional<int>(activeFolder.clearMarkRank)
                 : std::nullopt);
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    rank.reset();
  }
  chartRecordFilters.scoreRank = rank;
  if (!rank.has_value()) {
    chartRecordFilters.scoreRankOrAbove = false;
    chartRecordFilters.scoreRankOrBelow = false;
  }
  chartScoreRankDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartBpmMinFilter(const std::string &text) {
  chartBpmMinText = text;
  chartRecordFilters.bpmMin = parseOptionalBpmFilter(text);
  chart_record_filters::normalizeBpmRange(chartRecordFilters, chartBpmMinText,
                                          chartBpmMaxText);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartBpmMaxFilter(const std::string &text) {
  chartBpmMaxText = text;
  chartRecordFilters.bpmMax = parseOptionalBpmFilter(text);
  chart_record_filters::normalizeBpmRange(chartRecordFilters, chartBpmMinText,
                                          chartBpmMaxText);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyMinFilter(
    std::optional<std::string> level) {
  chart_record_filters::setDifficultyMinLevel(chartRecordFilters,
                                              chartFilterDifficultyLevels(),
                                              std::move(level));
  chartDifficultyMinDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyMaxFilter(
    std::optional<std::string> level) {
  chart_record_filters::setDifficultyMaxLevel(chartRecordFilters,
                                              chartFilterDifficultyLevels(),
                                              std::move(level));
  chartDifficultyMaxDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartClearMarkDropdownOpen(bool open) {
  if (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder) {
    open = false;
  }
  chartClearMarkDropdownOpen = open;
  if (open) {
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankDropdownOpen(bool open) {
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder
                 ? std::optional<int>(activeFolder.clearMarkRank)
                 : std::nullopt);
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    open = false;
  }
  chartScoreRankDropdownOpen = open;
  if (open) {
    chartClearMarkDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartClearMarkRange(bool orAbove, bool orBelow) {
  if (!chartRecordFilters.clearMarkRank.has_value()) {
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
  } else {
    chartRecordFilters.clearMarkOrAbove = orAbove;
    chartRecordFilters.clearMarkOrBelow = !orAbove && orBelow;
  }
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankRange(bool orAbove, bool orBelow) {
  if (!chartRecordFilters.scoreRank.has_value()) {
    chartRecordFilters.scoreRankOrAbove = false;
    chartRecordFilters.scoreRankOrBelow = false;
  } else {
    chartRecordFilters.scoreRankOrAbove = orAbove;
    chartRecordFilters.scoreRankOrBelow = !orAbove && orBelow;
  }
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyDropdownOpen(bool minLevel, bool open) {
  if (open) {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
  }
  if (minLevel) {
    chartDifficultyMinDropdownOpen = open;
    if (open) {
      chartDifficultyMaxDropdownOpen = false;
    }
  } else {
    chartDifficultyMaxDropdownOpen = open;
    if (open) {
      chartDifficultyMinDropdownOpen = false;
    }
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartSortCriterion(
    ChartRecordSortCriterion criterion) {
  if (criterion == ChartRecordSortCriterion::Difficulty &&
      !chartDifficultyRangeEnabled() && !temporaryChartFolder.has_value()) {
    return;
  }
  chartRecordFilters.sort =
      chart_record_filters::nextSortState(chartRecordFilters.sort, criterion);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::reloadChartListForFolderSelection() {
  prioritizeVisibleArtworkBindings = true;
  reloadChartList();
  prioritizeVisibleArtworkBindings = false;
}

void MainMenuScene::reloadChartList(bool preserveViewState) {
  if (recyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? recyclerView->scrollOffset : 0.0f;
  const int previousSelectedIndex =
      preserveViewState ? recyclerView->selectedIndex : -1;
  std::filesystem::path visibleSelectedPath;
  if (preserveViewState && previousSelectedIndex >= 0 &&
      previousSelectedIndex < recyclerView->size()) {
    visibleSelectedPath =
        recyclerView->get(previousSelectedIndex).meta.BmsPath;
  }
  const path_t previousSelectedPath =
      preserveViewState
          ? fspath_to_path_t(main_menu_library::chartSelectionPathForReload(
                visibleSelectedPath, selectedChartRecord))
          : path_t{};
  int previousTopIndex = -1;
  float previousTopItemOffset = 0.0f;
  path_t previousTopPath;
  if (preserveViewState && recyclerView->itemHeight > 0 &&
      recyclerView->size() > 0) {
    previousTopIndex = std::clamp(
        static_cast<int>(previousScrollOffset /
                         static_cast<float>(recyclerView->itemHeight)),
        0, recyclerView->size() - 1);
    previousTopItemOffset =
        previousScrollOffset -
        static_cast<float>(previousTopIndex * recyclerView->itemHeight);
    previousTopPath =
        fspath_to_path_t(recyclerView->get(previousTopIndex).meta.BmsPath);
  }

  ChartMetaQuery query = chartQueryForActiveFolder();

  std::optional<ChartMetaRecord> leadingRecord;
  if (!temporaryChartFolder.has_value() &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0) {
    ChartMetaRecord courseRecord;
    courseRecord.courseStart = true;
    courseRecord.meta.Title = activeFolder.label.empty() ? i18n::tr("menu.course.label")
                                                         : activeFolder.label;
    courseRecord.meta.Artist = activeFolder.courseGroupName.empty()
                                   ? i18n::tr("menu.course_mode.label")
                                   : activeFolder.courseGroupName;
    courseRecord.difficultyTableLabels =
        activeFolder.label.empty() ? i18n::tr("menu.course.label") : activeFolder.label;
    leadingRecord = std::move(courseRecord);
  }

  if (!temporaryChartFolder.has_value() &&
      activeFolder.type == LibraryFolderItem::Type::SolidArchives) {
    leadingRecord = main_menu_library::unzipAllRecord(
        folderMetadataCache.solidArchiveCount);
  }
  const int databaseCount = chartSession->CountChartMeta(query);
  selectedChartRecord = main_menu_library::chartSelectionRecordForReload(
      selectedChartRecord, leadingRecord, preserveViewState);
  const int count = databaseCount + (leadingRecord.has_value() ? 1 : 0);
  const int leadingOffset = leadingRecord.has_value() ? 1 : 0;
  chartListCache.reset(*chartSession, query, databaseCount,
                       std::move(leadingRecord));
  recyclerView->setItemProvider(
      count, [this](int index) -> const ChartMetaRecord & {
        return chartListCache.get(index);
      });
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshSelectedChartActionState();
  if (!temporaryChartFolder.has_value() && !selectedChartRecord.has_value() &&
      !preserveViewState &&
      activeFolder.type == LibraryFolderItem::Type::Course && count > 0) {
    recyclerView->selectedIndex = 0;
    if (recyclerView->onSelected) {
      recyclerView->onSelected(recyclerView->get(0), 0);
    }
  }
  if (!preserveViewState) {
    return;
  }

  const float maxOffset = std::max(
      0.0f, static_cast<float>(std::max(1, count) * recyclerView->itemHeight -
                               recyclerView->getHeight()));
  auto pathMatches = [&](int index, const path_t &path) {
    if (index < 0 || index >= count || path.empty()) {
      return false;
    }
    return fspath_to_path_t(recyclerView->get(index).meta.BmsPath) == path;
  };
  auto findPathNear = [&](const path_t &path, int preferredIndex) {
    if (path.empty() || count <= 0) {
      return -1;
    }
    if (pathMatches(preferredIndex, path)) {
      return preferredIndex;
    }
    const int databaseIndex = chartSession->FindChartMetaIndex(
        query, std::filesystem::path(path));
    if (databaseIndex < 0) {
      return -1;
    }
    const int index = databaseIndex + leadingOffset;
    return index >= 0 && index < count ? index : -1;
  };

  float restoredScrollOffset = std::clamp(previousScrollOffset, 0.0f, maxOffset);
  const int restoredTopIndex = findPathNear(previousTopPath, previousTopIndex);
  if (restoredTopIndex >= 0) {
    restoredScrollOffset = std::clamp(
        static_cast<float>(restoredTopIndex * recyclerView->itemHeight) +
            previousTopItemOffset,
        0.0f, maxOffset);
  }
  recyclerView->scrollOffset = restoredScrollOffset;

  const int restoredSelectedIndex =
      selectedChartRecord && selectedChartRecord->unzipAll && leadingOffset > 0
          ? 0 : findPathNear(previousSelectedPath, previousSelectedIndex);

  recyclerView->selectedIndex = restoredSelectedIndex;
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshSelectedChartActionState();
  recyclerView->rebindVisibleItems();
}

std::optional<std::string> MainMenuScene::reloadScoreClearRanks() {
  if (!chartSession.has_value()) {
    return "chart database is unavailable";
  }

  profile_database_activity::WriteGuard profileDatabaseOperation;
  const auto definitions = chartSession->SelectDifficultyCourseDefinitions();
  const CourseScoreRecoveryResult scoreRecovery =
      context.scoreRepository.RecoverCourseRecords(definitions);
  if (!scoreRecovery.ok()) {
    SDL_Log("SQL error while recovering course scores: %s",
            scoreRecovery.errorMessage.c_str());
  }

  auto prepared =
      context.scoreRepository.PrepareScoreQueryDatabase(*chartSession);
  if (const auto &error = prepared.error()) {
    SDL_Log("SQL error while preparing score query database: %s",
            error->c_str());
    return error;
  }
  scoreClearRanks = context.scoreRepository.LoadBestClearRanks(
      *chartSession, score_cache_queries::kScoreDatabaseSchema);
  scoreBestScores = context.scoreRepository.LoadBestScores(
      *chartSession, score_cache_queries::kScoreDatabaseSchema);
  const ScoreClearRankCache localClearRanks =
      context.scoreRepository.LoadLocalBestClearRanks(
          *chartSession, score_cache_queries::kScoreDatabaseSchema);
  folderClearData = chartSession->LoadFolderClearDataByLongNoteMode(
      scoreClearRanks, localClearRanks);
  scoreClearRanksRevision = context.scoreRepository.GetRevision();
  return std::nullopt;
}

std::optional<std::string> MainMenuScene::prepareScoreQueryDatabase() {
  if (!chartSession.has_value()) {
    return "chart database is unavailable";
  }

  auto prepared =
      context.scoreRepository.PrepareScoreQueryDatabase(*chartSession);
  if (const auto &error = prepared.error()) {
    SDL_Log("SQL error while preparing score query database: %s",
            error->c_str());
    return error;
  }
  return std::nullopt;
}

std::optional<std::string> MainMenuScene::refreshScoreClearRankViews() {
  if (const auto error = reloadScoreClearRanks()) {
    return error;
  }
  refreshLongNoteModeClearRankViews();
  return std::nullopt;
}

void MainMenuScene::refreshLongNoteModeClearRankViews() {
  refreshSelectedChartDetails();
  if (folderRecyclerView != nullptr) {
    reloadFolderItems(true);
  }
  if (recyclerView != nullptr) {
    const bool chartListDependsOnScores =
        activeFolder.type == LibraryFolderItem::Type::DifficultyClearMark ||
        chartRecordFilters.clearMarkRank.has_value() ||
        chartRecordFilters.scoreRank.has_value() ||
        chartRecordFilters.sort.criterion == ChartRecordSortCriterion::ClearMark ||
        chartRecordFilters.sort.criterion == ChartRecordSortCriterion::Rate;
    if (chartListDependsOnScores) {
      reloadChartList(true);
    } else {
      recyclerView->rebindVisibleItems();
    }
  }
}

void MainMenuScene::refreshScoreClearRanksIfNeeded() {
  const std::uint64_t revision = context.scoreRepository.GetRevision();
  if (revision == scoreClearRanksRevision) {
    return;
  }

  if (refreshScoreClearRankViews().has_value()) {
    const bool hadVisibleScoreState = scoreClearRanksRevision != 0;
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    if (hadVisibleScoreState) {
      refreshLongNoteModeClearRankViews();
    }
  }
}

void MainMenuScene::refreshIrRecordListIfNeeded() {
  bool recordsNeedRefresh = false;
  const std::uint64_t accountEvidenceRevision =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  if (accountEvidenceRevision != observedIrAccountEvidenceRevision) {
    observedIrAccountEvidenceRevision = accountEvidenceRevision;
    recordsNeedRefresh = true;
  }

  if (context.irSubmissionService != nullptr) {
    const auto status = context.irSubmissionService->reconciliationStatus(
        ir::kTachiProviderId);
    if (status.phase == ir::IrReconciliationPhase::Succeeded &&
        status.revision != 0 &&
        status.revision != observedIrReconciliationRevision) {
      observedIrReconciliationRevision = status.revision;
      recordsNeedRefresh = true;
    }
  }
  if (!recordsNeedRefresh) {
    return;
  }
  if (recordsModal_ != nullptr && recordsModal_->isVisible()) {
    recordsModal_->reloadRecords(true);
  }
}

void MainMenuScene::refreshLibraryIfNeeded() {
  if (archiveUnzipInProgress()) return;
  const std::uint64_t revision =
      context.chartRepository.GetLibraryRevision();
  if (libraryRevision == 0) {
    libraryRevision = revision;
    return;
  }
  if (revision == libraryRevision) {
    return;
  }

  ImageView::dropAllCache();
  reloadScoreClearRanks();
  reloadFolderItems(true);
  reloadChartList(true);
  libraryRevision = revision;
}

int MainMenuScene::clearRankForChart(const ChartMetaRecord &record) const {
  if (record.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course) {
    return clearRankForFolder(activeFolder.key);
  }
  if (record.solidArchive) {
    return kNoClearTypeRank;
  }
  return scoreClearRanks.bestRankFor(
      record.meta, long_note_mode::valueFromId(profileSelections.longNoteMode));
}

int MainMenuScene::clearRankForFolder(const std::string &key) const {
  const int mode = long_note_mode::valueFromId(profileSelections.longNoteMode);
  const auto &clearRanks = folderClearData.clearRanks[static_cast<size_t>(mode)];
  const auto it = clearRanks.find(key);
  return it == clearRanks.end() ? kNoClearTypeRank : it->second;
}

int MainMenuScene::clearMarkCountForFolder(const std::string &key,
                                           int clearMarkRank) const {
  const int mode = long_note_mode::valueFromId(profileSelections.longNoteMode);
  const auto &clearMarkCounts =
      folderClearData.clearMarkCounts[static_cast<size_t>(mode)];
  const auto folderIt = clearMarkCounts.find(key);
  if (folderIt == clearMarkCounts.end()) {
    return 0;
  }
  const auto countIt = folderIt->second.find(clearMarkRank);
  return countIt == folderIt->second.end() ? 0 : countIt->second;
}

void MainMenuScene::requestLibraryReload(bool includeFolders) {
  if (includeFolders) {
    context.chartLibraryFoldersReloadRequested = true;
  }
  context.chartLibraryListReloadRequested = true;
}

void MainMenuScene::applyPendingUiUpdates() {
  if (archiveUnzipInProgress()) return;
  if (context.chartLibraryTasks) {
    for (auto &completion :
         context.chartLibraryTasks->takeDownloadedIndexCompletions()) {
      std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
      pendingFindBmsSelectionHandoff = PendingFindBmsSelectionHandoff{
          .chartPath = std::move(completion.chartPath),
          .targetIdentity = std::move(completion.targetIdentity),
          .selectionGeneration = completion.selectionGeneration,
      };
    }
  }
  const bool shouldOpenTasksModal = tasksModalOpenRequested.exchange(false);
  const bool shouldReloadFolders =
      context.chartLibraryFoldersReloadRequested.exchange(false);
  const bool shouldReloadCharts =
      context.chartLibraryListReloadRequested.exchange(false);
  std::optional<PendingFindBmsSelectionHandoff> findBmsHandoff;
  if (shouldReloadFolders || shouldReloadCharts) {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    findBmsHandoff = std::move(pendingFindBmsSelectionHandoff);
    pendingFindBmsSelectionHandoff.reset();
  }
  std::optional<ChartMetaRecord> findBmsSelectionBeforeReload;
  if (findBmsHandoff.has_value()) {
    findBmsSelectionBeforeReload = selectedRecordSnapshot();
  }
  if (shouldOpenTasksModal) {
    showTasksModal();
  }
  if (shouldReloadFolders) {
    reloadScoreClearRanks();
    reloadFolderItems(true);
  }
  if (shouldReloadFolders || shouldReloadCharts) {
    ImageView::dropAllCache();
    reloadChartList(true);
    libraryRevision = context.chartRepository.GetLibraryRevision();
  }
  if ((shouldReloadFolders || shouldReloadCharts) &&
      pendingSelectChartPath.has_value()) {
    const std::filesystem::path path = *pendingSelectChartPath;
    pendingSelectChartPath.reset();
    selectChartByPathAfterReload(path, AutoSelectionPreview::Suppress);
  }
  if (findBmsHandoff.has_value()) {
    if (findBmsSelectionBeforeReload.has_value() &&
        main_menu_library::findBmsSelectionHandoffAllowed(
            findBmsHandoff->selectionGeneration, chartSelectionGeneration,
            findBmsHandoff->targetIdentity,
            *findBmsSelectionBeforeReload)) {
      selectChartByPathAfterReload(findBmsHandoff->chartPath,
                                   AutoSelectionPreview::Load);
    } else {
      archive_file::appendDebugLogLine(
          "Skipped Find BMS preview handoff because chart selection changed.");
    }
  }
}

void MainMenuScene::selectChartByPathAfterReload(
    const std::filesystem::path &path, AutoSelectionPreview preview) {
  if (recyclerView == nullptr || path.empty() || !chartSession.has_value()) {
    return;
  }
  const path_t target = fspath_to_path_t(path);
  const ChartMetaQuery query = chartQueryForActiveFolder();
  int index = chartSession->FindChartMetaIndex(query, path);
  if (index >= 0 && chartListCache.leadingRecord.has_value()) {
    index += 1;
  }
  if (index >= 0 && index < recyclerView->size()) {
    const ChartMetaRecord record = recyclerView->get(index);
    if (fspath_to_path_t(record.meta.BmsPath) == target) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          previous != index && recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = index;
      recyclerView->scrollOffset =
          main_menu_library::centeredScrollOffsetForItem(
              index, recyclerView->size(), recyclerView->itemHeight,
              recyclerView->getHeight());
      recyclerView->rebindVisibleItems();
      if (preview == AutoSelectionPreview::Suppress) {
        suppressPreviewForChartPath = record.meta.BmsPath;
      } else if (suppressPreviewForChartPath.has_value() &&
                 fspath_to_path_t(*suppressPreviewForChartPath) ==
                     fspath_to_path_t(record.meta.BmsPath)) {
        suppressPreviewForChartPath.reset();
      }
      if (recyclerView->onSelected) {
        recyclerView->onSelected(record, index);
      }
      archive_file::appendDebugLogLine(
          std::string(preview == AutoSelectionPreview::Suppress
                          ? "Selected unzipped chart: "
                          : "Selected indexed Find BMS chart: ") +
          fspath_to_utf8(record.meta.BmsPath));
      return;
    }
  }

  if (preview == AutoSelectionPreview::Load) {
    const std::array requestedPaths{path};
    const auto lookup = chartSession->SelectChartMetaByPaths(requestedPaths);
    const auto record =
        main_menu_library::findBmsUnfilteredHandoffRecord(lookup, path);
    if (record.has_value() && recyclerView->onSelected) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = -1;
      recyclerView->rebindVisibleItems();
      if (suppressPreviewForChartPath.has_value() &&
          fspath_to_path_t(*suppressPreviewForChartPath) == target) {
        suppressPreviewForChartPath.reset();
      }
      recyclerView->onSelected(*record, -1);
      archive_file::appendDebugLogLine(
          "Selected indexed Find BMS chart outside active filters: " +
          fspath_to_utf8(record->meta.BmsPath));
      return;
    }
  }

  if (activeFolder.type != LibraryFolderItem::Type::AllSongs) {
    activeFolder = {
        .key = "all",
        .label = i18n::tr("menu.all_songs.label"),
        .type = LibraryFolderItem::Type::AllSongs,
    };
    reloadFolderItems();
    reloadChartList();
    selectChartByPathAfterReload(path, preview);
  }
}

void MainMenuScene::selectFolder(LibraryFolderItem item) {
  const bool clearedSameFolderScope = clearSameFolderScope();
  auto toggleExpandedFolder = [this](const std::string &key) {
    const auto it = expandedLibraryFolders.find(key);
    if (it == expandedLibraryFolders.end()) {
      expandedLibraryFolders.insert(key);
    } else {
      expandedLibraryFolders.erase(it);
    }
  };

  if (item.type == LibraryFolderItem::Type::CoursesRoot) {
    const std::string previousActiveKey = activeFolder.key;
    toggleExpandedFolder(item.key);
    reloadFolderItems(true);
    if (clearedSameFolderScope || activeFolder.key != previousActiveKey) {
      reloadChartListForFolderSelection();
    }
    return;
  }

  const bool chartQueryUnchanged = activeFolder.key == item.key;
  activeFolder = item;
  refreshChartFilterPanel();
  if (item.expandable) {
    toggleExpandedFolder(item.key);
    reloadFolderItems(true);
  }
  if (item.expandable && chartQueryUnchanged && !clearedSameFolderScope) {
    return;
  }
  reloadChartListForFolderSelection();
}

bool MainMenuScene::toggleChartFavorite(const ChartMetaRecord &record,
                                        bool favorite) {
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return false;
  }

  if (!chartSession.has_value() ||
      !chartSession->SetFavorite(record.meta, favorite)) {
    return false;
  }

  refreshFavoriteFolderCount();
  if (activeFolder.type == LibraryFolderItem::Type::Favorites && !favorite) {
    reloadChartList(true);
  } else if (recyclerView != nullptr) {
    reloadChartList(true);
  }
  libraryRevision = context.chartRepository.GetLibraryRevision();
  return true;
}

std::optional<ChartMetaRecord> MainMenuScene::selectedRecordSnapshot() const {
  if (selectedChartRecord.has_value()) {
    return selectedChartRecord;
  }
  if (recyclerView == nullptr || recyclerView->selectedIndex < 0 ||
      recyclerView->selectedIndex >= recyclerView->size()) {
    return std::nullopt;
  }
  return recyclerView->get(recyclerView->selectedIndex);
}

void MainMenuScene::refreshSelectedChartActionState() {
  refreshSelectedChartDetails();
  refreshRankingsButton();
  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    refreshReplayAvailability(nullptr);
    setPlayableChartActionsVisible(false);
    setUnzipButtonVisible(false);
    setFindBmsButtonVisible(false);
    refreshStartButtonForActiveFolder();
    return;
  }

  refreshReplayAvailability(&*record);
  if (record->courseStart) {
    const bool currentCourseStart =
        activeFolder.type == LibraryFolderItem::Type::Course &&
        activeFolder.courseId > 0;
    setPlayableChartActionsVisible(currentCourseStart, false);
    refreshUnzipButtonForSelection(nullptr);
    setFindBmsButtonVisible(false);
    refreshStartButtonForActiveFolder();
    return;
  }

  setPlayableChartActionsVisible(!record->unavailable &&
                                 !record->solidArchive &&
                                 !record->meta.BmsPath.empty());
  refreshUnzipButtonForSelection(&*record);
  setFindBmsButtonVisible(
      record->unavailable && !record->solidArchive &&
      (!record->meta.SHA256.empty() || !record->meta.MD5.empty() ||
       !record->meta.Title.empty()));
  refreshStartButtonForActiveFolder();
}

void MainMenuScene::refreshRankingsButton() {
  if (rankingsButton == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  const bool enabled =
      context.irRankingService != nullptr && record.has_value() &&
      !record->courseStart && driver != nullptr &&
      driver->capabilities().chartRankings &&
      settings != context.settings.irProviders.end() &&
      settings->second.enabled &&
      ir::makeBokutachiRankingQuery(record->meta).value.has_value();
  rankingsButton->setEnabled(enabled);
}

void MainMenuScene::openRankingsForSelection() {
  if (rankingsButton == nullptr || !rankingsButton->isEnabled() ||
      context.irRankingService == nullptr || overlayPortal == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  if (!record || record->courseStart) {
    return;
  }
  const auto query = ir::makeBokutachiRankingQuery(record->meta);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (!query.value || settings == context.settings.irProviders.end() ||
      !settings->second.enabled) {
    refreshRankingsButton();
    return;
  }

  std::optional<ir::IrLocalComparison> comparison;
  const int selectedLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  const auto best = context.scoreRepository.LoadBestScoreForRuleset(
      record->meta, RulesetDescriptor::For(GameplayRuleset::LR2),
      selectedLongNoteMode);
  if (best) {
    comparison = ir::IrLocalComparison{
        .label = i18n::message("menu.local_pb.label"),
        .score = best->score,
        .maxScore = best->maxScore > 0
                        ? best->maxScore
                        : result_contract::maximumScoreForNotes(
                              record->meta.TotalNotes)
                              .value_or(0),
        .clearType = best->clearType,
        .badPoints = best->badPoints,
        .maxCombo = best->maxCombo,
    };
  }
  if (!rankingsModal) {
    rankingsModal = std::make_unique<ir::IrRankingModal>(
        *overlayPortal, *context.irRankingService);
  }
  rankingsModal->open(
      {.profileId = context.profileManager.activeProfile().id,
       .providerId = std::string(ir::kTachiProviderId),
       .serverOrigin = settings->second.serverOrigin,
       .chart = *query.value,
       .localComparison = std::move(comparison)},
      record->meta.Title.empty() ? i18n::tr("menu.selected_chart.label") : record->meta.Title);
}

MainMenuScene::EffectivePlayOptionSelection
MainMenuScene::currentEffectivePlayOptionSelection() const {
  EffectivePlayOptionSelection selection;
  selection.playOption =
      play_options::normalizePlayOption(profileSelections.playOption);
  selection.longNoteMode = long_note_mode::parseId(
      profileSelections.longNoteMode, AppSettings::kDefaultLnMode);
  selection.assistOption =
      assist_options::normalize(profileSelections.assistOption);

  const auto record = selectedRecordSnapshot();
  const bool selectedCourseStart =
      record.has_value() && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (selectedCourseStart) {
    const CourseConstraintSettings constraintSettings =
        courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
    if (coursePlayOptionLocksSelection(constraintSettings)) {
      selection.playOption = coursePlayOptionForConstraints(
          profileSelections.playOption, constraintSettings);
    }
    if (constraintSettings.rules.longNoteMode !=
        CourseLongNoteMode::Unspecified) {
      selection.longNoteMode = longNoteModeOptionFromCourseConstraint(
          constraintSettings.rules.longNoteMode);
      selection.longNoteModeLocked = true;
    }
    selection.assistOption = assist_options::kOff;
    selection.assistOptionLocked = true;
    return selection;
  }

  if (record.has_value()) {
    const int chartLnMode =
        normalizeChartLongNoteModeValue(record->meta.LnMode);
    if (chartLnMode > 0) {
      selection.longNoteMode =
          long_note_mode::idFromValue(chartLnMode, AppSettings::kDefaultLnMode);
      selection.longNoteModeLocked = true;
    }
  }

  return selection;
}

bool MainMenuScene::currentPlayOptionSelectionAllowed(
    const std::string &option) const {
  const auto record = selectedRecordSnapshot();
  const bool selectedCourseStart =
      record.has_value() && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (!selectedCourseStart) {
    return true;
  }

  const CourseConstraintSettings constraintSettings =
      courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
  return coursePlayOptionAllowedByConstraints(option, constraintSettings);
}

bool MainMenuScene::currentLongNoteModeSelectionAllowed(
    const std::string &mode) const {
  const EffectivePlayOptionSelection selection =
      currentEffectivePlayOptionSelection();
  if (!selection.longNoteModeLocked) {
    return true;
  }
  return long_note_mode::parseId(mode, AppSettings::kDefaultLnMode) ==
         selection.longNoteMode;
}

bool MainMenuScene::currentAssistOptionSelectionAllowed(
    const std::string &option) const {
  const EffectivePlayOptionSelection selection =
      currentEffectivePlayOptionSelection();
  if (!selection.assistOptionLocked) {
    return true;
  }
  return assist_options::normalize(option) == selection.assistOption;
}

void MainMenuScene::setGameplayRulesetSelection(GameplayRuleset ruleset) {
  const main_menu_profile::Selections previousSelections = profileSelections;
  const AppSettings previousSettings = context.settings;
  profileSelections.ruleset = ruleset;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  std::string errorMessage;
  if (!context.saveSettings(&errorMessage)) {
    profileSelections = previousSelections;
    context.settings = previousSettings;
    SDL_Log("Failed to save gameplay ruleset selection: %s",
            errorMessage.empty() ? "unknown error" : errorMessage.c_str());
  }
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setGaugeSelection(GaugeType gaugeType,
                                      GaugeAutoShiftMode autoShift) {
  profileSelections.gaugeType = gaugeType;
  profileSelections.gaugeAutoShift = autoShift;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gauge selection");
  }
  refreshGaugeSelectionButtons();
}

void MainMenuScene::setGaugeAutoShiftLowerBound(GaugeType gaugeType) {
  profileSelections.gaugeAutoShiftLowerBound = gaugeType;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gauge auto shift lower bound");
  }
  refreshGaugeSelectionButtons();
}

void MainMenuScene::refreshGaugeSelectionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlayOptionSelection(const std::string &option) {
  if (!currentPlayOptionSelectionAllowed(option)) {
    return;
  }
  profileSelections.playOption = play_options::normalizePlayOption(option);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save play option selection");
  }
  refreshPlayOptionButtons();
}

void MainMenuScene::refreshPlayOptionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setLongNoteModeSelection(const std::string &mode) {
  if (!currentLongNoteModeSelectionAllowed(mode)) {
    return;
  }
  const std::string previousMode = profileSelections.longNoteMode;
  profileSelections.longNoteMode =
      long_note_mode::parseId(mode, AppSettings::kDefaultLnMode);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save long note mode selection");
  }
  refreshLongNoteModeButtons();
  if (profileSelections.longNoteMode != previousMode) {
    refreshLongNoteModeClearRankViews();
  }
}

void MainMenuScene::refreshLongNoteModeButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setAssistOptionSelection(const std::string &option) {
  if (!currentAssistOptionSelectionAllowed(option)) {
    return;
  }
  profileSelections.assistOption = assist_options::normalize(option);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save assist option selection");
  }
  refreshAssistOptionButtons();
}

void MainMenuScene::refreshAssistOptionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPacemakerTargetSelection(const std::string &target) {
  profileSelections.pacemakerTarget = pacemaker::normalizeTargetId(target);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save pacemaker target selection");
  }
  refreshPacemakerTargetButtons();
}

void MainMenuScene::refreshPacemakerTargetButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlaybackRateSelection(int percent) {
  if (playbackSelectionLockedForCourse()) {
    return;
  }
  context.settings.selectedPlaybackRatePercent = percent;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save playback rate selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::setPlaybackModeSelection(const std::string &mode) {
  if (playbackSelectionLockedForCourse() || mode != "pitch-shift") {
    return;
  }
  context.settings.selectedPlaybackMode = audio::PlaybackMode::PitchShift;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save playback mode selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::toggleGameplayClubMode() {
  context.settings.gameplayClubModeEnabled =
      !context.settings.gameplayClubModeEnabled;
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gameplay Club mode selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::refreshPlaybackSelectionControls() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::refreshPlayOptionsPanel() {
  if (playOptionsPanel == nullptr) {
    return;
  }
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  const bool playbackLocked = playbackSelectionLockedForCourse();
  const int playbackRate =
      playbackLocked ? course_rules::kRequiredPlaybackRate.percent
                     : context.settings.selectedPlaybackRatePercent;
  playOptionsPanel->refresh(
      {.ruleset = profileSelections.ruleset,
       .gaugeType = profileSelections.gaugeType,
       .gaugeAutoShift = profileSelections.gaugeAutoShift,
       .gaugeAutoShiftLowerBound =
           profileSelections.gaugeAutoShiftLowerBound,
       .playOption = effective.playOption,
       .defaultLaneOrder = {},
       .laneOrderEnabled = false,
       .longNoteMode = effective.longNoteMode,
       .longNoteModeLocked = effective.longNoteModeLocked,
       .assistOption = effective.assistOption,
       .assistOptionLocked = effective.assistOptionLocked,
       .playbackRatePercent = playbackRate,
       .playbackLocked = playbackLocked,
       .clubMode = context.settings.gameplayClubModeEnabled,
       .pacemakerTarget = profileSelections.pacemakerTarget,
       .profileId = context.profileManager.activeProfile().id});
}

bool MainMenuScene::playbackSelectionLockedForCourse() const {
  const auto record = selectedRecordSnapshot();
  return record.has_value() && record->courseStart &&
         activeFolder.type == LibraryFolderItem::Type::Course &&
         activeFolder.courseId > 0;
}

void MainMenuScene::refreshSelectedChartDetails() {
  if (chartDetailsView_ == nullptr) return;
  // onSelected retains metadata by value. Never fetch a page, query score
  // history, or wait for preview parsing to populate this presentation.
  const auto *record = selectedChartRecord ? &*selectedChartRecord : nullptr;
  const bool chart = record && !record->courseStart && !record->solidArchive &&
                     !record->unavailable && !record->meta.BmsPath.empty();
  const auto best = chart ? scoreBestScores.bestFor(
      record->meta, long_note_mode::valueFromId(profileSelections.longNoteMode))
      : std::nullopt;
  chartDetailsView_->setChart(record, best,
      chart ? clearRankForChart(*record) : kNoClearTypeRank,
      chart ? formatGaugeTotal(record->meta, profileSelections.ruleset) : "");
}

void MainMenuScene::refreshReadySettingsSummary() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  if (readyGaugeText != nullptr) {
    readyGaugeText->setText(gaugeButtonLabel(profileSelections.gaugeType,
                                             profileSelections.gaugeAutoShift));
    readyGaugeText->setColor(readyGaugeTextColor(
        profileSelections.gaugeType, profileSelections.gaugeAutoShift));
  }
  if (readyPlayOptionText != nullptr) {
    readyPlayOptionText->setText(
        std::string(gameplayRulesetLabel(profileSelections.ruleset)) + " · " +
        effective.playOption + " · " + effective.longNoteMode);
  }
  refreshSelectedChartDetails();
  if (readyAssistOptionText != nullptr) {
    const int percent =
        playbackSelectionLockedForCourse()
            ? course_rules::kRequiredPlaybackRate.percent
            : context.settings.selectedPlaybackRatePercent;
    const bool optionEnabled =
        assist_options::isEnabled(effective.assistOption);
    if (!optionEnabled && percent == 100) {
      readyAssistOptionText->setLocalizedText(i18n::message("menu.assist_off.label"));
    } else {
      std::string reasons;
      if (optionEnabled) {
        reasons = effective.assistOption;
      }
      if (percent != 100) {
        if (!reasons.empty()) {
          reasons += "/";
        }
        reasons += std::to_string(percent) + "%";
      }
      readyAssistOptionText->setText(i18n::tr("menu.assist.prefix") + reasons);
    }
  }
  if (readyPacemakerText != nullptr) {
    readyPacemakerText->setText(
        i18n::tr("menu.target.prefix") +
        pacemaker::displayTargetLabel(profileSelections.pacemakerTarget));
  }
}

const MainMenuScene::CourseValidationCache &
MainMenuScene::courseValidationForActiveFolder() {
  const std::uint64_t currentLibraryRevision =
      context.chartRepository.GetLibraryRevision();
  if (courseValidationCache.valid &&
      courseValidationCache.libraryRevision == currentLibraryRevision &&
      courseValidationCache.courseId == activeFolder.courseId) {
    return courseValidationCache;
  }

  courseValidationCache = CourseValidationCache{};
  courseValidationCache.valid = true;
  courseValidationCache.libraryRevision = currentLibraryRevision;
  courseValidationCache.courseId = activeFolder.courseId;

  if (activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0 || !chartSession.has_value()) {
    return courseValidationCache;
  }

  ChartMetaQuery query;
  query.courseId = activeFolder.courseId;
  chartSession->QueryChartMeta(query, courseValidationCache.records);
  courseValidationCache.empty = courseValidationCache.records.empty();
  for (int i = 0;
       i < static_cast<int>(courseValidationCache.records.size()); ++i) {
    const auto &record =
        courseValidationCache.records[static_cast<std::size_t>(i)];
    if (record.solidArchive || record.unavailable ||
        record.meta.BmsPath.empty()) {
      courseValidationCache.firstMissingIndex = i;
      break;
    }
  }
  return courseValidationCache;
}

std::optional<MainMenuScene::CurrentCourseSelection>
MainMenuScene::currentCourseSelectionFor(
    const result_persistence::ModernCourseResult &result) {
  if (activeFolder.type != LibraryFolderItem::Type::Course) {
    return std::nullopt;
  }
  return course_records::currentCourseSelectionFor(
      activeFolder.courseKey, courseValidationForActiveFolder().records, result);
}

void MainMenuScene::refreshStartButtonForActiveFolder() {
  if (startButtonText == nullptr || willStart.load()) {
    return;
  }
  if (activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0) {
    startButtonText->setLocalizedText(i18n::message("library.folders.start.label"));
    return;
  }
  const auto selectedRecord = selectedRecordSnapshot();
  if (selectedRecord.has_value() && !selectedRecord->courseStart) {
    startButtonText->setLocalizedText(i18n::message("library.folders.start.label"));
    return;
  }

  const CourseValidationCache &validation = courseValidationForActiveFolder();
  if (validation.empty) {
    startButtonText->setLocalizedText(i18n::message("library.folders.no_course.label"));
    return;
  }

  startButtonText->setLocalizedText(validation.firstMissingIndex >= 0 ? i18n::message("library.folders.missing.label")
                                                             : i18n::message("library.folders.start_course.label"));
}

void MainMenuScene::startSelectedCourse() {
  if (willStart.load() || archiveUnzipInProgress() ||
      pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      recyclerView == nullptr ||
      activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0) {
    return;
  }

  const CourseValidationCache &validation = courseValidationForActiveFolder();
  if (validation.empty) {
    refreshStartButtonForActiveFolder();
    return;
  }

  const auto &records = validation.records;
  const int firstMissingIndex = validation.firstMissingIndex;
  if (firstMissingIndex >= 0) {
    int visibleMissingIndex = -1;
    const auto &missingRecord =
        records[static_cast<std::size_t>(firstMissingIndex)];
    if (!missingRecord.meta.BmsPath.empty()) {
      const int databaseIndex = chartSession.has_value()
                                    ? chartSession->FindChartMetaIndex(
                                          chartQueryForActiveFolder(),
                                          missingRecord.meta.BmsPath)
                                    : -1;
      if (databaseIndex >= 0) {
        visibleMissingIndex = databaseIndex + 1;
      }
    } else if (searchText.empty()) {
      visibleMissingIndex = firstMissingIndex + 1;
    }
    if (visibleMissingIndex >= 0) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          previous != visibleMissingIndex && recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = visibleMissingIndex;
      recyclerView->rebindVisibleItems();
      if (recyclerView->onSelected) {
        recyclerView->onSelected(recyclerView->get(visibleMissingIndex),
                                 visibleMissingIndex);
      }
    }
    refreshStartButtonForActiveFolder();
    return;
  }

  auto session = buildCourseGameplaySession(
      {.courseId = activeFolder.courseId,
       .courseKey = activeFolder.courseKey,
       .courseName = activeFolder.courseGroupName.empty()
                         ? activeFolder.label
                         : activeFolder.courseGroupName + " " +
                               activeFolder.label,
       .courseGroupName = activeFolder.courseGroupName,
       .constraintJson = activeFolder.courseConstraintJson,
       .records = records,
       .selections = profileSelections,
       .player2PlayOption = std::string(
           replay::beatorajaReplayOptionName(
               context.settings.skinPlayer2RandomOption)
               .value_or("NORMAL")),
       .doublePlayFlip = context.settings.skinDoublePlayOption == 1,
       .inputKeysoundEnabled = context.settings.inputKeysoundEnabled});
  startCourseDirect(std::move(session));
}

void MainMenuScene::startCourseDirect(
    std::shared_ptr<CoursePlaySession> session) {
  if (session == nullptr || session->entries.empty() ||
      willStart.exchange(true)) {
    return;
  }

  if (startButtonText != nullptr) {
    startButtonText->setLocalizedText(i18n::message("menu.loading.progress"));
  }
  ImageView::dropAllCache();
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  const int selectedLongNoteMode =
      normalizeChartLongNoteModeValue(session->longNoteMode) > 0
          ? normalizeChartLongNoteModeValue(session->longNoteMode)
          : long_note_mode::valueFromId(profileSelections.longNoteMode);
  session->longNoteMode = selectedLongNoteMode;
  if (!session->courseReplayPlayback) {
    session->assistOption = assist_options::kOff;
  }

  defer(
      [this, session, selectedLongNoteMode]() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
        if (context.gameplaySkinLifecycle &&
            !context.gameplaySkinLifecycle->presentationReady()) {
          return false;
        }
#endif
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }
        clearSelectedChart();

        const bms_parser::ChartMeta *firstMeta = session->currentMeta();
        if (firstMeta == nullptr || firstMeta->BmsPath.empty()) {
          return finishStart();
        }

        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> preparedChart;
        try {
          preparedChart = play_options::parseChart(firstMeta->BmsPath,
                                                   parseCancelled, "course");
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for course start: %s",
                  fspath_to_utf8(firstMeta->BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Course start parse exception: " +
              fspath_to_utf8(firstMeta->BmsPath) + ": " + e.what());
        }
        if (preparedChart == nullptr || parseCancelled) {
          finishStart();
          if (startButtonText != nullptr) {
            startButtonText->setLocalizedText(i18n::message("menu.course_start_failed.label"));
          }
          return true;
        }
        applyCourseConstraintsToChart(*preparedChart, session->constraints);

        play_options::PlayOptionReplayInfo playInfo =
            play_options::applySelectedPlayOptions(
                *preparedChart, session->requestedPlayOption,
                session->requestedPlayOption2, session->doublePlayFlip);
        applyEffectiveLongNoteModeToChart(*preparedChart,
                                          selectedLongNoteMode);
        session->playOption = playInfo.option;
        session->playOptionSeed = playInfo.seed;
        session->playOption2 = playInfo.option2;
        session->playOption2Seed = playInfo.seed2;

        context.jukebox.stop();
        context.jukebox.loadChart(*preparedChart, true, parseCancelled);
        if (parseCancelled) {
          return finishStart();
        }

        StartOptions options;
        options.startPosition = 0;
        options.autoKeySound = session->autoKeySound;
        options.autoPlay = false;
        options.gaugeType = session->gaugeType;
        options.gaugeProfile = session->gaugeProfile;
        options.gaugeAutoShift = session->gaugeAutoShift;
        options.gaugeAutoShiftLowerBound =
            session->gaugeAutoShiftLowerBound;
        options.playOption = playInfo.option;
        options.playOptionSeed = playInfo.seed;
        options.playOption2 = playInfo.option2;
        options.playOption2Seed = playInfo.seed2;
        options.doublePlayFlip = session->doublePlayFlip;
        options.longNoteMode = selectedLongNoteMode;
        options.assistOption = session->assistOption;
        options.playback = course_rules::kRequiredPlaybackRate;
        options.clubMode = context.settings.gameplayClubModeEnabled;
        options.courseSession = session;
        options.courseConstraints = session->constraints;
        options.ruleset = session->ruleset;
        options.requiredRulesetDescriptor = session->rulesetDescriptor;
        options.ownsChart = true;

        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(context, std::move(preparedChart),
                                            std::move(options)),
            true);
        return finishStart();
      },
      0, true);
}

void MainMenuScene::startSelectedChart() {
  if (willStart.load() || archiveUnzipInProgress() ||
      pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }
  if (record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    return;
  }
  startChartDirect(*record);
}

void MainMenuScene::startChartDirect(const ChartMetaRecord &record) {
  if (willStart.exchange(true)) {
    return;
  }

  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    resetStartLoadingUi();
    return;
  }

  if (startButtonText != nullptr) {
    startButtonText->setLocalizedText(i18n::message("menu.loading.progress"));
  }
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setChart(record);
    decideOverlay_->setVisible(true);
  }
  ImageView::dropAllCache();

  const GaugeType gaugeType = profileSelections.gaugeType;
  const GaugeAutoShiftMode gaugeAutoShift = profileSelections.gaugeAutoShift;
  const GaugeType gaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  const GameplayRuleset ruleset = profileSelections.ruleset;
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const std::string playOption = profileSelections.playOption;
  int selectedLongNoteMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
  if (selectedLongNoteMode == 0) {
    selectedLongNoteMode =
        long_note_mode::valueFromId(profileSelections.longNoteMode);
  }
  const std::string assistOption = profileSelections.assistOption;
  const std::string pacemakerTarget =
      pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const std::string normalizedPlayOption =
      play_options::normalizePlayOption(playOption);
  const bool canReusePreviewForStart =
      normalizedPlayOption.empty() || normalizedPlayOption == "NORMAL";
  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);
  std::string tableName;
  std::string tableLevel;
  if ((activeFolder.type == LibraryFolderItem::Type::DifficultyTable ||
       activeFolder.type == LibraryFolderItem::Type::DifficultyLevel ||
       activeFolder.type == LibraryFolderItem::Type::DifficultyClearMark) &&
      activeFolder.tableId > 0) {
    const auto table = std::ranges::find_if(
        folderMetadataCache.tables, [this](const DifficultyTableInfo &item) {
          return item.id == activeFolder.tableId;
        });
    if (table != folderMetadataCache.tables.end()) {
      tableName = table->name;
      if (!activeFolder.tableLevel.empty()) {
        // TableDataAccessor builds HashBar titles as TableData.tag + level.
        tableLevel = table->symbol + activeFolder.tableLevel;
      }
    }
  }

  defer(
      [this, record, gaugeType, gaugeAutoShift, gaugeAutoShiftLowerBound,
       ruleset, autoKeySound, playOption, selectedLongNoteMode, assistOption,
       pacemakerTarget, playback,
       canReusePreviewForStart, chartRandomInfo, tableName = std::move(tableName),
       tableLevel = std::move(tableLevel)]() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
        if (context.gameplaySkinLifecycle &&
            !context.gameplaySkinLifecycle->presentationReady()) {
          return false;
        }
#endif
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (!canReusePreviewForStart) {
          if (previewWorker_ != nullptr) {
            previewWorker_->cancel();
          }
        }
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }

        bms_parser::Chart *readyChart = nullptr;
        if (canReusePreviewForStart) {
          readyChart = loadedSelectedChartForPath(record.meta.BmsPath);
        }
        if (readyChart != nullptr) {
          archive_file::appendDebugLogLine(
              "Start reusing loaded preview chart: " +
              fspath_to_utf8(record.meta.BmsPath));
          applyEffectiveLongNoteModeToChart(*readyChart,
                                            selectedLongNoteMode);
          context.jukebox.stop();
          changeToGameplayScene(readyChart,
                                {
                                    .startPosition = 0,
                                    .autoKeySound = autoKeySound,
                                    .autoPlay = false,
                                    .gaugeType = gaugeType,
                                    .gaugeAutoShift = gaugeAutoShift,
                                    .gaugeAutoShiftLowerBound =
                                        gaugeAutoShiftLowerBound,
                                    .longNoteMode = selectedLongNoteMode,
                                    .assistOption = assistOption,
                                    .pacemakerTarget = pacemakerTarget,
                                    .tableName = tableName,
                                    .tableLevel = tableLevel,
                                    .playback = playback,
                                    .ruleset = ruleset,
                                });
          return finishStart();
        }

        if (previewWorker_ != nullptr) {
          previewWorker_->cancel();
        }
        selectedChartMediaReady.store(false);
        selectedChartReusableForStart.store(false);
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> preparedChart;
        try {
          preparedChart = play_options::parseChart(
              record.meta.BmsPath, chartRandomInfo.seed, chartRandomInfo.prng,
              chartRandomInfo.values, parseCancelled);
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for start: %s",
                  fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Start parse exception: " +
              fspath_to_utf8(record.meta.BmsPath) + ": " + e.what());
        }
        if (preparedChart != nullptr && !parseCancelled) {
          play_options::PlayOptionReplayInfo playInfo =
              play_options::applySelectedPlayOptions(*preparedChart,
                                                     playOption);
          applyEffectiveLongNoteModeToChart(*preparedChart,
                                            selectedLongNoteMode);
          context.jukebox.stop();
          context.jukebox.loadChart(*preparedChart, true, parseCancelled);
          bms_parser::Chart *loadedChart = nullptr;
          if (!parseCancelled) {
            loadedChart = setSelectedChart(
                std::move(preparedChart), true,
                play_options::isNormalPlayOption(playOption));
          }
          if (parseCancelled) {
            preparedChart.reset();
          } else {
            changeToGameplayScene(loadedChart,
                                  {
                                      .startPosition = 0,
                                      .autoKeySound = autoKeySound,
                                      .autoPlay = false,
                                      .gaugeType = gaugeType,
                                      .gaugeAutoShift = gaugeAutoShift,
                                      .gaugeAutoShiftLowerBound =
                                          gaugeAutoShiftLowerBound,
                                      .playOption = playInfo.option,
                                      .playOptionSeed = playInfo.seed,
                                      .playOption2 = playInfo.option2,
                                      .playOption2Seed = playInfo.seed2,
                                      .longNoteMode = selectedLongNoteMode,
                                      .assistOption = assistOption,
                                      .pacemakerTarget = pacemakerTarget,
                                      .tableName = tableName,
                                      .tableLevel = tableLevel,
                                      .playback = playback,
                                      .ruleset = ruleset,
                                  });
            return finishStart();
          }
        }

        auto *chart = loadedSelectedChartForPath(record.meta.BmsPath);
        if (chart == nullptr) {
          return finishStart();
        }

        context.jukebox.stop();
        applyEffectiveLongNoteModeToChart(*chart, selectedLongNoteMode);
        changeToGameplayScene(chart, {
                                         .startPosition = 0,
                                         .autoKeySound = autoKeySound,
                                         .autoPlay = false,
                                         .gaugeType = gaugeType,
                                         .gaugeAutoShift = gaugeAutoShift,
                                         .gaugeAutoShiftLowerBound =
                                             gaugeAutoShiftLowerBound,
                                         .longNoteMode = selectedLongNoteMode,
                                         .assistOption = assistOption,
                                         .pacemakerTarget = pacemakerTarget,
                                         .tableName = tableName,
                                         .tableLevel = tableLevel,
                                         .playback = playback,
                                         .ruleset = ruleset,
                                     });
        return finishStart();
      },
      0, true);
}

void MainMenuScene::openChartViewerForSelection() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      archiveUnzipInProgress() || pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }

  if (record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    return;
  }
  openChartViewerDirect(*record);
}

void MainMenuScene::openChartViewerDirect(const ChartMetaRecord &record) {
  if (willStart.load() || replayExportJob_.inProgress() ||
      record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    return;
  }

  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  archive_file::appendDebugLogLine(
      "Open chart viewer: " + fspath_to_utf8(record.meta.BmsPath));
  context.jukebox.stop();
  context.sceneManager->changeScene(
      std::make_unique<ChartViewerScene>(context, record, chartRandomInfo.seed,
                                         chartRandomInfo.prng,
                                         chartRandomInfo.values),
      true);
}

void MainMenuScene::toggleRevealContextMenu() {
  if (revealContextMenu == nullptr || revealButton == nullptr) {
    return;
  }
  if (revealContextMenu->isOpen()) {
    revealContextMenu->dismiss();
    return;
  }
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }

  const auto record = selectedRecordSnapshot();
  if (!record.has_value() || record->courseStart || record->solidArchive ||
      record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }

  const bool canShowSameFolder =
      main_menu_library::sameFolderForChart(*record).has_value();
  revealContextMenu->setViewportSize(rendering::window_width,
                                     rendering::window_height);
  revealContextMenu->show(
      {.x = revealButton->getX(),
       .y = revealButton->getY(),
       .width = revealButton->getWidth(),
       .height = revealButton->getHeight()},
      {{.id = "show-same-folder",
        .label = i18n::message("menu.show_same_folder.label"),
        .enabled = canShowSameFolder},
       {.id = "reveal-file", .label = i18n::message("menu.reveal_file.label")}},
      220);
}

void MainMenuScene::showSelectedChartFolder() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  if (!record.has_value() || record->courseStart || record->solidArchive ||
      record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }
  const auto folder = main_menu_library::sameFolderForChart(*record);
  if (!folder.has_value()) {
    return;
  }

  temporaryChartFolder = *folder;
  searchText.clear();
  if (searchBox != nullptr) {
    searchBox->setEditingText("");
  }
  chartRecordFilters =
      main_menu_library::filtersForSameFolder(chartRecordFilters);
  chartBpmMinText.clear();
  chartBpmMaxText.clear();
  chartClearMarkDropdownOpen = false;
  chartScoreRankDropdownOpen = false;
  chartDifficultyMinDropdownOpen = false;
  chartDifficultyMaxDropdownOpen = false;
  chartDifficultyRangeTableId.reset();

  if (folderRecyclerView != nullptr) {
    const int selectedIndex = folderRecyclerView->selectedIndex;
    if (selectedIndex >= 0 && selectedIndex < folderRecyclerView->size() &&
        folderRecyclerView->onUnselected) {
      folderRecyclerView->onUnselected(folderRecyclerView->get(selectedIndex),
                                       selectedIndex);
    }
    folderRecyclerView->selectedIndex = -1;
    folderRecyclerView->rebindVisibleItems();
  }

  refreshChartFilterPanel();
  reloadChartList(true);
  if (recyclerView->selectedIndex >= 0) {
    recyclerView->scrollOffset =
        main_menu_library::centeredScrollOffsetForItem(
            recyclerView->selectedIndex, recyclerView->size(),
            recyclerView->itemHeight, recyclerView->getHeight());
    recyclerView->rebindVisibleItems();
  }
}

bool MainMenuScene::clearSameFolderScope() {
  if (!temporaryChartFolder.has_value()) {
    return false;
  }
  temporaryChartFolder.reset();
  return true;
}

void MainMenuScene::revealSelectedChartInFileManager() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr || revealButton == nullptr) {
    return;
  }

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }

  if (record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }

  std::string errorMessage;
  const OverlayAnchor sourceAnchor{
      .x = revealButton->getX(),
      .y = revealButton->getY(),
      .width = revealButton->getWidth(),
      .height = revealButton->getHeight(),
  };
  const auto normalized = normalizeOverlayAnchor(
      sourceAnchor, rendering::window_width, rendering::window_height);
  if (!platform_open::revealPathInFileManager(
          record->meta.BmsPath,
          {.x = normalized.x,
           .y = normalized.y,
           .width = normalized.width,
           .height = normalized.height},
          errorMessage)) {
    SDL_Log("Failed to reveal chart file %s: %s",
            fspath_to_utf8(record->meta.BmsPath).c_str(),
            errorMessage.c_str());
  }
}

void MainMenuScene::reselectCurrentChart() {
  if (recyclerView == nullptr || !recyclerView->onSelected) {
    return;
  }
  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  recyclerView->onSelected(record, selected);
}

void MainMenuScene::refreshReplayAvailability(const ChartMetaRecord *record) {
  if (record != nullptr && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      (!activeFolder.courseKey.empty() || activeFolder.courseId > 0)) {
    const auto courseRecords = context.replayRepository.ListLegacyCourseSummaries(
        {.courseKey = activeFolder.courseKey,
         .legacyCourseId = activeFolder.courseId});
    bool hasModernRecords = false;
    if (!activeFolder.courseKey.empty()) {
      const auto modern = context.replayRepository.ListModernCourseResults(
          activeFolder.courseKey, 1);
      hasModernRecords =
          modern.status == ModernCourseHistoryReadStatus::Loaded &&
          !modern.records.empty();
    }
    setReplayButtonVisible(!courseRecords.empty() || hasModernRecords);
    return;
  }

  if (record == nullptr || record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    setReplayButtonVisible(false);
    return;
  }

  setReplayButtonVisible(true);
}

void MainMenuScene::setReplayButtonVisible(bool visible) {
  if (replayButtonSlot == nullptr) {
    return;
  }

  replayButton->setEnabled(visible);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::setPlayableChartActionsVisible(bool visible) {
  setPlayableChartActionsVisible(visible, visible);
}

void MainMenuScene::setPlayableChartActionsVisible(bool visible,
                                                   bool chartActionsVisible) {
  if (startButton != nullptr) {
    startButton->setEnabled(visible);
  }
  if (chartActionsRow != nullptr) {
    const bool showChartActions = visible && chartActionsVisible;
    for (auto *child : chartActionsRow->getChildren()) {
      if (auto *button = dynamic_cast<Button *>(child)) {
        button->setEnabled(showChartActions);
      }
    }
    if (!showChartActions && revealContextMenu != nullptr) {
      revealContextMenu->dismiss();
    }
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::setUnzipButtonVisible(bool visible) {
  if (unzipButtonSlot == nullptr) {
    return;
  }

  unzipButtonSlot->setVisible(visible);
  unzipButtonSlot->setHeight(visible ? currentMenuActionHeight() : 0.0f);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::refreshUnzipButtonForSelection(
    const ChartMetaRecord *record) {
  bool visible = false;
  if (record != nullptr && !record->unavailable &&
      (record->unzipAll || !record->meta.BmsPath.empty())) {
    visible = record->solidArchive;
  }
  if (unzipButtonText != nullptr) {
    unzipButtonText->setLocalizedText(record != nullptr && record->unzipAll
                                ? i18n::message("library.archive.unzip_all.label") : i18n::message("library.archive.unzip.label"));
  }
  setUnzipButtonVisible(visible);
}

void MainMenuScene::startUnzipSelectedArchiveFolder() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      archiveUnzipInProgress() || pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  startUnzipArchiveFolder(record);
}

void MainMenuScene::startUnzipArchiveFolder(const ChartMetaRecord &record) {
  if (willStart.load() || replayExportJob_.inProgress() ||
      archiveUnzipInProgress() || pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      archiveUnzipModal_ == nullptr || record.unavailable ||
      (!record.unzipAll && record.meta.BmsPath.empty()) || !record.solidArchive ||
      archive_file::isVirtualPath(record.meta.BmsPath)) {
    return;
  }
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  stopAndClearSelectedChart();
  if (record.unzipAll) {
    archiveUnzipModal_->startAll();
  } else {
    archiveUnzipModal_->start(record);
  }
}

bool MainMenuScene::archiveUnzipInProgress() const {
  return archiveUnzipModal_ != nullptr && archiveUnzipModal_->inProgress();
}

void MainMenuScene::buildUnzipProgressModal() {
  ArchiveUnzipModalCallbacks callbacks;
  callbacks.libraryChanged = [this]() { requestLibraryReload(true); };
  callbacks.finished = [this](const ArchiveUnzipResult &result) {
    if (result.success && !result.chartPath.empty()) {
      pendingSelectChartPath = result.chartPath;
    }
    archive_file::appendDebugLogLine(
        result.message.resolve() + (result.chartPath.empty()
                              ? ""
                              : ": " + fspath_to_utf8(result.chartPath)));
  };
  archiveUnzipModal_ = ArchiveUnzipModal::Create(
      rootLayout, context.chartRepository, std::move(callbacks));
}

void MainMenuScene::startLibraryRefresh() {
  if (willStart.load() || replayExportJob_.inProgress()) {
    return;
  }
  enqueueLibraryRefreshTask(i18n::message("menu.refresh_library.label"));
}

void MainMenuScene::startLibraryRebuild() {
  if (willStart.load() || replayExportJob_.inProgress()) {
    return;
  }
  enqueueLibraryRefreshTask(i18n::message("menu.rebuild_library.label"), std::filesystem::path(), "",
                            true);
  tasksModalOpenRequested.store(true);
}

void MainMenuScene::setFindBmsButtonVisible(bool visible) {
  findBmsAvailableWithoutTutorial_ = visible;
  visible = visible || (tutorial_ && tutorial_->getVisible() &&
                       tutorial_->step() == NewcomerTutorialStep::Download);
  if (findBmsButtonSlot == nullptr) {
    return;
  }

  findBmsButtonSlot->setVisible(visible);
  findBmsButtonSlot->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
  const int primaryHeight = rendering::window_height > rendering::window_width
                                ? kPortraitMenuActionHeight : 88;
  findBmsButtonSlot->setHeight(visible ? primaryHeight : 0);
  if (startButton != nullptr) {
    startButton->setVisible(!visible);
    startButton->setDisplay(visible ? YGDisplayNone : YGDisplayFlex);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::openFindBmsForSelection() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || !record.unavailable ||
      (record.meta.SHA256.empty() && record.meta.MD5.empty() &&
       record.meta.Title.empty())) {
    return;
  }
  showFindBmsModal(record);
}

std::filesystem::path MainMenuScene::preferredBmsDownloadRoot() {
  return findBmsDownloadRoot(chartSession ? &*chartSession : nullptr);
}

#if TARGET_OS_ANDROID
void MainMenuScene::buildFileActionsModal() {
  if (rootLayout == nullptr) return;

  // The scene view tree owns all panels for their entire lifetime. Switching
  // panels only changes visibility, including inside button callbacks.
  fileActionsModalRoot_ = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  fileActionsModalRoot_->setPositionType(YGPositionTypeAbsolute);
  fileActionsModalRoot_->setPosition(Edge::Left, 0);
  fileActionsModalRoot_->setPosition(Edge::Top, 0);
  fileActionsModalRoot_->setZIndex(1000);
  fileActionsModalRoot_->setVisible(false);
  fileActionsModalRoot_->setFlexDirection(FlexDirection::Column);
  fileActionsModalRoot_->setAlignItems(YGAlignCenter);
  fileActionsModalRoot_->setJustifyContent(YGJustifyCenter);
  fileActionsModalRoot_->setThemedBackgroundColor(ui_theme::scrim);

  auto makePanel = [this](const char *titleKey, const char *introKey,
                          const char *footerKey,
                          std::function<void()> onClose, View **panelOut,
                          bool showComputerHelp = false) {
    auto *panel = new View();
    panel->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)
        ->setGap(14)
        ->setPadding(Edge::All, 20)
        ->setThemedBackgroundColor(ui_theme::panelStrong)
        ->setCornerRadius(ui_theme::panelRadius())
        ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
        ->setThemedBorderColor(modal_view::modalPanelBorder)
        ->setBorderWidth(1);
    auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    title->setLocalizedText(i18n::message(titleKey));
    title->setThemedColor(ui_theme::textPrimary);
    title->setWrap(true);
    title->setFlexShrink(0);
    panel->addView(title);

    auto *intro = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
    intro->setLocalizedText(i18n::message(introKey));
    intro->setThemedColor(ui_theme::textSecondary);
    intro->setWrap(true);
    intro->setFlexShrink(0);
    panel->addView(intro);

    auto *scroll = new ScrollView();
    scroll->setName("fileActionsScroll");
    scroll->setFlex(1)->setMinHeight(0);
    // Put the scrollbar in the panel's padding so cards align with the footer.
    scroll->setMargin(Edge::Right, -12);
    scroll->setContentPadding(Edge::Right, 12);
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Column);
    content->setAlignItems(YGAlignStretch);
    content->setGap(14);
    scroll->setContentView(content);
    panel->addView(scroll);

    TextView *closeText = nullptr;
    auto *close = makeModalButton(i18n::message(footerKey), 20, &closeText);
    close->setWidth(140)->setAlignSelf(YGAlignFlexEnd)->setFlexShrink(0);
    close->setOnClickListener(std::move(onClose));
    styleThemedActionButton(close, closeText, true, ui_theme::control,
                            ui_theme::controlHover, ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
    if (showComputerHelp) {
      auto *footer = new View();
      footer->setFlexDirection(FlexDirection::Row)
          ->setAlignItems(YGAlignCenter)->setGap(16)->setHeight(58)
          ->setFlexShrink(0);
      TextView *helpText = nullptr;
      auto *help = makeModalButton(
          i18n::message("menu.manage_files.computer.label"), 18, &helpText);
      help->setName("fileActionsComputerHelp");
      help->setWidth(0)->setFlex(1)->setMinWidth(0);
      help->setStyledBorderWidth(0);
      help->setThemedBackgroundColors(
          [] { return Color(0, 0, 0, 0); },
          [] { return ui_theme::withAlpha(ui_theme::cyan(), 14); },
          [] { return ui_theme::withAlpha(ui_theme::cyan(), 26); });
      helpText->setAlign(TextView::LEFT);
      helpText->setWrap(true);
      helpText->setThemedColor(ui_theme::cyan);
      help->setOnClickListener([this] {
        fileActionsPanel_->setVisible(false);
        fileActionsPanel_->setDisplay(YGDisplayNone);
        computerImportPanel_->setVisible(true);
        computerImportPanel_->setDisplay(YGDisplayFlex);
        fileActionsModalRoot_->applyYogaLayout();
      });
      footer->addView(help);
      footer->addView(close);
      panel->addView(footer);
    } else {
      panel->addView(close);
    }
    fileActionsModalRoot_->addView(panel);
    *panelOut = panel;
    return content;
  };
  auto addAction = [](View *content, uint32_t iconCodepoint,
                      const char *labelKey, const char *descriptionKey,
                      std::function<void()> onClick) {
    auto *card = new Button();
    card->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignCenter)
        ->setJustifyContent(YGJustifyCenter)
        ->setFlexShrink(0)
        ->setGap(18)
        ->setPadding(Edge::All, 20)
        ->setCornerRadius(ui_theme::controlRadius());
    card->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
    card->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorderStrong,
                                ui_theme::accentBorderStrong);
    card->setStyledBorderWidth(1);
    card->setOnClickListener(std::move(onClick));

    auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 80);
    icon->setName("fileActionIcon");
    icon->setText(ui_icons::textForCodepoint(iconCodepoint));
    icon->setThemedColor([] { return ui_theme::activePalette().cyan; });
    icon->setAlign(TextView::CENTER);
    icon->setVAlign(TextView::MIDDLE);
    icon->setSize(88, 88);
    icon->setFlexShrink(0);
    card->addView(icon);

    auto *copy = new View();
    copy->setName("fileActionCopy");
    copy->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch)->setGap(8);
    auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    label->setName("fileActionLabel");
    label->setLocalizedText(i18n::message(labelKey));
    label->setThemedColor(ui_theme::textPrimary);
    label->setWrap(true);
    label->setFlexShrink(0);
    copy->addView(label);
    auto *description = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    description->setName("fileActionDescription");
    description->setLocalizedText(i18n::message(descriptionKey));
    description->setThemedColor(ui_theme::textSecondary);
    description->setWrap(true);
    description->setFlexShrink(0);
    copy->addView(description);
    card->addView(copy);
    content->addView(card);
    return card;
  };
  auto addRow = [](View *content) {
    auto *row = new View();
    row->setName("fileActionCards");
    row->setWidthPercent(100)->setGap(14)->setFlexShrink(0);
    content->addView(row);
    return row;
  };

  auto *actions = makePanel(
      "menu.manage_files.label", "menu.manage_files.intro",
      "menu.manage_files.close.label",
      [this]() { fileActionsModalRoot_->setVisible(false); }, &fileActionsPanel_, true);
  auto *actionCards = addRow(actions);
  addAction(actionCards, ui_icons::kDownload,
            "menu.import_folder.label", "menu.manage_files.folder",
            [this]() {
              fileActionsPanel_->setVisible(false);
              fileActionsPanel_->setDisplay(YGDisplayNone);
              folderImportPanel_->setVisible(true);
              folderImportPanel_->setDisplay(YGDisplayFlex);
              fileActionsModalRoot_->applyYogaLayout();
            });
  addAction(actionCards, 0xf1c6 /* file-zipper */,
            "menu.import_archive.label", "menu.manage_files.archive",
            [this]() {
              fileActionsModalRoot_->setVisible(false);
              if (context.chartLibraryFolderActions) {
                showTasksModal();
                context.chartLibraryFolderActions->requestImportArchive();
              }
            });
  addAction(actionCards, ui_icons::kReveal,
            "menu.open_files.label", "menu.manage_files.open",
            [this]() {
              fileActionsModalRoot_->setVisible(false);
              std::string error;
              if (!OpenAndroidDocumentsFolder(error)) {
                SDL_Log("Open Documents: %s", error.c_str());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AsoBMaShow",
                                         i18n::tr("menu.open_files.failed"), nullptr);
              }
            });
  if (AndroidBuildHasManageExternalStorage()) {
    auto *link = addAction(actions, 0xf0c1 /* link */,
                          "menu.manage_files.link.label", "menu.manage_files.link",
                          [this]() {
                            fileActionsModalRoot_->setVisible(false);
                            if (context.requestAddChartFolderFromFiles) {
                              context.requestAddChartFolderFromFiles();
                            }
                          });
    link->setName("fileActionLink");
  }

  auto *folderActions = makePanel(
      "menu.import_folder.label", "menu.manage_files.folder_choice",
      "library.tasks.back.label", [this]() { showFileActionsModal(); },
      &folderImportPanel_);
  auto importFolder = [this](bool moveSource) {
    fileActionsModalRoot_->setVisible(false);
    if (context.chartLibraryFolderActions) {
      showTasksModal();
      context.chartLibraryFolderActions->requestImportFolder(moveSource);
    }
  };
  auto *folderCards = addRow(folderActions);
  addAction(folderCards, 0xf0c5 /* copy */,
            "menu.manage_files.copy.label", "menu.manage_files.copy",
            [importFolder]() { importFolder(false); });
  addAction(folderCards, 0xf362 /* right-left */,
            "menu.manage_files.move.label", "menu.manage_files.move",
            [importFolder]() { importFolder(true); });
  folderImportPanel_->setVisible(false);
  folderImportPanel_->setDisplay(YGDisplayNone);

  auto *computerActions = makePanel(
      "menu.manage_files.computer.label", "menu.manage_files.computer.choice",
      "library.tasks.back.label", [this]() { showFileActionsModal(); },
      &computerImportPanel_);
  auto showComputerGuide = [this](const char *titleKey, const char *instructionsKey) {
    const auto path = ChartRepository::DefaultBmsFolderPath().string();
    const auto instructions = i18n::format(instructionsKey, {{"path", path}});
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
         i18n::tr("library.tasks.back.label")},
        {0, 1, i18n::tr("menu.refresh_library.label")},
    };
    SDL_MessageBoxData messageBox{};
    messageBox.flags = SDL_MESSAGEBOX_INFORMATION;
    messageBox.title = i18n::tr(titleKey);
    messageBox.message = instructions.c_str();
    messageBox.numbuttons = 2;
    messageBox.buttons = buttons;
    int selectedButton = -1;
    if (SDL_ShowMessageBox(&messageBox, &selectedButton) == 0 && selectedButton == 1) {
      fileActionsModalRoot_->setVisible(false);
      startLibraryRefresh();
      showTasksModal();
    }
  };
  auto *computerCards = addRow(computerActions);
  addAction(computerCards, 0xf1c6 /* file-zipper */,
            "menu.manage_files.computer.regular.label",
            "menu.manage_files.computer.regular.description",
            [showComputerGuide] {
              showComputerGuide("menu.manage_files.computer.regular.label",
                                "menu.manage_files.computer.regular.instructions");
            });
  addAction(computerCards, 0xf108 /* desktop */,
            "menu.manage_files.computer.gui.label",
            "menu.manage_files.computer.gui.description",
            [showComputerGuide] {
              showComputerGuide("menu.manage_files.computer.gui.label",
                                "menu.manage_files.computer.gui.instructions");
            });
  addAction(computerCards, 0xf120 /* terminal */,
            "menu.manage_files.computer.cli.label",
            "menu.manage_files.computer.cli.description",
            [showComputerGuide] {
              showComputerGuide("menu.manage_files.computer.cli.label",
                                "menu.manage_files.computer.cli.instructions");
            });
  computerImportPanel_->setVisible(false);
  computerImportPanel_->setDisplay(YGDisplayNone);
  rootLayout->addView(fileActionsModalRoot_);
  resizeFileActionsModal();
}

void MainMenuScene::resizeFileActionsModal() {
  if (fileActionsModalRoot_ == nullptr) return;
  const auto safe = getSafeAreaInsetsUi();
  fileActionsModalRoot_->setSize(rendering::window_width, rendering::window_height);
  fileActionsModalRoot_->setPadding(Edge::Top, safe.top);
  fileActionsModalRoot_->setPadding(Edge::Bottom, safe.bottom);
  fileActionsModalRoot_->setPadding(Edge::Left, safe.left);
  fileActionsModalRoot_->setPadding(Edge::Right, safe.right);
  const float width = std::min(1040.0f, std::max(
      0.0f, rendering::window_width - safe.left - safe.right - 32.0f));
  const bool columns = rendering::window_width > rendering::window_height && width >= 900.0f;
  const float availableHeight = std::max(
      0.0f, rendering::window_height - safe.top - safe.bottom - 32.0f);
  auto layoutCard = [](View *card, float cardWidth, bool vertical) {
    card->setWidth(cardWidth)->setHeight(vertical ? 280.0f : 164.0f);
    card->setFlexDirection(vertical ? FlexDirection::Column : FlexDirection::Row);
    auto *copy = card->findViewByName("fileActionCopy");
    copy->setWidth(std::max(0.0f, vertical ? cardWidth - 40.0f : cardWidth - 146.0f));
    for (const char *name : {"fileActionLabel", "fileActionDescription"}) {
      auto *text = static_cast<TextView *>(card->findViewByName(name));
      text->setAlign(vertical ? TextView::CENTER : TextView::LEFT);
    }
  };
  for (auto *panel : {fileActionsPanel_, folderImportPanel_, computerImportPanel_}) {
    auto *scroll = static_cast<ScrollView *>(panel->findViewByName("fileActionsScroll"));
    auto *content = scroll->getContentView();
    auto *row = content->findViewByName("fileActionCards");
    const auto count = row->getChildren().size();
    const float contentWidth = std::max(0.0f, width - 42.0f);
    const float cardsHeight = columns ? 280.0f : count * 164.0f + (count - 1) * 14.0f;
    auto *link = content->findViewByName("fileActionLink");
    const float height = cardsHeight + 224.0f + (link ? 178.0f : 0.0f);
    panel->setWidth(width)->setHeight(std::min(height, availableHeight));
    row->setFlexDirection(columns ? FlexDirection::Row : FlexDirection::Column);
    const float cardWidth = columns ? (contentWidth - (count - 1) * 14.0f) / count
                                    : contentWidth;
    for (auto *card : row->getChildren()) layoutCard(card, cardWidth, columns);
    if (link) layoutCard(link, contentWidth, false);
    scroll->refreshContentLayout();
  }
}

void MainMenuScene::resizeTasksModal() {
  if (tasksModalRoot == nullptr || tasksScrollView == nullptr) return;
  auto *panel = tasksModalRoot->findViewByName("mainMenuTasksPanel");
  if (panel == nullptr) return;
  const float width = std::min(760.0f, std::max(
      0.0f, static_cast<float>(rendering::window_width) - 32.0f));
  const float height = std::min(460.0f, std::max(
      0.0f, static_cast<float>(rendering::window_height) - 32.0f));
  panel->setWidth(width)->setHeight(height);
  tasksScrollView->setWidth(std::max(0.0f, width - 44.0f));
}

void MainMenuScene::showFileActionsModal() {
  if (fileActionsModalRoot_ == nullptr) return;
  resizeFileActionsModal();
  fileActionsPanel_->setVisible(true);
  fileActionsPanel_->setDisplay(YGDisplayFlex);
  folderImportPanel_->setVisible(false);
  folderImportPanel_->setDisplay(YGDisplayNone);
  computerImportPanel_->setVisible(false);
  computerImportPanel_->setDisplay(YGDisplayNone);
  fileActionsModalRoot_->setVisible(true);
  fileActionsModalRoot_->applyYogaLayout();
}
#endif

void MainMenuScene::buildParseLogModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 900.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  parseLogModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                              rendering::window_height);
  parseLogModalRoot->setPositionType(YGPositionTypeAbsolute);
  parseLogModalRoot->setPosition(Edge::Left, 0);
  parseLogModalRoot->setPosition(Edge::Top, 0);
  parseLogModalRoot->setZIndex(1000);
  parseLogModalRoot->setVisible(false);
  parseLogModalRoot->setFlexDirection(FlexDirection::Column);
  parseLogModalRoot->setAlignItems(YGAlignCenter);
  parseLogModalRoot->setJustifyContent(YGJustifyCenter);
  parseLogModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(640)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setLocalizedText(i18n::message("menu.parsing_logs.label"));
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  parseLogRecyclerView = new RecyclerView<MainMenuParseLogRow>(
      [](const MainMenuParseLogRow &a, const MainMenuParseLogRow &b) {
        return a.id == b.id;
      });
  parseLogRecyclerView->itemHeight = kParseLogRowHeight;
  parseLogRecyclerView->reserveScrollbarGutter = true;
  parseLogRecyclerView->setWidth(kModalContentWidth);
  parseLogRecyclerView->setFlex(1);
  parseLogRecyclerView->setThemedBackgroundColor(ui_theme::insetSurface);
  parseLogRecyclerView->setCornerRadius(ui_theme::controlRadius());
  parseLogRecyclerView->setThemedBorderColor(ui_theme::hairline);
  parseLogRecyclerView->setBorderWidth(1);
  parseLogRecyclerView->onCreateView = [](const MainMenuParseLogRow &) {
    return new ParseLogRowView();
  };
  parseLogRecyclerView->onBind = [](View *view,
                                    const MainMenuParseLogRow &row, int,
                                    bool) {
    auto *rowView = dynamic_cast<ParseLogRowView *>(view);
    if (rowView != nullptr) {
      rowView->setRow(row);
    }
  };
  panel->addView(parseLogRecyclerView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  parseLogExportStatusText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  parseLogExportStatusText->setFlex(1);
  parseLogExportStatusText->setVAlign(TextView::MIDDLE);
  parseLogExportStatusText->setOverflow(TextView::TextOverflow::Hidden);
  parseLogExportStatusText->setThemedColor(ui_theme::textSecondary);
  footer->addView(parseLogExportStatusText);

  parseLogExportButton =
      makeModalButton(i18n::message("menu.export_log.label"), 20, &parseLogExportButtonText);
  parseLogExportButton->setWidth(160);
  parseLogExportButton->setOnClickListener(
      [this]() { startParseLogExport(); });
  footer->addView(parseLogExportButton);

  parseLogCloseButton = makeModalButton(i18n::message("menu.parse_log.close.label"), 20, &parseLogCloseButtonText);
  parseLogCloseButton->setWidth(130);
  parseLogCloseButton->setOnClickListener([this]() { hideParseLogModal(); });
  styleThemedActionButton(parseLogCloseButton, parseLogCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);

  footer->addView(parseLogCloseButton);
  panel->addView(footer);

  parseLogModalRoot->addView(panel);
  rootLayout->addView(parseLogModalRoot);
  parseLogDisplayedRevision = 0;
  refreshParseLogExportControls();
  refreshParseLogModal(true);
}

void MainMenuScene::showParseLogModal() {
  if (parseLogModalRoot == nullptr) {
    return;
  }
  parseLogModalRoot->setSize(rendering::window_width, rendering::window_height);
  parseLogModalRoot->setVisible(true);
  parseLogDisplayedRevision = 0;
  refreshParseLogExportControls();
  refreshParseLogModal(true);
}

void MainMenuScene::hideParseLogModal() {
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setVisible(false);
  }
}

void MainMenuScene::startParseLogExport() {
  if (parseLogDocumentHandoff) {
    return;
  }
  if (parseLogExportStatusText != nullptr) {
    parseLogExportStatusText->setLocalizedText(i18n::message("menu.preparing_performance_log.progress"));
  }
  std::string logText = archive_file::debugLogText();
  const std::uint64_t exportLimit = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(logText.size()));
  parseLogDocumentHandoff =
      platform_document_handoff::ExportTextDocumentAsync({
          .text = std::move(logText),
          .suggestedName = "AsoBMaShow-performance-log.txt",
          .maxBytes = exportLimit,
      });
  refreshParseLogExportControls();
}

void MainMenuScene::applyParseLogDocumentHandoff() {
  if (!parseLogDocumentHandoff || !parseLogDocumentHandoff.ready()) {
    return;
  }
  auto result = parseLogDocumentHandoff.takeResult();
  parseLogDocumentHandoff.close();
  if (parseLogExportStatusText != nullptr && result) {
    if (result->ok()) {
      parseLogExportStatusText->setLocalizedText(i18n::message("menu.performance_log_exported.message"));
    } else if (result->cancelled()) {
      parseLogExportStatusText->setLocalizedText(i18n::message("menu.log_export_cancelled.message"));
    } else {
      const std::string message =
          result->message.empty() ? i18n::tr("menu.log_export_failed.message") : result->message;
      parseLogExportStatusText->setText(message);
      SDL_Log("Performance log export failed: %s", message.c_str());
    }
  }
  refreshParseLogExportControls();
}

void MainMenuScene::refreshParseLogExportControls() {
  if (parseLogExportButton == nullptr || parseLogExportButtonText == nullptr) {
    return;
  }
  const bool enabled = !static_cast<bool>(parseLogDocumentHandoff);
  parseLogExportButtonText->setLocalizedText(enabled ? i18n::message("menu.export_log.label") : i18n::message("menu.exporting.progress"));
  styleThemedActionButton(
      parseLogExportButton, parseLogExportButtonText, enabled,
      ui_theme::primaryAction, ui_theme::primaryActionHover,
      ui_theme::primaryActionPressed, ui_theme::accentBorderStrong);
}

void MainMenuScene::refreshParseLogModal(bool forceScrollToBottom) {
  if (parseLogModalRoot == nullptr || parseLogRecyclerView == nullptr) {
    return;
  }

  const std::uint64_t revision = archive_file::debugLogRevision();
  if (revision == parseLogDisplayedRevision) {
    if (forceScrollToBottom) {
      scrollParseLogModalToBottom();
    }
    return;
  }
  const bool shouldScrollToBottom =
      forceScrollToBottom || parseLogDisplayedRevision == 0 ||
      isParseLogScrolledNearBottom();
  parseLogDisplayedRevision = revision;
  parseLogRecyclerView->setItems(
      parseLogRowsFromLines(archive_file::debugLogLines()));
  if (shouldScrollToBottom) {
    scrollParseLogModalToBottom();
  }
}

bool MainMenuScene::isParseLogScrolledNearBottom() const {
  if (parseLogRecyclerView == nullptr) {
    return true;
  }
  const float maxOffset = std::max(
      0.0f, static_cast<float>(parseLogRecyclerView->size() *
                                   parseLogRecyclerView->itemHeight -
                               parseLogRecyclerView->getHeight()));
  return maxOffset - parseLogRecyclerView->scrollOffset <=
         static_cast<float>(parseLogRecyclerView->itemHeight * 2);
}

void MainMenuScene::scrollParseLogModalToBottom() {
  if (parseLogRecyclerView == nullptr) {
    return;
  }
  const float maxOffset = std::max(
      0.0f, static_cast<float>(parseLogRecyclerView->size() *
                                   parseLogRecyclerView->itemHeight -
                               parseLogRecyclerView->getHeight()));
  parseLogRecyclerView->scrollOffset = maxOffset;
  parseLogRecyclerView->rebindVisibleItems();
}

void MainMenuScene::buildMusicModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;

  musicModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  musicModalRoot->setPositionType(YGPositionTypeAbsolute);
  musicModalRoot->setPosition(Edge::Left, 0);
  musicModalRoot->setPosition(Edge::Top, 0);
  musicModalRoot->setZIndex(1000);
  musicModalRoot->setVisible(false);
  musicModalRoot->setFlexDirection(FlexDirection::Column);
  musicModalRoot->setAlignItems(YGAlignCenter);
  musicModalRoot->setJustifyContent(YGJustifyCenter);
  musicModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(650)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setLocalizedText(i18n::message("menu.music_player.label"));
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  musicTrackText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  musicTrackText->setHeight(62);
  musicTrackText->setWrap(true);
  musicTrackText->setOverflow(TextView::TextOverflow::Hidden);
  musicTrackText->setThemedColor(ui_theme::textPrimary);
  panel->addView(musicTrackText);

  musicStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  musicStatusText->setHeight(70);
  musicStatusText->setWrap(true);
  musicStatusText->setThemedColor(ui_theme::textSecondary);
  panel->addView(musicStatusText);

  musicPlaylistText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  musicPlaylistText->setHeight(100);
  musicPlaylistText->setWrap(true);
  musicPlaylistText->setOverflow(TextView::TextOverflow::Hidden);
  musicPlaylistText->setThemedColor(ui_theme::textSecondary);
  panel->addView(musicPlaylistText);

  auto *sourceRow = makeModalOptionRow();
  musicSelectedButton =
      makeModalButton(i18n::message("menu.play_selected.label"), 18, &musicSelectedButtonText);
  musicSelectedButton->setFlex(1);
  musicSelectedButton->setOnClickListener(
      [this]() { playSelectedChartAsMusic(); });
  musicRandomButton = makeModalButton(i18n::message("menu.random_all.label"), 20, &musicRandomButtonText);
  musicRandomButton->setFlex(1);
  musicRandomButton->setOnClickListener([this]() { playRandomMusicLibrary(); });
  sourceRow->addView(musicSelectedButton);
  sourceRow->addView(musicRandomButton);
  panel->addView(sourceRow);

  auto *playlistRow = makeModalOptionRow();
  musicAddSelectedButton =
      makeModalButton(i18n::message("menu.add_selected.label"), 18, &musicAddSelectedButtonText);
  musicAddSelectedButton->setFlex(1);
  musicAddSelectedButton->setOnClickListener(
      [this]() { addSelectedChartToMusicPlaylist(); });
  musicRemoveSelectedButton =
      makeModalButton(i18n::message("menu.remove_selected.label"), 16, &musicRemoveSelectedButtonText);
  musicRemoveSelectedButton->setFlex(1);
  musicRemoveSelectedButton->setOnClickListener(
      [this]() { removeSelectedChartFromMusicPlaylist(); });
  musicPlaylistButton =
      makeModalButton(i18n::message("menu.play_playlist.label"), 18, &musicPlaylistButtonText);
  musicPlaylistButton->setFlex(1);
  musicPlaylistButton->setOnClickListener(
      [this]() { playSavedMusicPlaylist(); });
  musicClearPlaylistButton =
      makeModalButton(i18n::message("menu.clear.label"), 18, &musicClearPlaylistButtonText);
  musicClearPlaylistButton->setFlex(1);
  musicClearPlaylistButton->setOnClickListener(
      [this]() { clearSavedMusicPlaylist(); });
  playlistRow->addView(musicAddSelectedButton);
  playlistRow->addView(musicRemoveSelectedButton);
  playlistRow->addView(musicPlaylistButton);
  playlistRow->addView(musicClearPlaylistButton);
  panel->addView(playlistRow);

  auto *transportRow = makeModalOptionRow();
  musicPreviousButton =
      makeModalButton(i18n::message("menu.previous.label"), 16, &musicPreviousButtonText);
  musicPreviousButton->setFlex(1);
  musicPreviousButton->setOnClickListener(
      [this]() { playPreviousMusicTrack(); });
  musicSeekBackwardButton =
      makeModalButton("-10s", 18, &musicSeekBackwardButtonText);
  musicSeekBackwardButton->setFlex(1);
  musicSeekBackwardButton->setOnClickListener(
      [this]() { seekMusicRelative(-10000000LL); });
  musicPlayPauseButton = makeModalButton(i18n::message("menu.play.label"), 20, &musicPlayPauseButtonText);
  musicPlayPauseButton->setFlex(1);
  musicPlayPauseButton->setOnClickListener([this]() { toggleMusicPlayback(); });
  musicSeekForwardButton =
      makeModalButton("+10s", 18, &musicSeekForwardButtonText);
  musicSeekForwardButton->setFlex(1);
  musicSeekForwardButton->setOnClickListener(
      [this]() { seekMusicRelative(10000000LL); });
  musicNextButton = makeModalButton(i18n::message("menu.next.label"), 20, &musicNextButtonText);
  musicNextButton->setFlex(1);
  musicNextButton->setOnClickListener([this]() { playNextMusicTrack(); });
  musicStopButton = makeModalButton(i18n::message("menu.stop.label"), 18, &musicStopButtonText);
  musicStopButton->setFlex(1);
  musicStopButton->setOnClickListener([this]() { stopMusicPlayback(); });
  transportRow->addView(musicPreviousButton);
  transportRow->addView(musicSeekBackwardButton);
  transportRow->addView(musicPlayPauseButton);
  transportRow->addView(musicSeekForwardButton);
  transportRow->addView(musicNextButton);
  transportRow->addView(musicStopButton);
  panel->addView(transportRow);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  musicCloseButton = makeModalButton(i18n::message("menu.music_player.close.label"), 20, &musicCloseButtonText);
  musicCloseButton->setWidth(130);
  musicCloseButton->setOnClickListener([this]() { hideMusicModal(); });
  footer->addView(musicCloseButton);
  panel->addView(footer);

  musicModalRoot->addView(panel);
  rootLayout->addView(musicModalRoot);
  refreshMusicModal();
}

void MainMenuScene::showMusicModal() {
  if (musicModalRoot == nullptr) {
    return;
  }
  musicModalRoot->setSize(rendering::window_width, rendering::window_height);
  musicModalRoot->setVisible(true);
  std::string errorMessage;
  if (!context.musicPlayer.ReloadPlaylists(errorMessage) &&
      !errorMessage.empty()) {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::hideMusicModal() {
  if (musicModalRoot != nullptr) {
    musicModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshMusicModal() {
  if (musicModalRoot == nullptr || musicTrackText == nullptr ||
      musicStatusText == nullptr || musicPlaylistText == nullptr) {
    return;
  }

  const auto track = context.musicPlayer.CurrentTrackSnapshot();
  const auto playback = context.musicPlayer.PlaybackState();

  musicTrackText->setText(
      musicTrackDisplayName(track ? &track.value() : nullptr));

  std::string status;
  if (!musicStatusMessage.empty()) {
    status += musicStatusMessage.resolve() + "\n";
  }
  if (!playback.supported) {
    status += i18n::tr("menu.native_music_playback_unavailable_on_platform.message");
  } else if (!playback.loaded) {
    status += i18n::tr("menu.choose_play_selected_play_playlist_random_all.message");
  } else {
    status += playback.playing ? i18n::tr("menu.playing.prefix") : i18n::tr("menu.paused.prefix");
    status += formatMusicTime(playback.positionMicros) + " / " +
              formatMusicTime(playback.durationMicros);
  }
  if (const auto playlist = context.musicPlayer.DefaultPlaylistSnapshot()) {
    status += "  " + playlist->name + ": " +
              std::to_string(playlist->trackCount);
  }
  const std::size_t libraryTrackCount = context.musicPlayer.LibraryTrackCount();
  if (libraryTrackCount > 0) {
    status += "  " + i18n::format("menu.music_player.library_tracks.count",
        {{"count", std::to_string(libraryTrackCount)}});
  }
  musicStatusText->setText(status);
  musicPlaylistText->setText(
      musicPlaylistTextSnapshot(
          context.musicPlayer.DefaultPlaylistTracksSnapshot()));

  if (musicPlayPauseButtonText != nullptr) {
    musicPlayPauseButtonText->setLocalizedText(
        playback.playing ? i18n::message("menu.pause.label") : (playback.loaded ? i18n::message("menu.resume.label") : i18n::message("menu.play.label")));
  }

  styleThemedActionButton(musicSelectedButton, musicSelectedButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  styleThemedActionButton(musicAddSelectedButton, musicAddSelectedButtonText,
                          true, ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicRemoveSelectedButton,
                          musicRemoveSelectedButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicPlaylistButton, musicPlaylistButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  styleThemedActionButton(musicClearPlaylistButton,
                          musicClearPlaylistButtonText, true,
                          ui_theme::warningAction,
                          ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicRandomButton, musicRandomButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicPreviousButton, musicPreviousButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicSeekBackwardButton,
                          musicSeekBackwardButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicPlayPauseButton, musicPlayPauseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(musicSeekForwardButton, musicSeekForwardButtonText,
                          true, ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicNextButton, musicNextButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicStopButton, musicStopButtonText, true,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicCloseButton, musicCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
}

void MainMenuScene::playSelectedChartAsMusic() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = i18n::message("menu.select_chart_first.message");
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = i18n::message("menu.selected_chart_unable_played_as_music.message");
    refreshMusicModal();
    return;
  }

  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();

  MusicTrackRecord musicRecord{.representativeChart = record.meta,
                               .chartCount = 1};
  context.musicPlayer.SetNowPlaying({music_playlist::MakeTrack(musicRecord)});

  context.musicPlayer.PlayCurrentAsync(
      musicStatusMessage, i18n::message("menu.playing_selected_chart.message"));
  refreshMusicModal();
}

void MainMenuScene::addSelectedChartToMusicPlaylist() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = i18n::message("menu.music_player.select_chart_first.message");
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = i18n::message("menu.music_player.selected_chart_unable_added_playlist.message");
    refreshMusicModal();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.AddChartToDefaultPlaylist(record.meta,
                                                    errorMessage)) {
    musicStatusMessage = i18n::message("menu.music_player.added_selected_chart_my_playlist.message");
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::removeSelectedChartFromMusicPlaylist() {
  if (willStart.load() || replayExportJob_.inProgress() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = i18n::message("menu.music_player.select_chart_first.message");
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = i18n::message("menu.music_player.selected_chart_unable_removed_from_playlist.message");
    refreshMusicModal();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.RemoveChartFromDefaultPlaylist(record.meta,
                                                         errorMessage)) {
    musicStatusMessage = i18n::message("menu.music_player.removed_selected_chart_from_my_playlist.message");
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playSavedMusicPlaylist() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();

  std::string errorMessage;
  if (!context.musicPlayer.StartDefaultPlaylist(errorMessage)) {
    musicStatusMessage = errorMessage;
  } else {
    context.musicPlayer.PlayCurrentAsync(
        musicStatusMessage,
        i18n::message("menu.music_player.playing_my_playlist.message"));
  }
  refreshMusicModal();
}

void MainMenuScene::clearSavedMusicPlaylist() {
  std::string errorMessage;
  if (context.musicPlayer.ClearDefaultPlaylist(errorMessage)) {
    musicStatusMessage = i18n::message("menu.music_player.cleared_my_playlist.message");
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playRandomMusicLibrary() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();

  std::string errorMessage;
  if (!context.musicPlayer.ReloadLibrary(errorMessage) ||
      !context.musicPlayer.StartRandomLibrary(errorMessage)) {
    musicStatusMessage = errorMessage;
  } else {
    context.musicPlayer.PlayCurrentAsync(
        musicStatusMessage, i18n::message("menu.playing_now_playing.message"));
  }
  refreshMusicModal();
}

void MainMenuScene::toggleMusicPlayback() {
  std::string errorMessage;
  const auto playback = context.musicPlayer.PlaybackState();
  if (playback.playing) {
    context.musicPlayer.Pause(errorMessage);
    musicStatusMessage = errorMessage;
  } else if (playback.loaded) {
    context.musicPlayer.Resume(errorMessage);
    musicStatusMessage = errorMessage;
  } else {
    context.musicPlayer.PlayCurrentAsync(
        musicStatusMessage, i18n::message("menu.playing_current_track.message"));
  }
  refreshMusicModal();
}

void MainMenuScene::seekMusicRelative(long long deltaMicros) {
  const auto playback = context.musicPlayer.PlaybackState();
  if (!playback.supported || !playback.loaded) {
    musicStatusMessage = i18n::message("menu.playback.no_track_error");
    refreshMusicModal();
    return;
  }

  long long targetMicros = std::max(0LL, playback.positionMicros + deltaMicros);
  if (playback.durationMicros > 0) {
    targetMicros = std::min(targetMicros, playback.durationMicros);
  }

  std::string errorMessage;
  if (context.musicPlayer.Seek(targetMicros, errorMessage)) {
    musicStatusMessage = i18n::message(
        "menu.music_player.seeked_to.message",
        {{"time", formatMusicTime(targetMicros)}});
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playNextMusicTrack() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();

  context.musicPlayer.PlayNextAsync(
      musicStatusMessage, i18n::message("menu.playing_next_track.message"));
  refreshMusicModal();
}

void MainMenuScene::playPreviousMusicTrack() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();

  context.musicPlayer.PlayPreviousAsync(
      musicStatusMessage, i18n::message("menu.playing_previous_track.message"));
  refreshMusicModal();
}

void MainMenuScene::stopMusicPlayback() {
  std::string errorMessage;
  if (context.musicPlayer.Stop(errorMessage)) {
    musicStatusMessage = i18n::message("menu.stopped.message");
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::buildTasksModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  tasksModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  tasksModalRoot->setPositionType(YGPositionTypeAbsolute);
  tasksModalRoot->setPosition(Edge::Left, 0);
  tasksModalRoot->setPosition(Edge::Top, 0);
  tasksModalRoot->setZIndex(1000);
  tasksModalRoot->setVisible(false);
  tasksModalRoot->setFlexDirection(FlexDirection::Column);
  tasksModalRoot->setAlignItems(YGAlignCenter);
  tasksModalRoot->setJustifyContent(YGJustifyCenter);
  tasksModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
#if TARGET_OS_ANDROID
  panel->setName("mainMenuTasksPanel");
#endif
  panel->setWidth(kModalPanelWidth)
      ->setHeight(460)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setLocalizedText(i18n::message("menu.tasks.label"));
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  tasksScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 400);
  tasksScrollView->setWidth(kModalContentWidth);
  tasksScrollView->setFlex(1);
#if TARGET_OS_ANDROID
  tasksScrollView->setMinHeight(0);
#endif
  tasksScrollView->setThemedBackgroundColor(ui_theme::insetSurface);
  tasksScrollView->setCornerRadius(ui_theme::controlRadius());
  tasksScrollView->setThemedBorderColor(ui_theme::hairline);
  tasksScrollView->setBorderWidth(1);
  tasksScrollView->setContentPadding(Edge::All, 12);

  tasksContent = new View();
  tasksContent->setFlexDirection(FlexDirection::Column);
  tasksContent->setAlignItems(YGAlignStretch);

  tasksText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  tasksText->setText(tasksModalTextSnapshot());
  tasksText->setThemedColor(ui_theme::textSecondary);
  tasksText->setWrap(true);
  tasksText->setOverflow(TextView::TextOverflow::Visible);
  tasksContent->addView(tasksText);
  tasksScrollView->setContentView(tasksContent);
  panel->addView(tasksScrollView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  tasksRefreshButton =
      makeModalButton(i18n::message("menu.refresh_list.label"), 18, &tasksRefreshButtonText);
  tasksRefreshButton->setWidth(150);
  tasksRefreshButton->setOnClickListener([this]() {
    requestLibraryScanFlush();
    applyPendingUiUpdates();
    hideTasksModal();
  });
  styleThemedActionButton(tasksRefreshButton, tasksRefreshButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);

  tasksCloseButton = makeModalButton(i18n::message("menu.tasks.close.label"), 20, &tasksCloseButtonText);
  tasksCloseButton->setWidth(130);
  tasksCloseButton->setOnClickListener([this]() { hideTasksModal(); });
  styleThemedActionButton(tasksCloseButton, tasksCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);

#if TARGET_OS_ANDROID
  footer->setFlexShrink(0);
  tasksRefreshButton->setWidth(0)->setFlex(1)->setMinWidth(0);
  tasksCloseButton->setWidth(0)->setFlex(1)->setMinWidth(0);
  tasksRefreshButtonText->setWrap(true);
  tasksCloseButtonText->setWrap(true);
#endif
  footer->addView(tasksRefreshButton);
  footer->addView(tasksCloseButton);
  panel->addView(footer);

  tasksModalRoot->addView(panel);
  rootLayout->addView(tasksModalRoot);
#if TARGET_OS_ANDROID
  resizeTasksModal();
#endif
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  refreshTasksModal();
}

void MainMenuScene::showTasksModal() {
  if (tasksModalRoot == nullptr) {
    return;
  }
  tasksModalRoot->setSize(rendering::window_width, rendering::window_height);
  tasksModalRoot->setVisible(true);
#if TARGET_OS_ANDROID
  resizeTasksModal();
  tasksModalRoot->applyYogaLayout();
#endif
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  refreshTasksModal();
}

void MainMenuScene::hideTasksModal() {
  if (tasksModalRoot != nullptr) {
    tasksModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshTasksModal(bool force) {
  if (tasksModalRoot == nullptr || tasksText == nullptr) {
    return;
  }

  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  if (!force && snapshot.revision == displayedLibraryTasksRevision &&
      snapshot.progress.revision == displayedLibraryProgressRevision) {
    return;
  }
  displayedLibraryTasksRevision = snapshot.revision;
  displayedLibraryProgressRevision = snapshot.progress.revision;
  tasksText->setText(tasksModalTextSnapshot());
}

std::string MainMenuScene::tasksModalTextSnapshot() {
  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  const LibraryTaskProgressSnapshot &progressSnapshot = snapshot.progress;
  std::vector<LibraryTaskInfo> activeTasks;
  std::vector<LibraryTaskInfo> recentTasks;
  activeTasks.reserve(snapshot.tasks.size());
  recentTasks.reserve(snapshot.tasks.size());
  for (const auto &task : snapshot.tasks) {
    if (task.status == LibraryTaskStatus::Queued ||
        task.status == LibraryTaskStatus::Running ||
        task.status == LibraryTaskStatus::Paused) {
      activeTasks.push_back(task);
    } else {
      recentTasks.push_back(task);
    }
  }

  if (activeTasks.empty() && recentTasks.empty()) {
    return i18n::tr("menu.no_parsing_tasks.message");
  }

  std::ostringstream text;
  if (activeTasks.empty()) {
    text << i18n::tr("menu.no_active_tasks_recent_tasks.label");
  } else {
    text << i18n::format(activeTasks.size() == 1 ? "menu.tasks.active_count.one"
                                                : "menu.tasks.active_count.other",
                            {{"count", std::to_string(activeTasks.size())}})
         << "\n\n";
  }

  auto appendTask = [&text, &progressSnapshot](const LibraryTaskInfo &task) {
    std::string statusText;
    switch (task.status) {
    case LibraryTaskStatus::Queued:
      statusText = i18n::tr("menu.queued.label");
      break;
    case LibraryTaskStatus::Running:
      statusText = i18n::tr("menu.running.label");
      break;
    case LibraryTaskStatus::Complete:
      statusText = i18n::tr("menu.complete.label");
      break;
    case LibraryTaskStatus::Failed:
      statusText = i18n::tr("menu.failed.label");
      break;
    case LibraryTaskStatus::Paused:
      statusText = i18n::tr("menu.paused.label");
      break;
    }

    text << task.title.resolve() << "\n";
    text << statusText;
    if (task.status == LibraryTaskStatus::Running) {
      if (progressSnapshot.valid && progressSnapshot.taskId == task.id) {
        text << " - " << (progressSnapshot.basisPoints / 100) << "%";
        if (progressSnapshot.total > 0) {
          text << " (" << progressSnapshot.current << " / "
               << progressSnapshot.total << ")";
        }
        text << "\n" << chartScanProgressStageText(progressSnapshot.stage);
      } else {
        text << " - " << static_cast<int>(std::round(task.fraction * 100.0))
             << "%";
        if (task.total > 0) {
          text << " (" << task.current << " / " << task.total << ")";
        }
        if (!task.detail.empty()) {
          text << "\n" << task.detail.resolve();
        }
      }
    } else if (!task.detail.empty() && task.detail.resolve() != statusText) {
      text << "\n" << task.detail.resolve();
    }
    text << "\n\n";
  };

  for (const auto &task : activeTasks) {
    appendTask(task);
  }

  if (!activeTasks.empty() && !recentTasks.empty()) {
    text << i18n::tr("menu.recent_tasks.label");
  }

  constexpr std::size_t kMaxRecentTasksShown = 8;
  std::size_t shownRecentTasks = 0;
  for (auto it = recentTasks.rbegin();
       it != recentTasks.rend() && shownRecentTasks < kMaxRecentTasksShown;
       ++it, ++shownRecentTasks) {
    appendTask(*it);
  }

  return text.str();
}

void MainMenuScene::buildFindBmsModal() {
  findBmsModal_ = FindBmsModal::Create(
      rootLayout, {
      .downloadRoot = [this]() { return preferredBmsDownloadRoot(); },
      .downloadOptions = [this]() {
        return BmsSearchDownloadOptions{
            .skipUnarchivingForNonSolidArchives =
                context.settings.findBmsSkipUnarchivingForNonSolidArchives};
      },
      .downloadStarted = [this]() {
        findBmsSelectionGenerationAtDownloadStart = chartSelectionGeneration;
      },
      .filesReady = [this](const ChartMetaRecord &record,
                           const BmsSearchResult &result, bool matched) {
        enqueueDownloadedPathIndexTask(
            result.outputPath,
            matched ? main_menu_library::findBmsChartIdentity(record.meta)
                    : main_menu_library::FindBmsChartIdentity{},
            matched ? findBmsSelectionGenerationAtDownloadStart : 0,
            result.removedPaths);
      },
      .refreshLibrary = [this]() { startLibraryRefresh(); }});
}

void MainMenuScene::showFindBmsModal(const ChartMetaRecord &record) {
  if (findBmsModal_ != nullptr) findBmsModal_->show(record);
}

void MainMenuScene::hideFindBmsModal() {
  if (findBmsModal_ != nullptr) findBmsModal_->hide();
}

void MainMenuScene::refreshFindBmsModal(bool refreshCandidates) {
  if (findBmsModal_ != nullptr) findBmsModal_->refresh(refreshCandidates);
}

void MainMenuScene::applyFindBmsUpdates() {
  if (findBmsModal_ != nullptr) findBmsModal_->update();
}

void MainMenuScene::buildPlayOptionsModal() {
  if (rootLayout == nullptr) {
    return;
  }
  playOptionsModal = MainMenuPlayOptionsModal::Create(
      rootLayout,
      {.onRulesetSelected = [this](GameplayRuleset ruleset) {
         setGameplayRulesetSelection(ruleset);
       },
       .onGaugeSelected = [this](GaugeType type,
                                 GaugeAutoShiftMode autoShift) {
         setGaugeSelection(type, autoShift);
       },
       .onGaugeLowerBoundSelected = [this](GaugeType type) {
         setGaugeAutoShiftLowerBound(type);
       },
       .onPlayOptionSelected = [this](const std::string &option) {
         setPlayOptionSelection(option);
       },
       .isPlayOptionAllowed = [this](const std::string &option) {
         return currentPlayOptionSelectionAllowed(option);
       },
       .onLongNoteModeSelected = [this](const std::string &mode) {
         setLongNoteModeSelection(mode);
       },
       .onAssistOptionSelected = [this](const std::string &option) {
         setAssistOptionSelection(option);
       },
       .onPlaybackRateSelected = [this](int percent) {
         setPlaybackRateSelection(percent);
       },
       .onPlaybackModeSelected = [this](const std::string &mode) {
         setPlaybackModeSelection(mode);
       },
       .onClubModeToggled = [this]() { toggleGameplayClubMode(); },
       .onPacemakerSelected = [this](const std::string &target) {
         setPacemakerTargetSelection(target);
       }}, overlayPortal);
  if (!playOptionsModal) return;
  playOptionsModalRoot = playOptionsModal->root();
  playOptionsPanel = playOptionsModal->panel();
  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshPlaybackSelectionControls();
  refreshPacemakerTargetButtons();
}

void MainMenuScene::showPlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }

  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshPlaybackSelectionControls();
  refreshPacemakerTargetButtons();
  playOptionsModal->show();
}

void MainMenuScene::hidePlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }
  playOptionsModal->hide();
}

void MainMenuScene::openReplayRecordsForSelection() {
  if (recordsModal_ == nullptr || recyclerView == nullptr) {
    return;
  }
  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }
  const auto *selectedMeta = &recyclerView->get(selected);
  const bool courseStartReplay =
      selectedMeta->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (selectedMeta->solidArchive || selectedMeta->unavailable ||
      (!courseStartReplay && selectedMeta->meta.BmsPath.empty())) {
    return;
  }
  replayIrObservedRevisions.clear();
  recordsModal_->showChart(*selectedMeta);
  setReplayButtonVisible(true);
}

std::vector<ResultRecordSummary>
MainMenuScene::loadRecordsForModal(const ChartMetaRecord &record) {
  ResultRecordsLoadOptions options;
  if (record.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      (!activeFolder.courseKey.empty() || activeFolder.courseId > 0)) {
    options.course = CourseReplayLookup{.courseKey = activeFolder.courseKey,
                                        .legacyCourseId = activeFolder.courseId};
  } else {
    options.autoPlay = autoPlayReplaySummary(record);
  }
  options.irServerOrigin = activeReplayIrServerOrigin();
  const auto provider = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  options.irEnabled = provider != context.settings.irProviders.end() &&
                      provider->second.enabled;
  options.attemptActivity = [this](std::string_view attemptId) {
    const auto status = context.irSubmissionService != nullptr
                            ? context.irSubmissionService->status(
                                  ir::kTachiProviderId, attemptId)
                            : ir::IrAttemptStatusSnapshot{};
    return replay_records::recordActivity(status.activeRequest);
  };
  auto loaded = loadResultRecords(context.replayRepository, record, options);
  for (const auto &diagnostic : loaded.diagnostics) {
    if (diagnostic.message != publishedResultRecordDiagnostic) {
      publishedResultRecordDiagnostic = diagnostic.message;
      SDL_Log("%s", diagnostic.message.c_str());
      archive_file::appendDebugLogLine(diagnostic.message);
    }
  }
  if (loaded.complete) {
    publishedResultRecordDiagnostic.clear();
  }
  return std::move(loaded.records);
}

void MainMenuScene::shareReplayFile(const replay::ReplayFileActionRequest &request) {
  if (replayExportJob_.inProgress() || replayLoadTask_.active() ||
      replayResultRecallInProgress || replayIrUploadInProgress ||
      (recordFileActions_ && recordFileActions_->active())) return;
  if (!recordFileActions_) {
    recordFileActions_ = std::make_unique<RecordFileActions>(context.replayRepository);
  }
  const auto feedback = recordFileActions_->share(request);
  if (feedback.failed) SDL_Log("Replay share: %s", feedback.message.c_str());
  if (recordsModal_) {
    recordsModal_->setDocumentHandoffActive(recordFileActions_->active());
    if (feedback.reloadRecords) recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(feedback.message);
  }
}

void MainMenuScene::removeReplayFile(const replay::ReplayFileActionRequest &request) {
  if (replayExportJob_.inProgress() || replayLoadTask_.active() ||
      replayResultRecallInProgress || replayIrUploadInProgress ||
      (recordFileActions_ && recordFileActions_->active())) return;
  if (!recordFileActions_) {
    recordFileActions_ = std::make_unique<RecordFileActions>(context.replayRepository);
  }
  const auto feedback = recordFileActions_->remove(request);
  if (feedback.failed) SDL_Log("Replay delete: %s", feedback.message.c_str());
  if (recordsModal_) {
    if (feedback.reloadRecords) recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(feedback.message);
  }
}

void MainMenuScene::applyReplayFileDocumentHandoff() {
  if (!recordFileActions_) return;
  const auto feedback = recordFileActions_->poll();
  if (!feedback) return;
  if (feedback->failed) SDL_Log("Replay share: %s", feedback->message.c_str());
  if (recordsModal_) {
    recordsModal_->setDocumentHandoffActive(recordFileActions_->active());
    if (feedback->reloadRecords) recordsModal_->reloadRecords(true);
    recordsModal_->setStatus(feedback->message);
  }
}

ReplayRecordsModalCallbacks MainMenuScene::makeRecordsModalCallbacks() {
  ReplayRecordsModalCallbacks callbacks;
  callbacks.loadPreferences = [this] { return context.settings.replayPreferences; };
  callbacks.savePreferences = [this](const player_settings::ReplayPreferences &preferences) {
    context.settings.replayPreferences = preferences;
    if (!context.saveSettings()) SDL_Log("Failed to save replay preferences");
  };
  callbacks.loadRecords = [this](const ChartMetaRecord &record) {
    return loadRecordsForModal(record);
  };
  callbacks.watchModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernReplayPlayback(record, modern);
      };
  callbacks.watchModernCourse =
      [this](const ChartMetaRecord &record,
             const ModernCourseResultRecord &modern) {
        startModernCourseReplayPlayback(record, modern);
      };
  callbacks.watchAutoPlay = [this](const ChartMetaRecord &record) {
    startAutoPlayPlayback(record);
  };
  callbacks.gbattle =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernGBattlePlayback(record, modern);
      };
  callbacks.recallModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernReplayResultRecall(record, modern);
      };
  callbacks.recallModernCourse = [this](const ModernCourseResultRecord &modern,
                                        bool retrySame) {
    startModernCourseReplayResultRecall(modern, retrySame);
  };
  callbacks.recallRemote = [this](const IrRemoteRecordId &identity,
                                  const std::string &selectedStableKey) {
    startRemoteResultRecall(identity, selectedStableKey);
  };
  callbacks.exportModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern,
             ReplayVideoExportOptions options) {
        options.pacemakerTarget =
            pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
        startModernReplayVideoExport(record, modern, options);
      };
  callbacks.exportModernCourse =
      [this](const ModernCourseResultRecord &modern,
             ReplayVideoExportOptions options) {
        options.pacemakerTarget =
            pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
        startModernCourseReplayVideoExport(modern, options);
      };
  callbacks.exportAutoPlay =
      [this](const ChartMetaRecord &record, ReplayVideoExportOptions options) {
        if (options.pacemakerTarget.empty()) {
          options.pacemakerTarget =
              pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
        }
        startAutoPlayVideoExport(record, options);
      };
  callbacks.share = [this](const replay::ReplayFileActionRequest &request) {
    shareReplayFile(request);
  };
  callbacks.remove = [this](const replay::ReplayFileActionRequest &request) {
    removeReplayFile(request);
  };
  callbacks.irUpload = [this](const ModernChartResultRecord &modern) {
    startModernReplayIrUpload(modern);
  };
  callbacks.irStatusFeedback = [this](ir::IrRecordState state) {
    publishReplayIrStatusFeedback(state);
  };
  return callbacks;
}

bms_parser::ChartMeta
MainMenuScene::replayLoadMetaForRecord(const ChartMetaRecord &record) const {
  bms_parser::ChartMeta meta = record.meta;
  if (normalizeChartLongNoteModeValue(meta.LnMode) == 0) {
    meta.LnMode = long_note_mode::valueFromId(profileSelections.longNoteMode);
  }
  return meta;
}

ReplaySummary
MainMenuScene::autoPlayReplaySummary(const ChartMetaRecord &record) const {
  std::optional<std::string> playOption;
  if (!play_options::isNormalPlayOption(profileSelections.playOption)) {
    playOption = profileSelections.playOption;
  }
  return replay_autoplay::BuildSummary(
      replayLoadMetaForRecord(record), profileSelections.gaugeType,
      profileSelections.gaugeAutoShift, playOption, std::nullopt, std::nullopt,
      std::nullopt, profileSelections.assistOption,
      {.percent = context.settings.selectedPlaybackRatePercent,
       .mode = context.settings.selectedPlaybackMode},
      profileSelections.ruleset);
}

bool MainMenuScene::prepareAutoPlayChartForRecord(
    const ChartMetaRecord &record,
    std::unique_ptr<bms_parser::Chart> &preparedChart,
    play_options::PlayOptionReplayInfo &playInfo,
    std::atomic_bool &parseCancelled,
    const main_menu_profile::Selections &selections,
    const SelectedChartRandomInfo &chartRandomInfo) const {
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return false;
  }

  try {
    preparedChart = play_options::parseChart(
        record.meta.BmsPath, chartRandomInfo.seed, chartRandomInfo.prng,
        chartRandomInfo.values, parseCancelled, "autoplay");
  } catch (const std::exception &e) {
    SDL_Log("Error parsing %s for autoplay: %s",
            fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
    archive_file::appendDebugLogLine(
        "Autoplay parse exception: " + fspath_to_utf8(record.meta.BmsPath) +
        ": " + e.what());
  }
  if (preparedChart == nullptr || parseCancelled) {
    return false;
  }

  playInfo = play_options::applySelectedPlayOptions(
      *preparedChart, selections.playOption);
  applyEffectiveLongNoteModeToChart(
      *preparedChart,
      long_note_mode::valueFromId(selections.longNoteMode));
  return true;
}

void MainMenuScene::startAutoPlayPlayback(const ChartMetaRecord &record) {
  if (record.courseStart || willStart.load()) return;
  willStart.store(true);
  if (previewWorker_) previewWorker_->cancel();
  if (recordsModal_) recordsModal_->setLoadInProgress(true);
  const auto selections = profileSelections;
  const auto randomInfo = selectedChartRandomInfoForPath(record.meta.BmsPath);
  const audio::PlaybackRate autoPlayPlayback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode};
  const auto autoPlayRuleset = selections.ruleset;
  startReplayLoadWorker([this, record, selections, randomInfo, autoPlayPlayback,
                         autoPlayRuleset](std::shared_ptr<std::atomic_bool> cancelled) {
    if (previewWorker_) previewWorker_->stop();
    std::unique_ptr<bms_parser::Chart> autoPlayChart;
    play_options::PlayOptionReplayInfo playInfo;
    if (!prepareAutoPlayChartForRecord(record, autoPlayChart, playInfo,
                                      *cancelled, selections, randomInfo)) {
      if (!cancelled->load()) {
        queueReplayLoadCompletion([this] {
          (void)finishReplayLoadFailure(i18n::tr("menu.auto_play_failed.label"), {}, i18n::tr("menu.autoplay_chart_failed_prepared.message"));
        });
      }
      return;
    }
    context.jukebox.stop();
    context.jukebox.loadChart(*autoPlayChart, true, *cancelled);
    if (cancelled->load()) return;
    auto preparedChart = std::make_shared<std::unique_ptr<bms_parser::Chart>>(std::move(autoPlayChart));
    queueReplayLoadCompletion([this, preparedChart, playInfo, selections,
                               autoPlayPlayback, autoPlayRuleset]() mutable {
      auto *chart = setSelectedChart(std::move(*preparedChart), true, false);
      if (!chart) {
        (void)finishReplayLoadFailure(i18n::tr("menu.auto_play_failed.label"), {}, i18n::tr("menu.prepared_autoplay_chart_unavailable.message"));
        return;
      }
      if (recordsModal_) {
        recordsModal_->setLoadInProgress(false);
        recordsModal_->hide();
      }
        changeToGameplayScene(
            chart, {
                         .startPosition = 0,
                         .autoKeySound = true,
                         .autoPlay = true,
                         .gaugeType = selections.gaugeType,
                         .gaugeAutoShift = selections.gaugeAutoShift,
                         .gaugeAutoShiftLowerBound =
                             selections.gaugeAutoShiftLowerBound,
                         .playOption = playInfo.option,
                         .playOptionSeed = playInfo.seed,
                         .playOption2 = playInfo.option2,
                         .playOption2Seed = playInfo.seed2,
                         .longNoteMode = long_note_mode::valueFromId(
                             selections.longNoteMode),
                         .assistOption = selections.assistOption,
                         .pacemakerTarget = pacemaker::kTargetOff,
                         .playback = autoPlayPlayback,
                         .touchVisualizationEnabled = false,
                         .replayGhostRenderingEnabled = false,
                         .ruleset = autoPlayRuleset,
                   });
      willStart.store(false);
    });
  });
}

void MainMenuScene::startModernReplayPlayback(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || willStart.load()) {
    return;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const std::string pacemakerTarget =
      pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
  const bool replayAutoKeySound = recordsModal_ && recordsModal_->autoKeySound();
  const bool renderTouchPoints =
      recordsModal_ != nullptr ? recordsModal_->renderTouchPoints() : false;
  const bool renderGhosts =
      recordsModal_ != nullptr ? recordsModal_->renderReplayGhosts() : true;
  if (previewWorker_ != nullptr) {
    previewWorker_->cancelAndReleaseWhenIdle();
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern), pacemakerTarget,
       renderTouchPoints,
       renderGhosts, replayAutoKeySound](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          auto consumer = replay::makeRuntimeChartReplayConsumer(
              context.replayRepository);
          auto loaded = consumer.load(modern, record.meta.BmsPath, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  i18n::tr("menu.watch_failed.label"), diagnostic,
                  i18n::tr("menu.replay_playback_failed_prepared.message"));
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            context.jukebox.stop();
            context.jukebox.loadChart(*loaded.chart, true, *cancelled);
          }
          if (cancelled->load()) {
            return;
          }

          struct Completion {
            replay::ChartReplayConsumerOutcome loaded;
          };
          auto completion = std::make_shared<Completion>(
              Completion{.loaded = std::move(loaded)});
          queueReplayLoadCompletion(
              [this, completion, pacemakerTarget, renderTouchPoints,
               renderGhosts, replayAutoKeySound]() mutable {
                auto &loaded = completion->loaded;
                if (!loaded.diagnostic.empty()) {
                  publishReplayLoadDiagnostic(i18n::tr("menu.watch_warning.label"),
                                              loaded.diagnostic);
                }
                auto *chart =
                    setSelectedChart(std::move(loaded.chart), true, false);
                if (chart == nullptr) {
                  (void)finishReplayLoadFailure(
                      i18n::tr("menu.watch_failed.label"), {},
                      i18n::tr("menu.prepared_replay_chart_unavailable.message"));
                  return;
                }
                StartOptions replayOptions{
                    .startPosition = 0,
                    .autoKeySound = replayAutoKeySound,
                    .autoPlay = false,
                    .gaugeType = loaded.replayData->initialGaugeType,
                    .gaugeAutoShift = loaded.replayData->gaugeAutoShift,
                    .replayData = loaded.replayData,
                    .pacemakerTarget = pacemakerTarget,
                    .touchVisualizationEnabled = renderTouchPoints,
                    .replayGhostRenderingEnabled = renderGhosts,
                };
                applyReplayProvenanceToStartOptions(replayOptions,
                                                    *loaded.replayData);
                context.jukebox.stop();
                if (recordsModal_ != nullptr) recordsModal_->hide();
                changeToGameplayScene(chart, std::move(replayOptions));
                willStart.store(false);
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                i18n::tr("menu.watch_failed.label"), {}, i18n::tr("menu.replay_playback_failed_prepared.message"));
          });
        }
      });
}

void MainMenuScene::startModernGBattlePlayback(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || willStart.load()) {
    return;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const GaugeType gaugeType = profileSelections.gaugeType;
  const GaugeAutoShiftMode gaugeAutoShift = profileSelections.gaugeAutoShift;
  const GaugeType gaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const GameplayRuleset ruleset = profileSelections.ruleset;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };

  if (previewWorker_ != nullptr) {
    previewWorker_->cancelAndReleaseWhenIdle();
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern), gaugeType, gaugeAutoShift,
       gaugeAutoShiftLowerBound, autoKeySound, ruleset,
       playback](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          auto consumer = replay::makeRuntimeChartReplayConsumer(
              context.replayRepository);
          auto loaded = consumer.load(modern, record.meta.BmsPath, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  i18n::tr("menu.g_battle_failed.label"), diagnostic,
                  i18n::tr("menu.g_battle_replay_failed_prepared.message"));
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            context.jukebox.stop();
            context.jukebox.loadChart(*loaded.chart, true, *cancelled);
          }
          if (cancelled->load()) {
            return;
          }

          struct Completion {
            replay::ChartReplayConsumerOutcome loaded;
            result_persistence::ChartScoreWrite targetScore;
          };
          auto completion = std::make_shared<Completion>(
              Completion{.loaded = std::move(loaded),
                         .targetScore = modern.result.score});
          queueReplayLoadCompletion(
              [this, completion, gaugeType, gaugeAutoShift,
               gaugeAutoShiftLowerBound, autoKeySound, ruleset,
               playback]() mutable {
                auto &loaded = completion->loaded;
                if (!loaded.diagnostic.empty()) {
                  publishReplayLoadDiagnostic(i18n::tr("menu.g_battle_warning.label"),
                                              loaded.diagnostic);
                }
                auto *chart =
                    setSelectedChart(std::move(loaded.chart), true, false);
                if (chart == nullptr) {
                  (void)finishReplayLoadFailure(
                      i18n::tr("menu.g_battle_failed.label"), {},
                      i18n::tr("menu.prepared_replay_chart_unavailable.message"));
                  return;
                }
                auto recordData = loaded.replayData;
                context.jukebox.stop();
                if (recordsModal_ != nullptr) recordsModal_->hide();
                changeToGameplayScene(
                    chart, {
                               .startPosition = 0,
                               .autoKeySound = autoKeySound,
                               .autoPlay = false,
                               .gaugeType = gaugeType,
                               .gaugeAutoShift = gaugeAutoShift,
                               .gaugeAutoShiftLowerBound =
                                   gaugeAutoShiftLowerBound,
                               .gbattleRecordData = recordData,
                               .targetScore = completion->targetScore,
                               .playOption = recordData->playOption,
                               .playOptionSeed = recordData->playOptionSeed,
                               .playOption2 = recordData->playOption2,
                               .playOption2Seed = recordData->playOption2Seed,
                               .longNoteMode = normalizeChartLongNoteModeValue(
                                   recordData->chartMeta.LnMode),
                               .assistOption = recordData->assistOption,
                               .pacemakerTarget = pacemaker::kTargetOff,
                               .playback = playback,
                               .replayGhostRenderingEnabled = false,
                               .ruleset = ruleset,
                           });
                willStart.store(false);
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                i18n::tr("menu.g_battle_failed.label"), {},
                i18n::tr("menu.g_battle_replay_failed_prepared.message"));
          });
        }
      });
}

void MainMenuScene::startModernCourseReplayPlayback(
    const ChartMetaRecord &record, ModernCourseResultRecord modern) {
  if (!record.courseStart || willStart.load()) {
    return;
  }
  auto currentSelection = currentCourseSelectionFor(modern.result);
  if (!currentSelection) {
    return;
  }
  auto chartPaths = std::move(currentSelection->completedChartPaths);

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const auto pacemakerTarget = profileSelections.pacemakerTarget;
  const bool replayAutoKeySound = recordsModal_ && recordsModal_->autoKeySound();
  const bool renderTouchPoints =
      recordsModal_ != nullptr ? recordsModal_->renderTouchPoints() : false;
  const bool renderGhosts =
      recordsModal_ != nullptr ? recordsModal_->renderReplayGhosts() : true;
  if (previewWorker_ != nullptr) {
    previewWorker_->cancelAndReleaseWhenIdle();
  }
  startReplayLoadWorker(
      [this, modern = std::move(modern), chartPaths = std::move(chartPaths),
       renderTouchPoints, pacemakerTarget,
       renderGhosts, replayAutoKeySound](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }

          auto consumer =
              replay::makeRuntimeCourseReplayConsumer(context.replayRepository);
          auto loaded = consumer.load(modern, chartPaths, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  i18n::tr("menu.course_watch_failed.label"), diagnostic,
                  i18n::tr("menu.course_replay_playback_failed_prepared.message"));
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          const std::string warning = loaded.diagnostic;
          auto session = replay::makeCourseReplayLaunchSession(
              std::move(loaded), replay::CourseReplayLaunchMode::Watch,
              renderTouchPoints, renderGhosts);
          if (session != nullptr) session->autoKeySound = replayAutoKeySound;
          if (session == nullptr) {
            queueReplayLoadCompletion([this]() {
              (void)finishReplayLoadFailure(
                  i18n::tr("menu.course_watch_failed.label"), {},
                  i18n::tr("menu.prepared_course_replay_session_unavailable.message"));
            });
            return;
          }
          auto stageReplay = session->currentCourseReplayStageReplay();
          auto chart = session->takePreparedCourseChart(session->currentIndex);
          if (!stageReplay || !chart) {
            queueReplayLoadCompletion([this] {
              (void)finishReplayLoadFailure(i18n::tr("menu.course_watch_failed.label"), {},
                                           i18n::tr("menu.prepared_course_replay_chart_unavailable.message"));
            });
            return;
          }
          session->applyReplayStagePlayOptions(*stageReplay);
          context.jukebox.stop();
          context.jukebox.loadChart(*chart, true, *cancelled);
          if (cancelled->load()) return;
          StartOptions options = makeCourseReplayStageStartOptions(session, stageReplay);
          options.pacemakerTarget = pacemakerTarget;
          options.returnScene = this;
          auto preparedChart = std::make_shared<std::unique_ptr<bms_parser::Chart>>(std::move(chart));
          queueReplayLoadCompletion(
              [this, preparedChart, options = std::move(options), warning]() mutable {
                if (!warning.empty()) publishReplayLoadDiagnostic(i18n::tr("menu.course_watch_warning.label"), warning);
                if (recordsModal_) recordsModal_->hide();
                context.sceneManager->changeScene(std::make_unique<GamePlayScene>(
                    context, std::move(*preparedChart), std::move(options)), true);
                willStart.store(false);
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                i18n::tr("menu.course_watch_failed.label"), {},
                i18n::tr("menu.course_replay_playback_failed_prepared.message"));
          });
        }
      });
}

bms_parser::Chart *
MainMenuScene::setSelectedChart(std::unique_ptr<bms_parser::Chart> chart,
                                bool mediaReady, bool reusableForStart) {
  bms_parser::Chart *raw = chart.get();
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChart = std::move(chart);
    selectedChartMediaReady.store(mediaReady);
    selectedChartReusableForStart.store(reusableForStart);
  }
  return raw;
}

void MainMenuScene::clearSelectedChart() {
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChartMediaReady.store(false);
    selectedChartReusableForStart.store(false);
  }
}

void MainMenuScene::stopAndClearSelectedChart() {
  context.jukebox.stop();
  clearSelectedChart();
}

MainMenuScene::SelectedChartRandomInfo
MainMenuScene::selectedChartRandomInfoForPath(
    const std::filesystem::path &path) const {
  SelectedChartRandomInfo info;
  std::lock_guard<std::mutex> lock(selectedChartMutex);
  if (!selectedChartReusableForStart.load() || selectedChart == nullptr ||
      fspath_to_path_t(selectedChart->Meta.BmsPath) != fspath_to_path_t(path)) {
    return info;
  }
  info.seed = selectedChart->Meta.RandomSeed;
  info.prng = selectedChart->Meta.RandomPrng;
  if (!selectedChart->Meta.RandomValues.empty()) {
    info.values = selectedChart->Meta.RandomValues;
  }
  return info;
}

bms_parser::Chart *MainMenuScene::loadedSelectedChartForPath(
    const std::filesystem::path &path) const {
  std::lock_guard<std::mutex> lock(selectedChartMutex);
  if (!selectedChartMediaReady.load() || !selectedChartReusableForStart.load() ||
      selectedChart == nullptr ||
      fspath_to_path_t(selectedChart->Meta.BmsPath) != fspath_to_path_t(path)) {
    return nullptr;
  }
  return selectedChart.get();
}

void MainMenuScene::resetStartLoadingUi() {
  willStart.store(false);
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setVisible(false);
  }
  refreshStartButtonForActiveFolder();
}

void MainMenuScene::resetReplayWatchLoadingUi() {
  willStart.store(false);
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(false);
}

void MainMenuScene::publishReplayLoadDiagnostic(
    const char *action, const std::string &diagnostic) const {
  const std::string safeDiagnostic = ir::sanitizeDiagnostic(diagnostic);
  if (safeDiagnostic.empty()) {
    return;
  }
  SDL_Log("Replay %s: %s", action, safeDiagnostic.c_str());
  archive_file::appendDebugLogLine("Replay " + std::string(action) + ": " +
                                   safeDiagnostic);
}

bool MainMenuScene::finishReplayLoadFailure(const char *action,
                                            std::string diagnostic,
                                            const char *fallback) {
  const std::string safeDiagnostic = replay_records::diagnosticOr(diagnostic, fallback);
  publishReplayLoadDiagnostic(action, safeDiagnostic);
  resetReplayWatchLoadingUi();
  if (recordsModal_ != nullptr) {
    recordsModal_->setStatus(safeDiagnostic);
    recordsModal_->reloadRecords(true);
  }
  return true;
}

void MainMenuScene::startReplayLoadWorker(
    std::function<void(std::shared_ptr<std::atomic_bool>)> work) {
  replayLoadTask_.start([this, work = std::move(work)](std::shared_ptr<std::atomic_bool> cancelled) {
    try {
      work(cancelled);
    } catch (const std::exception &error) {
      const auto diagnostic = replay_records::diagnosticOr(error.what(), i18n::tr("menu.records_preparation_failed.message"));
      queueReplayLoadCompletion([this, diagnostic] {
        (void)finishReplayLoadFailure("preparation failed", diagnostic, i18n::tr("menu.records_preparation_failed.message"));
      });
    } catch (...) {
      queueReplayLoadCompletion([this] {
        (void)finishReplayLoadFailure("preparation failed", {}, i18n::tr("menu.records_preparation_failed.message"));
      });
    }
  });
}

void MainMenuScene::queueReplayLoadCompletion(
    std::function<void()> completion) {
  replayLoadTask_.publish(std::move(completion));
}

void MainMenuScene::applyReplayLoadCompletion() {
  if (context.appInBackground.load()) return;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (context.gameplaySkinLifecycle &&
      !context.gameplaySkinLifecycle->presentationReady()) {
    return;
  }
#endif
  if (auto completion = replayLoadTask_.takeCompletion()) {
    resetReplayWatchLoadingUi();
    replayResultRecallInProgress = false;
    if (recordsModal_) recordsModal_->setResultRecallInProgress(false);
    completion();
  }
}

void MainMenuScene::stopReplayLoadWorker() {
  replayLoadTask_.cancelAndWait();
  replayResultRecallInProgress = false;
  if (recordsModal_ != nullptr) {
    recordsModal_->setLoadInProgress(false);
    recordsModal_->setResultRecallInProgress(false);
  }
}

void MainMenuScene::changeToGameplayScene(bms_parser::Chart *chart,
                                          StartOptions options) {
  // Mode finalization can demote malformed holds. A later launch with another
  // LN mode must reparse, including while the resume preview is still loading.
  selectedChartReusableForStart.store(false);
  if (options.replayData == nullptr) {
    options.clubMode = context.settings.gameplayClubModeEnabled;
  }
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, chart, std::move(options)),
      true);
}

bool MainMenuScene::beginReplayExport(const i18n::Text &progressTitle,
                                      const i18n::Text &progressMessage,
                                      const i18n::Text &statusMessage) {
  if (!replayExportJob_.tryBegin()) {
    return false;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(true);
    recordsModal_->showExportProgress(progressTitle, progressMessage);
    recordsModal_->setStatus(statusMessage);
  }
  return true;
}

void MainMenuScene::preparePreviewForReplayExport() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  context.jukebox.stop();
}

void MainMenuScene::startAutoPlayVideoExport(
    const ChartMetaRecord &record, ReplayVideoExportOptions options) {
  if (!beginReplayExport(i18n::message("menu.exporting_replay.label"), i18n::message("menu.preparing_export.label"),
                         i18n::message("menu.exporting.progress"))) {
    return;
  }

  const auto selections = profileSelections;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const bool clubMode = context.settings.gameplayClubModeEnabled;
  const SelectedChartRandomInfo randomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  replayExportJob_.start(std::move(options),
      [this, record, selections, playback, clubMode, randomInfo](
          const ReplayVideoExportOptions &options,
          std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        preparePreviewForReplayExport();
        if (cancelled) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }

        std::unique_ptr<bms_parser::Chart> chart;
        try {
          chart = play_options::parseChart(
              record.meta.BmsPath, randomInfo.seed, randomInfo.prng,
              randomInfo.values, cancelled, "autoplay export");
        } catch (const std::exception &error) {
          SDL_Log("Error parsing %s for autoplay export: %s",
                  fspath_to_utf8(record.meta.BmsPath).c_str(), error.what());
          archive_file::appendDebugLogLine(
              "Autoplay export parse exception: " +
              fspath_to_utf8(record.meta.BmsPath) + ": " + error.what());
        }
        if (chart == nullptr || cancelled) {
          return {.success = false, .message = "No Chart"};
        }
        if (options.stop.stop_requested()) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }

        const auto playInfo =
            play_options::applySelectedPlayOptions(*chart, selections.playOption);
        applyEffectiveLongNoteModeToChart(
            *chart, long_note_mode::valueFromId(selections.longNoteMode));
        ReplayData replay = replay_autoplay::BuildReplayData(
            *chart, selections.gaugeType, selections.gaugeAutoShift, playback,
            playInfo.option, playInfo.seed, playInfo.option2, playInfo.seed2,
            selections.assistOption, clubMode, selections.gaugeAutoShiftLowerBound,
            selections.ruleset);
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.renderTouchPoints = false;
        exportOptions.renderReplayGhosts = false;
        return ReplayVideoExporter::Export(context, chart.get(), replay,
                                            exportOptions);
      });
}

void MainMenuScene::startModernReplayVideoExport(
    const ChartMetaRecord &record, ModernChartResultRecord modern,
    ReplayVideoExportOptions options) {
  if (!beginReplayExport(i18n::message("menu.exporting_replay.label"), i18n::message("menu.preparing_export.label"),
                         i18n::message("menu.exporting.progress"))) {
    return;
  }

  replayExportJob_.start(std::move(options),
      [this, record, modern = std::move(modern)](
          const ReplayVideoExportOptions &options,
          std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        preparePreviewForReplayExport();
        if (cancelled) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }
        auto consumer = replay::makeRuntimeChartReplayConsumer(
            context.replayRepository);
        auto loaded = consumer.load(modern, record.meta.BmsPath, cancelled);
        if (cancelled) {
          return {.success = false,
                  .message = i18n::tr("menu.replay_export_preparation_cancelled.message")};
        }
        if (!loaded.ready()) {
          return {.success = false,
                  .message = replay_records::diagnosticOr(
                      loaded.diagnostic,
                      i18n::tr("menu.replay_export_playback_failed_prepared.message"))};
        }
        if (!loaded.diagnostic.empty()) {
          publishReplayLoadDiagnostic("video export warning", loaded.diagnostic);
        }
        if (options.stop.stop_requested()) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }
        return ReplayVideoExporter::Export(
            context, loaded.chart.get(), *loaded.replayData, options);
      });
}

void MainMenuScene::startModernCourseReplayVideoExport(
    ModernCourseResultRecord modern, ReplayVideoExportOptions options) {
  auto currentSelection = currentCourseSelectionFor(modern.result);
  if (!currentSelection) {
    return;
  }
  auto chartPaths = std::move(currentSelection->completedChartPaths);
  if (!beginReplayExport(i18n::message("menu.exporting_course_replay.label"), i18n::message("menu.preparing_export.label"),
                         i18n::message("menu.exporting.progress"))) {
    return;
  }

  replayExportJob_.start(std::move(options),
      [this, modern = std::move(modern), chartPaths = std::move(chartPaths)](
          const ReplayVideoExportOptions &options,
          std::atomic_bool &cancelled) -> ReplayVideoExportResult {
        preparePreviewForReplayExport();
        if (cancelled) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }
        auto consumer = replay::makeRuntimeCourseReplayConsumer(
            context.replayRepository);
        auto loaded = consumer.load(modern, chartPaths, cancelled);
        if (cancelled) {
          return {.success = false,
                  .message = i18n::tr("menu.course_replay_export_preparation_cancelled.message")};
        }
        if (!loaded.ready()) {
          return {.success = false,
                  .message = replay_records::diagnosticOr(
                      loaded.diagnostic,
                      i18n::tr("menu.course_replay_export_playback_failed_prepared.message"))};
        }
        if (!loaded.diagnostic.empty()) {
          publishReplayLoadDiagnostic("course video export warning",
                                      loaded.diagnostic);
        }
        if (options.stop.stop_requested()) {
          return {.success = false, .message = i18n::tr("menu.replay_export_cancelled.label")};
        }
        return ReplayVideoExporter::ExportCourseReplay(
            context, std::move(loaded), options);
      });
}

std::optional<std::string>
MainMenuScene::activeReplayIrServerOrigin() const {
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (settings == context.settings.irProviders.end()) {
    return std::string(ir::kDefaultTachiServerOrigin);
  }
  return ir::normalizeServerOrigin(settings->second.serverOrigin);
}

void MainMenuScene::publishReplayIrStatusFeedback(ir::IrRecordState state) {
  const auto message = replay_records::irStatusFeedback(state);
  if (!message.empty() && recordsModal_ != nullptr) {
    recordsModal_->showIrFeedback(std::string(message));
  }
}

void MainMenuScene::observeReplayIrServiceRevisions() {
  if (recordsModal_ == nullptr || !recordsModal_->isVisible() ||
      recordsModal_->records().empty() ||
      context.irSubmissionService == nullptr) {
    return;
  }

  bool reload = false;
  for (const ResultRecordSummary &summary : recordsModal_->records()) {
    if (!summary.modern.has_value()) {
      continue;
    }
    const std::string &attemptId = summary.modern->result.attemptId;
    const auto status = context.irSubmissionService->status(
        ir::kTachiProviderId, attemptId);
    const auto observed = replayIrObservedRevisions.find(attemptId);
    if (observed != replayIrObservedRevisions.end() &&
        observed->second == status.revision) {
      continue;
    }
    replayIrObservedRevisions[attemptId] = status.revision;
    reload = true;
  }
  if (reload) {
    recordsModal_->reloadRecords(true);
  }
}

void MainMenuScene::startModernReplayIrUpload(
    ModernChartResultRecord modern) {
  if (replayIrUploadInProgress || replayResultRecallInProgress ||
      replayExportJob_.inProgress()) {
    return;
  }

  if (const auto unavailable = replay_records::irUploadUnavailable(context)) {
    finishReplayIrUpload(modern.result.attemptId, *unavailable);
    return;
  }

  replayIrUploadInProgress = true;
  if (recordsModal_ != nullptr) {
    recordsModal_->setIrUploadInProgress(true);
    recordsModal_->showIrFeedback(i18n::message("menu.preparing_ir.progress"));
  }
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }

  defer(
      [this, modern = std::move(modern)]() {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          finishReplayIrUpload(
              modern.result.attemptId,
              replay_records::uploadSavedResult(context, modern.result.attemptId));
        } catch (...) {
          finishReplayIrUpload(modern.result.attemptId,
                               i18n::tr("menu.ir_upload_failed_prepared.message"));
        }
        return true;
      },
      1, true);
}

void MainMenuScene::finishReplayIrUpload(std::string attemptId,
                                         std::string message) {
  replayIrUploadInProgress = false;
  std::string safeMessage = ir::sanitizeDiagnostic(message);
  if (safeMessage.empty()) {
    safeMessage = i18n::tr("menu.ir_upload_failed_queued.message");
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setIrUploadInProgress(false);
    if (!attemptId.empty()) {
      recordsModal_->reloadRecords(true);
    }
    recordsModal_->showIrFeedback(safeMessage);
  }
}

void MainMenuScene::startModernReplayResultRecall(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || replayResultRecallInProgress ||
      replayExportJob_.inProgress() || replayIrUploadInProgress) {
    return;
  }

  replayResultRecallInProgress = true;
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancelAndReleaseWhenIdle();
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern)](
          std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          auto prepared = chart_records::prepareChartResult(
              context.replayRepository, record, modern.result.attemptId, *cancelled);
          if (cancelled->load()) return;
          if (!prepared.completion) {
            queueReplayLoadCompletion([this, diagnostic = std::move(prepared.diagnostic)] {
              finishReplayResultRecallFailure(diagnostic);
            });
            return;
          }
          auto completion = std::move(prepared.completion);
          queueReplayLoadCompletion([this, completion]() mutable {
            auto &result = completion->view;
            auto chart = std::move(result.chart);
            const bms_parser::ChartMeta meta = chart->Meta;
            const std::string attemptId = result.result.attemptId;
            const ScoreProvenance provenance = result.result.score.provenance;
            const SkinGameplayGraphState gameplayGraph =
                completion->retryData != nullptr
                    ? replay_result::BuildSkinGameplayGraphState(
                          *chart, *completion->retryData, result.state)
                    : replay_result::BuildSkinGameplayChartGraphState(
                          *chart, result.state);
            replayResultRecallInProgress = false;
            if (recordsModal_ != nullptr) {
              recordsModal_->setResultRecallInProgress(false);
            }
            context.sceneManager->changeScene(
                std::make_unique<ResultScene>(
                    context, meta, result.state, provenance, nullptr,
                    ResultPersistenceOptions{}, completion->retryData.get(),
                    ResultPracticeOptions{}, false, ResultCourseOptions{},
                    profileSelections.pacemakerTarget, std::move(chart),
                    nullptr, std::nullopt, completion->retryData.get(),
                    attemptId, completion->retryData != nullptr,
                    ResultTableContext{}, gameplayGraph,
                    result.result.playedAtUnixMillis),
                true);
          });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            finishReplayResultRecallFailure(
                "saved chart result could not be recalled");
          });
        }
      });
}

void MainMenuScene::startModernCourseReplayResultRecall(
    ModernCourseResultRecord modern, bool retrySameAllowed) {
  if (replayResultRecallInProgress || replayExportJob_.inProgress() ||
      replayIrUploadInProgress) {
    return;
  }

  auto currentSelection = currentCourseSelectionFor(modern.result);

  replayResultRecallInProgress = true;
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancelAndReleaseWhenIdle();
  }
  startReplayLoadWorker([this, modern = std::move(modern), retrySameAllowed,
                         currentSelection = std::move(currentSelection)](
                            std::shared_ptr<std::atomic_bool>
                                cancelled) mutable {
    try {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      auto prepared = course_records::prepareCourseResult(
          context.replayRepository, modern.result.attemptId, currentSelection,
          retrySameAllowed, *cancelled);
      if (cancelled->load()) {
        return;
      }
      if (!prepared.session) {
        queueReplayLoadCompletion([this, diagnostic = std::move(prepared.diagnostic)]() {
          finishReplayResultRecallFailure(diagnostic);
        });
        return;
      }
      auto session = std::move(prepared.session);
      queueReplayLoadCompletion([this, session = std::move(session)]() {
        context.jukebox.stop();
        const auto &first = session->completedResults.front();
        const ScoreProvenance firstProvenance =
            *session->stageProvenance.front();
        const ReplayData *firstReplay = session->resultBrowseStageReplay(0);
        bms_parser::Chart *firstReplayChart =
            session->resultBrowseReplayChart(0);
        if (firstReplay != nullptr) {
          session->applyReplayStagePlayOptions(*firstReplay);
        }
        replayResultRecallInProgress = false;
        if (recordsModal_ != nullptr) {
          recordsModal_->setResultRecallInProgress(false);
        }
        context.sceneManager->changeScene(
            std::make_unique<ResultScene>(
                context, first.meta, first.state, firstProvenance, firstReplay,
                ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{},
                false,
                ResultCourseOptions{.mode = ResultCourseMode::Stage,
                                    .session = session,
                                    .savedResultBrowsing = true},
                profileSelections.pacemakerTarget,
                std::unique_ptr<bms_parser::Chart>{}, firstReplayChart,
                std::nullopt, firstReplay, std::nullopt, true,
                ResultTableContext{}, first.gameplayGraph,
                session->modernCoursePlayedAtUnixMillis),
                true);
      });
    } catch (...) {
      queueReplayLoadCompletion([this]() {
        finishReplayResultRecallFailure(
            "saved course result could not be recalled");
      });
    }
  });
}

void MainMenuScene::startRemoteResultRecall(IrRemoteRecordId identity,
                                            std::string selectedStableKey) {
  if (replayResultRecallInProgress || replayExportJob_.inProgress() ||
      replayIrUploadInProgress || identity.providerId.empty() ||
      identity.serverOrigin.empty() || identity.remoteScoreId.empty() ||
      selectedStableKey.empty()) {
    return;
  }
  replayResultRecallInProgress = true;
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  startReplayLoadWorker(
      [this, identity = std::move(identity),
       selectedStableKey = std::move(selectedStableKey)](std::shared_ptr<std::atomic_bool> cancelled) {
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }

        RemoteResultRecallRequest request{
            .identity = std::move(identity),
            .selectedStableKey = std::move(selectedStableKey),
        };
        auto loaded = context.replayRepository.LoadIrRemoteScore(
            request.identity.providerId, request.identity.serverOrigin,
            request.identity.remoteScoreId);
        if (cancelled->load()) return;
        queueReplayLoadCompletion([this, request, loaded = std::move(loaded)]() mutable {
        RemoteResultRecallCallbacks callbacks{
            .selectionStillMatches = [this](const RemoteResultRecallRequest &request) {
              return remoteResultRecallSelectionMatches(
                  recordsModal_ != nullptr ? recordsModal_->selection()
                                           : std::optional<ResultRecordSummary>{},
                  request);
            },
            .loadExact = [&loaded](const IrRemoteRecordId &) { return std::move(loaded); },
            .transition = [this](ResultRemoteOptions remote,
                                 bool retainCurrentScene) {
              remote.returnScene = this;
              auto next =
                  std::make_unique<ResultScene>(context, std::move(remote));
              replayResultRecallInProgress = false;
              if (recordsModal_ != nullptr) {
                recordsModal_->setResultRecallInProgress(false);
              }
              context.jukebox.stop();
              context.sceneManager->changeScene(std::move(next),
                                                retainCurrentScene);
              return true;
            },
            .failAndReload = [this](std::string diagnostic) {
              finishRemoteResultRecallFailure(std::move(diagnostic));
            },
        };
        (void)executeRemoteResultRecall(request, callbacks);
        });
      });
}

void MainMenuScene::finishReplayResultRecallFailure(std::string diagnostic) {
  const std::string safeDiagnostic = ir::sanitizeDiagnostic(diagnostic);
  SDL_Log("Saved result recall failed: %s",
          safeDiagnostic.empty() ? "result unavailable"
                                 : safeDiagnostic.c_str());
  if (recordsModal_ != nullptr) {
    recordsModal_->setResultRecallInProgress(false);
    if (!safeDiagnostic.empty()) {
      recordsModal_->setStatus(safeDiagnostic);
    }
  }
  replayResultRecallInProgress = false;
}

void MainMenuScene::finishRemoteResultRecallFailure(std::string diagnostic) {
  if (recordsModal_ != nullptr) {
    recordsModal_->reloadRecords(true);
  }
  finishReplayResultRecallFailure(std::move(diagnostic));
}

void MainMenuScene::applyReplayExportProgress() {
  const auto progress = replayExportJob_.takeProgress();
  if (progress && recordsModal_ != nullptr) {
    recordsModal_->updateExportProgress(progress->fraction, progress->message);
  }
}

void MainMenuScene::applyReplayExportResult() {
  const auto result = replayExportJob_.takeResult();
  if (!result) {
    return;
  }
  willStart.store(false);
  (void)replayExportJob_.takeProgress();

  if (recyclerView != nullptr) {
    const int selected = recyclerView->selectedIndex;
    if (selected >= 0 && selected < recyclerView->size()) {
      const auto &selectedMeta = recyclerView->get(selected);
      if (!selectedMeta.unavailable && !selectedMeta.meta.BmsPath.empty() &&
          recyclerView->onSelected) {
        recyclerView->onSelected(selectedMeta, selected);
      }
    }
  }

  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(false);
    recordsModal_->returnToList(
        result->success
            ? (result->message == "Saved to Photos" ? i18n::message("menu.saved.label") : i18n::message("menu.exported.label"))
            : (result->message == "No Chart"
                   ? i18n::message("menu.no_chart.label")
                   : (result->message.empty()
                          ? i18n::message("menu.replay_export_failed.message")
                          : i18n::Text(result->message))));
  }

  if (result->success) {
    SDL_Log("Replay video exported: %s (%s)",
            fspath_to_utf8(result->outputPath).c_str(),
            result->message.c_str());
  } else {
    SDL_Log("Replay video export failed: %s (%s)", result->message.c_str(),
            fspath_to_utf8(result->outputPath).c_str());
  }
}

void MainMenuScene::update(float dt) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (presentationSkinRefreshPending &&
      (!context.gameplaySkinLifecycle || context.gameplaySkinLifecycle->presentationReady())) {
    presentationSkinRefreshPending = false;
    queueSelectedSkinHandoff();
  }
#endif
  // Update the scene logic
  // std::cout << "Updating Main Menu Scene, dt: " << dt << std::endl;
  refreshScoreClearRanksIfNeeded();
  refreshIrRecordListIfNeeded();
  refreshTasksButton();
  applyPendingUiUpdates();
  applyFindBmsUpdates();
  if (archiveUnzipModal_ != nullptr) {
    archiveUnzipModal_->update();
  }
  applyReplayLoadCompletion();
  applyReplayExportProgress();
  applyReplayExportResult();
  applyReplayFileDocumentHandoff();
  applyParseLogDocumentHandoff();
  observeReplayIrServiceRevisions();
  if (recordsModal_ != nullptr) {
    recordsModal_->update();
  }
  if (parseLogModalRoot != nullptr && parseLogModalRoot->getVisible()) {
    refreshParseLogModal();
  }
  i18n::Text nativeMusicStatusMessage;
  if (context.musicPlayer.ProcessNativeControlEvents(
          nativeMusicStatusMessage)) {
    musicStatusMessage = nativeMusicStatusMessage;
  }
  if (context.musicPlayer.ConsumeNativeControlStatus(
          nativeMusicStatusMessage)) {
    musicStatusMessage = nativeMusicStatusMessage;
  }
  if (musicModalRoot != nullptr && musicModalRoot->getVisible()) {
    refreshMusicModal();
  }
  if (tasksModalRoot != nullptr && tasksModalRoot->getVisible()) {
    refreshTasksModal();
  }
  if (rankingsModal) {
    rankingsModal->update();
  }
  if (rootLayout && rendering::window_height > rendering::window_width) {
    const auto safe = getSafeAreaInsetsUi();
    const float availableHeight = std::max(0, rendering::window_height - safe.top - safe.bottom - 2 * kRootPadding - 24);
    const auto *details = rootLayout->findViewByName("mainMenuDetails");
    if (details && std::abs(details->getHeight() - portraitDetailsHeight(availableHeight)) > 1) {
      updatePanelLayout();
    }
  }
}

void MainMenuScene::renderScene() {
  // Render the scene
  // SDL_Log("Rendering Main Menu Scene");
  if (rootLayout == nullptr) {
    return;
  }
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  const bool layoutChanged =
      rendering::window_width != lastLayoutWidth ||
      rendering::window_height != lastLayoutHeight || safe.top != lastSafeTop ||
      safe.left != lastSafeLeft || safe.bottom != lastSafeBottom ||
      safe.right != lastSafeRight;
  rootLayout->setSize(rendering::window_width, rendering::window_height);
  if (recordsModal_ != nullptr) {
    recordsModal_->resize(rendering::window_width, rendering::window_height);
  }
  if (playOptionsModalRoot != nullptr) {
    playOptionsModalRoot->setSize(rendering::window_width,
                                  rendering::window_height);
  }
  if (overlayPortal != nullptr) {
    overlayPortal->setSize(rendering::window_width, rendering::window_height);
  }
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setSize(rendering::window_width, rendering::window_height);
  }
  if (revealContextMenu != nullptr) {
    revealContextMenu->setViewportSize(rendering::window_width,
                                       rendering::window_height);
    if (layoutChanged && revealContextMenu->isOpen()) {
      revealContextMenu->dismiss();
    }
  }
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setSize(rendering::window_width,
                               rendering::window_height);
  }
  if (musicModalRoot != nullptr) {
    musicModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
  if (tasksModalRoot != nullptr) {
    tasksModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
#if TARGET_OS_ANDROID
  if (layoutChanged) {
    resizeFileActionsModal();
    resizeTasksModal();
  }
#endif
  if (findBmsModal_ != nullptr) {
    findBmsModal_->resize(rendering::window_width, rendering::window_height);
  }
  if (archiveUnzipModal_ != nullptr) {
    archiveUnzipModal_->resize(rendering::window_width, rendering::window_height);
  }
  if (layoutChanged) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    lastSafeTop = safe.top;
    lastSafeLeft = safe.left;
    lastSafeBottom = safe.bottom;
    lastSafeRight = safe.right;
    rootLayout->setPadding(Edge::Top, safe.top + kRootPadding);
    rootLayout->setPadding(Edge::Left, safe.left + kRootPadding);
    rootLayout->setPadding(Edge::Right, safe.right + kRootPadding);
    rootLayout->setPadding(Edge::Bottom, safe.bottom + kRootPadding);
    updatePanelLayout();
    rootLayout->applyYogaLayout();
  }
  if (tutorial_ && tutorial_->getVisible()) {
    tutorial_->updateLayout(rendering::window_width, rendering::window_height);
  }
}

void MainMenuScene::cleanupScene() {
  tutorial_ = nullptr;
  addFolderButton_ = nullptr;
  tutorialRightScroll_ = nullptr;
  detailsContent_ = nullptr;
  detailsControlsContent_ = nullptr;
  detailsControlsScroll_ = nullptr;
  // Cleanup resources when exiting the scene
  revealContextMenu.reset();
  rankingsModal.reset();
  playOptionsModal.reset();
  if (recordFileActions_) recordFileActions_->close();
  parseLogDocumentHandoff.close();
  stopReplayAndPreviewWork();
  context.profileSwitchBlockers.scene = nullptr;
  context.profileSwitchBlockers.background = nullptr;
  context.refreshProfileCaches = nullptr;
  if (replayExportJob_.hasWorker()) {
    SDL_Log("Joining replayExportThread");
    replayExportJob_.cancelAndWait();
  }
  findBmsModal_.reset();
  archiveUnzipModal_.reset();
  stopAndClearSelectedChart();
  selectedChartRecord.reset();
  chartListCache.clear();
  chartSession.reset();
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  temporaryChartFolder.reset();
  rootLayout = nullptr;
  if (decideOverlay_ != nullptr) {
    overlayPortal->dismiss(decideOverlay_);
    delete decideOverlay_;
    decideOverlay_ = nullptr;
  }
  previewWorker_.reset();
  overlayPortal = nullptr;
  jacketView = nullptr;
  chartDetailsView_ = nullptr;
  searchBox = nullptr;
  startButton = nullptr;
  rankingsButton = nullptr;
  rankingsButtonText = nullptr;
  chartActionsRow = nullptr;
  revealButton = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  findBmsButtonSlot = nullptr;
  findBmsButton = nullptr;
  findBmsButtonText = nullptr;
  unzipButtonSlot = nullptr;
  unzipButton = nullptr;
  unzipButtonText = nullptr;
  parseLogButton = nullptr;
  parseLogButtonText = nullptr;
  musicButton = nullptr;
  musicButtonText = nullptr;
  irUploadsButton = nullptr;
  irUploadsButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  recordsModal_.reset();
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  musicModalRoot = nullptr;
  parseLogModalRoot = nullptr;
  tasksModalRoot = nullptr;
#if TARGET_OS_ANDROID
  fileActionsModalRoot_ = nullptr;
  fileActionsPanel_ = nullptr;
  folderImportPanel_ = nullptr;
  computerImportPanel_ = nullptr;
#endif
  parseLogRecyclerView = nullptr;
  parseLogExportStatusText = nullptr;
  parseLogExportButton = nullptr;
  parseLogExportButtonText = nullptr;
  parseLogCloseButton = nullptr;
  parseLogCloseButtonText = nullptr;
  musicTrackText = nullptr;
  musicStatusText = nullptr;
  musicPlaylistText = nullptr;
  musicSelectedButton = nullptr;
  musicAddSelectedButton = nullptr;
  musicRemoveSelectedButton = nullptr;
  musicPlaylistButton = nullptr;
  musicClearPlaylistButton = nullptr;
  musicRandomButton = nullptr;
  musicPreviousButton = nullptr;
  musicSeekBackwardButton = nullptr;
  musicPlayPauseButton = nullptr;
  musicSeekForwardButton = nullptr;
  musicNextButton = nullptr;
  musicStopButton = nullptr;
  musicCloseButton = nullptr;
  musicSelectedButtonText = nullptr;
  musicAddSelectedButtonText = nullptr;
  musicRemoveSelectedButtonText = nullptr;
  musicPlaylistButtonText = nullptr;
  musicClearPlaylistButtonText = nullptr;
  musicRandomButtonText = nullptr;
  musicPreviousButtonText = nullptr;
  musicSeekBackwardButtonText = nullptr;
  musicPlayPauseButtonText = nullptr;
  musicSeekForwardButtonText = nullptr;
  musicNextButtonText = nullptr;
  musicStopButtonText = nullptr;
  musicCloseButtonText = nullptr;
  tasksScrollView = nullptr;
  tasksContent = nullptr;
  tasksText = nullptr;
  tasksRefreshButton = nullptr;
  tasksRefreshButtonText = nullptr;
  tasksCloseButton = nullptr;
  tasksCloseButtonText = nullptr;
  readyGaugeText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  readyPacemakerText = nullptr;
  readyPlayOptionsButton = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayExportJob_.reset();
  pendingSelectChartPath.reset();
  {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    pendingFindBmsSelectionHandoff.reset();
  }
  suppressPreviewForChartPath.reset();
  chartSelectionGeneration = 0;
  findBmsSelectionGenerationAtDownloadStart = 0;
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
  replayIrObservedRevisions.clear();
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  displayedLibraryTasksButtonText.clear();
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  publishedResultRecordDiagnostic.clear();
  playOptionsPanel = nullptr;
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}

#ifdef _WIN32
void MainMenuScene::FindFilesWin(const std::filesystem::path &path,
                                 std::vector<Diff> &diffs,
                                 const std::unordered_set<path_t> &oldFilesWs,
                                 std::vector<path_t> &directoriesToVisit,
                                 const std::stop_token &stop_token) {
  WIN32_FIND_DATAW findFileData;
  HANDLE hFind =
      FindFirstFileW((path.wstring() + L"\\*.*").c_str(), &findFileData);

  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (stop_token.stop_requested()) {
        break;
      }
      if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        path_t filename(findFileData.cFileName);

        if (asobmshow::bms_chart_file::isBmsChartFileName(filename)) {
          path_t dirPath;

          path_t fullPath = path.wstring() + L"\\" + filename;
          if (oldFilesWs.find(fullPath) == oldFilesWs.end()) {
            diffs.push_back({fullPath, Added});
          }
        }
      } else if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        path_t filename(findFileData.cFileName);

        if (filename != L"." && filename != L"..") {
          directoriesToVisit.push_back(path.wstring() + L"\\" + filename);
        }
      }
    } while (FindNextFileW(hFind, &findFileData) != 0);
    FindClose(hFind);
  }
}
#elif TARGET_OS_OSX || TARGET_OS_LINUX || TARGET_OS_ANDROID
void MainMenuScene::resolveDType(const std::filesystem::path &directoryPath,
                                 struct dirent *entry) {
  if (entry->d_type == DT_UNKNOWN) {
    std::filesystem::path fullPath = directoryPath / entry->d_name;
    struct stat statbuf;
    if (stat(fullPath.c_str(), &statbuf) == 0) {
      if (S_ISREG(statbuf.st_mode)) {
        entry->d_type = DT_REG;
      } else if (S_ISDIR(statbuf.st_mode)) {
        entry->d_type = DT_DIR;
      }
    }
  }
}
// TODO: Use platform-specific method for faster traversal
void MainMenuScene::FindFilesUnix(
    const std::filesystem::path &directoryPath, std::vector<Diff> &diffs,
    const std::unordered_set<path_t> &oldFiles,
    std::vector<std::filesystem::path> &directoriesToVisit,
    const std::stop_token &stop_token) {
  UniqueResource<DIR, closedir> dir(opendir(directoryPath.c_str()));
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir.get())) != nullptr) {
      if (stop_token.stop_requested()) {
        break;
      }
      resolveDType(directoryPath, entry);
      if (entry->d_type == DT_REG) {
        std::string filename = entry->d_name;
        if (asobmshow::bms_chart_file::isBmsChartFileName(filename)) {
          std::filesystem::path fullPath = directoryPath / filename;
          if (oldFiles.find(fspath_to_path_t(fullPath)) == oldFiles.end()) {
            diffs.push_back({fullPath, Added});
          }
        }
      } else if (entry->d_type == DT_DIR) {
        std::string filename = entry->d_name;
        if (filename != "." && filename != "..") {
          directoriesToVisit.push_back(directoryPath / filename);
        }
      } else {
        SDL_Log("Unknown file type: %s", entry->d_name);
      }
    }
  } else {
    SDL_Log("Failed to open directory: %s", directoryPath.c_str());
  }
}

#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
void MainMenuScene::FindFilesIOS(
    const std::filesystem::path &directoryPath, std::vector<Diff> &diffs,
    const std::unordered_set<path_t> &oldFilesWs,
    std::vector<std::filesystem::path> &directoriesToVisit,
    const std::stop_token &stop_token) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directoryPath, std::filesystem::directory_options::skip_permission_denied,
      error);
  if (error) {
    SDL_Log("Failed to open iOS directory: %s (%s)",
            fspath_to_utf8(directoryPath).c_str(), error.message().c_str());
    return;
  }

  for (const auto end = std::filesystem::directory_iterator(); iterator != end;
       iterator.increment(error)) {
    if (stop_token.stop_requested()) {
      break;
    }
    if (error) {
      SDL_Log("Failed while reading iOS directory: %s (%s)",
              fspath_to_utf8(directoryPath).c_str(), error.message().c_str());
      error.clear();
      continue;
    }

    const std::filesystem::directory_entry &entry = *iterator;
    std::error_code typeError;
    if (entry.is_regular_file(typeError)) {
      if (asobmshow::bms_chart_file::isBmsChartPath(entry.path())) {
        if (oldFilesWs.find(fspath_to_path_t(entry.path())) ==
            oldFilesWs.end()) {
          diffs.push_back({entry.path(), Added});
        }
      }
    } else if (!typeError && entry.is_directory(typeError)) {
      directoriesToVisit.push_back(entry.path());
    } else if (typeError) {
      SDL_Log("Failed to inspect iOS path: %s (%s)",
              fspath_to_utf8(entry.path()).c_str(),
              typeError.message().c_str());
    }
  }
}
#endif

void MainMenuScene::FindNewBmsFiles(
    std::vector<Diff> &diffs, const std::unordered_set<path_t> &oldFilesWs,
    const std::filesystem::path &path, const std::stop_token &stop_token) {
#ifdef _WIN32
  std::vector<path_t> directoriesToVisit;
  directoriesToVisit.push_back(path.wstring());
#else
  std::vector<std::filesystem::path> directoriesToVisit;
  directoriesToVisit.push_back(path);
#endif
  SDL_Log("Finding new bms files in %s", path_t_to_utf8(path).c_str());
  while (!directoriesToVisit.empty()) {
    if (stop_token.stop_requested()) {
      break;
    }
    std::filesystem::path currentDir = directoriesToVisit.back();
    directoriesToVisit.pop_back();

#ifdef _WIN32
    FindFilesWin(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#elif TARGET_OS_OSX || TARGET_OS_LINUX || TARGET_OS_ANDROID
    FindFilesUnix(currentDir, diffs, oldFilesWs, directoriesToVisit,
                  stop_token);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    FindFilesIOS(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#endif
  }
}
