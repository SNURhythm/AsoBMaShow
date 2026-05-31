#include "MainMenuScene.h"
#include "MainMenuLibrary.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../ReplayDBHelper.h"
#include "../ReplayVideoExporter.h"
#include "../view/ChartListItemView.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../video/transcode.h"
#include "../view/Button.h"
#include "play/GamePlayScene.h"
#include "../view/ClearLampColors.h"
#include <memory>
#include <unordered_set>
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
#include "../MacNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#endif
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
#include <sstream>

namespace {
constexpr int kRootPadding = 28;

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
  insets.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  insets.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  insets.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
#endif
  return insets;
}

using main_menu_library::folderKeyForCourse;
using main_menu_library::folderKeyForCourseGroup;
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
  case GaugeType::Normal:
  default:
    return kClearTypeNormalClearRank;
  }
}

std::string gaugeButtonLabel(GaugeType gaugeType, bool autoShift) {
  if (autoShift) {
    return "GAS";
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
  default:
    return "NORMAL";
  }
}

std::string formatReplayGauge(float gauge) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << gauge << "%";
  return stream.str();
}

std::string replayGaugeLabel(GaugeType gaugeType, bool autoShift) {
  return autoShift ? "GAS" : gaugeButtonLabel(gaugeType, false);
}

void styleActionButton(Button *button, TextView *text, bool enabled,
                       const Color &normal, const Color &hover,
                       const Color &pressed, const Color &border) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  if (enabled) {
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(border, Color(border.r, border.g, border.b, 255),
                            Color(235, 246, 255, 255));
    text->setColor({242, 247, 255, 255});
  } else {
    button->setBackgroundColors(Color(25, 31, 39, 154),
                                Color(25, 31, 39, 154),
                                Color(25, 31, 39, 154));
    button->setBorderColors(Color(76, 88, 102, 120),
                            Color(76, 88, 102, 120),
                            Color(76, 88, 102, 120));
    text->setColor({129, 143, 160, 255});
  }
}

void styleOptionButton(Button *button, TextView *text, bool selected) {
  if (selected) {
    styleActionButton(button, text, true, Color(38, 97, 87, 232),
                      Color(50, 121, 109, 242), Color(65, 146, 130, 250),
                      Color(112, 212, 191, 255));
  } else {
    styleActionButton(button, text, true, Color(22, 34, 51, 220),
                      Color(32, 48, 70, 232), Color(44, 65, 94, 242),
                      Color(83, 109, 140, 220));
  }
}

class ModalRootView : public View {
public:
  using View::View;

private:
  bool handleEventsImpl(SDL_Event &event) override {
    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      return false;
    default:
      return true;
    }
  }
};

class ReplayListItemView : public View {
public:
  ReplayListItemView() {
    clearLamp = new View();
    textColumn = new View();
    titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    detailText = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);

    setFlexDirection(FlexDirection::Row)
        ->setAlignItems(YGAlignCenter)
        ->setPadding(Edge::All, 8)
        ->setGap(12);

    clearLamp->setWidth(5)->setHeight(52)->setFlexShrink(0);
    addView(clearLamp);

    textColumn->setFlexDirection(FlexDirection::Column)
        ->setJustifyContent(YGJustifyCenter)
        ->setFlexGrow(1)
        ->setFlexBasis(0)
        ->setMinWidth(0)
        ->setGap(4);
    titleText->setHeight(28);
    titleText->setOverflow(TextView::TextOverflow::Marquee);
    detailText->setHeight(22);
    detailText->setOverflow(TextView::TextOverflow::Hidden);
    textColumn->addView(titleText);
    textColumn->addView(detailText);
    addView(textColumn);

    scoreText->setWidth(140)->setHeight(32);
    scoreText->setAlign(TextView::TextAlign::RIGHT);
    scoreText->setVAlign(TextView::TextVAlign::MIDDLE);
    addView(scoreText);
    onUnselected();
  }

  void setSummary(const ReplaySummary &summary) {
    titleText->setText(summary.createdAt.empty()
                           ? "Replay #" + std::to_string(summary.id)
                           : summary.createdAt);
    detailText->setText(replayGaugeLabel(summary.initialGaugeType,
                                         summary.gaugeAutoShift) +
                        "  Gauge " + formatReplayGauge(summary.finalGauge) +
                        "  Events " + std::to_string(summary.eventCount));
    scoreText->setText(std::to_string(summary.finalScore));

    if (hasClearLampColor(summary.clearType)) {
      clearLamp->setBackgroundColor(clearLampColorForRank(summary.clearType));
    } else {
      clearLamp->clearBackgroundColor();
    }
  }

  void onSelected() override {
    setBackgroundColor(Color(32, 55, 82, 224));
    setBorderColor(Color(112, 177, 238, 255));
    setBorderWidth(1);
    titleText->setColor({255, 255, 255, 255});
    detailText->setColor({203, 220, 239, 255});
    scoreText->setColor({245, 250, 255, 255});
  }

  void onUnselected() override {
    setBackgroundColor(Color(7, 12, 20, 138));
    setBorderColor(Color(38, 52, 70, 160));
    setBorderWidth(1);
    titleText->setColor({235, 242, 250, 255});
    detailText->setColor({151, 171, 194, 255});
    scoreText->setColor({197, 216, 238, 255});
  }

private:
  View *clearLamp = nullptr;
  View *textColumn = nullptr;
  TextView *titleText = nullptr;
  TextView *detailText = nullptr;
  TextView *scoreText = nullptr;
};
} // namespace

void MainMenuScene::ChartListPageCache::reset(
    sqlite3 *database, const ChartMetaQuery &chartQuery, int count) {
  db = database;
  query = chartQuery;
  query.limit = 0;
  query.offset = 0;
  totalCount = std::max(0, count);
  clear();
}

void MainMenuScene::ChartListPageCache::clear() {
  pages.clear();
  pageOrder.clear();
}

const ChartMetaRecord &MainMenuScene::ChartListPageCache::get(
    int index) const {
  if (db == nullptr || index < 0 || index >= totalCount) {
    return fallbackRecord;
  }

  const int pageIndex = index / pageSize;
  auto pageIt = pages.find(pageIndex);
  if (pageIt == pages.end()) {
    ChartMetaQuery pageQuery = query;
    pageQuery.limit = pageSize;
    pageQuery.offset = pageIndex * pageSize;

    std::vector<ChartMetaRecord> records;
    records.reserve(pageSize);
    ChartDBHelper::GetInstance().QueryChartMeta(db, pageQuery, records);
    pageIt = pages.emplace(pageIndex, std::move(records)).first;
  }
  touchPage(pageIndex);

  const int localIndex = index - (pageIndex * pageSize);
  if (localIndex < 0 ||
      localIndex >= static_cast<int>(pageIt->second.size())) {
    return fallbackRecord;
  }
  return pageIt->second[localIndex];
}

void MainMenuScene::ChartListPageCache::touchPage(int pageIndex) const {
  pageOrder.erase(std::remove(pageOrder.begin(), pageOrder.end(), pageIndex),
                  pageOrder.end());
  pageOrder.push_back(pageIndex);

  while (static_cast<int>(pages.size()) > maxPages && !pageOrder.empty()) {
    const int victim = pageOrder.front();
    pageOrder.pop_front();
    if (victim == pageIndex && pages.size() == 1) {
      pageOrder.push_back(victim);
      break;
    }
    pages.erase(victim);
  }
}

void MainMenuScene::init() {
  // Initialize the scene
  db = ChartDBHelper::GetInstance().Connect();
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
  checkEntriesThread =
      std::jthread(CheckEntries, std::ref(context), std::ref(*this));
}

void MainMenuScene::onResume() { requestLibraryReload(true); }

void MainMenuScene::CheckEntries(const std::stop_token &stop_token,
                                 ApplicationContext &context,
                                 MainMenuScene &scene) {
  auto dbHelper = ChartDBHelper::GetInstance();
  auto db = dbHelper.Connect();
  dbHelper.CreateChartMetaTable(db);
  dbHelper.CreateEntriesTable(db);
  dbHelper.CreateDifficultyTableTables(db);
  const int importedTables = dbHelper.ImportDifficultyTablesFromDirectory(
      db, Utils::GetDocumentsPath("tables"));
  if (importedTables > 0 && !stop_token.stop_requested()) {
    scene.requestLibraryReload(true);
  }
  auto entries = dbHelper.SelectAllEntries(db);

  // Check for stop request before proceeding
  if (stop_token.stop_requested()) {
    dbHelper.Close(db);
    return;
  }

  // open folder select if no entries
  if (entries.empty()) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    auto path = Utils::GetDocumentsPath("BMS");
    std::filesystem::create_directories(path);
    entries.push_back(path);
    // for iOS, not writing the entry to db is intentional, since it may change
    // due to update with new code signing
#else
    char *folder_c = tinyfd_selectFolderDialog("Select Folder", nullptr);
    std::string folder;
    if (folder_c == nullptr) {
      std::cerr << "tinyfd_selectFolderDialog error: " << strerror(errno)
                << std::endl;
      // get input from stdin
      std::cout << "Failed to open folder select dialog.\n";

      while (folder.empty()) {
        // Check for stop request during user input
        if (stop_token.stop_requested()) {
          dbHelper.Close(db);
          return;
        }

        std::cout << "Enter bms folder path: ";
        std::cin >> folder;
        if (std::cin.eof() || std::cin.fail()) {
          break;
        }
        if (folder.empty()) {
          continue;
        }

        // replace ~ with home directory
        if (folder[0] == '~') {
          folder.replace(0, 1, getenv("HOME"));
        }
        std::ifstream test(folder);
        if (!test)
          folder = "";
      }

      if (folder.empty()) {
        dbHelper.Close(db);
        return;
      }
    } else {
      folder = folder_c;
    }
    std::filesystem::path path(folder);
    dbHelper.InsertEntry(db, path);
    entries = dbHelper.SelectAllEntries(db);
#endif
  }

  // Check for stop request before loading charts
  if (stop_token.stop_requested()) {
    dbHelper.Close(db);
    return;
  }

  LoadCharts(dbHelper, db, entries, scene, stop_token);
  dbHelper.Close(db);
}

void MainMenuScene::initView(ApplicationContext &context) {
  // Initialize the view
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  difficultyFilterBox = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayExportOptionsContent = nullptr;
  replayModalTitleText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
  pendingReplayExportResult.reset();
  replayExportInProgress = false;
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  gaugeSelectionButtons.clear();

  const Color kBackdropTint(10, 18, 30, 112);
  const Color kPanelFill(17, 27, 42, 196);
  const Color kSurfaceFill(11, 18, 30, 168);
  const Color kPrimaryButtonNormal(29, 73, 120, 216);
  const Color kPrimaryButtonHover(40, 96, 156, 228);
  const Color kPrimaryButtonPressed(58, 129, 204, 236);
  const Color kSecondaryButtonNormal(76, 49, 36, 208);
  const Color kSecondaryButtonHover(101, 65, 47, 220);
  const Color kSecondaryButtonPressed(133, 87, 63, 232);

  recyclerView = new RecyclerView<ChartMetaRecord>(
      [](const ChartMetaRecord &a, const ChartMetaRecord &b) {
        return a.meta.SHA256 == b.meta.SHA256 && a.meta.MD5 == b.meta.MD5 &&
               a.meta.BmsPath == b.meta.BmsPath &&
               a.difficultyTableLabels == b.difficultyTableLabels &&
               a.unavailable == b.unavailable;
      });
  folderRecyclerView = new RecyclerView<LibraryFolderItem>(
      [](const LibraryFolderItem &a, const LibraryFolderItem &b) {
        return a.key == b.key;
      });
  auto dbHelper = ChartDBHelper::GetInstance();
  dbHelper.CreateChartMetaTable(db);
  dbHelper.CreateDifficultyTableTables(db);

  recyclerView->onCreateView = [this](const ChartMetaRecord &item) {
    return new ChartListItemView(0, 0, rendering::window_width, 100, item);
  };
  recyclerView->itemHeight = 100;
  recyclerView->onBind = [this](View *view, const ChartMetaRecord &item,
                                int idx, bool isSelected) {
    auto *chartListItemView = dynamic_cast<ChartListItemView *>(view);
    chartListItemView->setMeta(item);
    chartListItemView->setClearRank(clearRankForChart(item));
    if (isSelected) {
      chartListItemView->onSelected();
    } else {
      chartListItemView->onUnselected();
    }
  };

  jacketView = new ImageView(0, 0, 0, 0);
  recyclerView->onSelected = [this, &context](const ChartMetaRecord &item,
                                              int idx) {
    if (willStart.load())
      return;
    const auto &meta = item.meta;
    auto selectedView = recyclerView->getViewByIndex(idx);
    if (selectedView) {
      selectedView->onSelected();
    }
    refreshReplayAvailability(&item);
    previewLoadCancelled = true;
    if (loadThread.joinable()) {
      SDL_Log("Joining preview thread");
      loadThread.join();
    }
    selectedChartMediaReady.store(false);
    delete selectedChart.exchange(nullptr);
    if (item.unavailable || meta.BmsPath.empty()) {
      jacketView->freeImage();
      context.jukebox.stop();
      return;
    }
    if (!meta.StageFile.empty()) {
      jacketView->setImage(meta.Folder / meta.StageFile);
    } else {
      jacketView->freeImage();
    }
    loadThread = std::thread([this, meta, &context]() {
      SDL_Log("Previewing %s", path_t_to_utf8(meta.BmsPath).c_str());

      previewLoadCancelled = false;
      // Debounce selection changes before doing expensive chart/media loading.
      for (int i = 0; i < 50; i++) {
        if (previewLoadCancelled) {
          return;
        }
        if (willStart.load())
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      context.jukebox.stop();
      bms_parser::Parser parser;
      bms_parser::Chart *parsedChart = nullptr;

      try {
        SDL_Log("Parsing %s", path_t_to_utf8(meta.BmsPath).c_str());
        parser.Parse(meta.BmsPath, &parsedChart, false, false,
                     previewLoadCancelled);
        SDL_Log("Parsed %s", path_t_to_utf8(meta.BmsPath).c_str());
      } catch (std::exception &e) {
        delete parsedChart;
        SDL_Log("Error parsing %s: %s", path_t_to_utf8(meta.BmsPath).c_str(),
                e.what());
        return;
      }
      std::unique_ptr<bms_parser::Chart> chart(parsedChart);
      if (chart == nullptr) {
        SDL_Log("Chart is null");
        return;
      }

      context.jukebox.loadChart(*chart, true, previewLoadCancelled);
      if (previewLoadCancelled) {
        return;
      }
      auto *loadedChart = chart.release();
      delete selectedChart.exchange(loadedChart);
      selectedChartMediaReady.store(true);
      if (!willStart.load()) {
        context.jukebox.play();
      }
    });
  };
  recyclerView->onUnselected = [this](const ChartMetaRecord &item, int idx) {
    auto unselectedView = recyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  folderRecyclerView->onCreateView = [](const LibraryFolderItem &item) {
    return new LibraryFolderItemView(0, 0, 260, 44);
  };
  folderRecyclerView->itemHeight = 44;
  folderRecyclerView->onBind = [](View *view, const LibraryFolderItem &item,
                                  int idx, bool isSelected) {
    auto *folderView = dynamic_cast<LibraryFolderItemView *>(view);
    if (folderView != nullptr) {
      folderView->setItem(item.label, item.depth, item.count, isSelected,
                          item.clearRank);
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
  rootLayout->setBackgroundColor(kBackdropTint);

  auto nav = new View();
  nav->setFlexDirection(FlexDirection::Column);
  nav->setAlignItems(YGAlignStretch);
  nav->setWidth(280);
  nav->setGap(12);
  nav->setPadding(Edge::All, 14);
  nav->setBackgroundColor(kPanelFill);
  nav->setBorderColor(Color(70, 95, 124, 255));
  nav->setBorderWidth(2);

  auto *navTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  navTitle->setText("Library");
  navTitle->setColor({243, 247, 255, 255});
  nav->addView(navTitle);

  folderRecyclerView->setFlex(1);
  folderRecyclerView->clearBackgroundColor();
  folderRecyclerView->setBorderColor(Color(63, 86, 113, 255));
  folderRecyclerView->setBorderWidth(2);
  nav->addView(folderRecyclerView);
  rootLayout->addView(nav);

  auto left = new View();
  left->setFlexDirection(FlexDirection::Column);
  left->setAlignItems(YGAlignStretch);
  left->setFlex(1);
  left->setGap(14);
  left->setPadding(Edge::All, 16);
  left->setBackgroundColor(kPanelFill);
  left->setBorderColor(Color(70, 95, 124, 255));
  left->setBorderWidth(2);

  auto *libraryTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 44);
  libraryTitle->setText("Song Select");
  libraryTitle->setColor({243, 247, 255, 255});
  left->addView(libraryTitle);

  auto *librarySubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  librarySubtitle->setText(
      "Search your library and preview charts before starting.");
  librarySubtitle->setColor({157, 177, 200, 255});
  left->addView(librarySubtitle);

  auto *filterRow = new View();
  filterRow->setFlexDirection(FlexDirection::Row);
  filterRow->setAlignItems(YGAlignStretch);
  filterRow->setGap(10);

  searchBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  searchBox->setText("");
  searchBox->setHeight(56);
  searchBox->setFlex(1);
  searchBox->setBackgroundColor(kSurfaceFill);
  searchBox->setBorderColor(Color(88, 115, 149, 255));
  searchBox->setBorderWidth(2);
  searchBox->setVAlign(TextView::MIDDLE);
  searchBox->setColor({239, 244, 251, 255});
  auto onSearchChanged = [this](const std::string &text) {
    searchText = text;
    reloadChartList();
  };
  searchBox->onTextChanged(onSearchChanged);
  searchBox->onSubmit(onSearchChanged);
  filterRow->addView(searchBox);

  difficultyFilterBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  difficultyFilterBox->setText("");
  difficultyFilterBox->setHeight(56);
  difficultyFilterBox->setWidth(180);
  difficultyFilterBox->setBackgroundColor(kSurfaceFill);
  difficultyFilterBox->setBorderColor(Color(88, 115, 149, 255));
  difficultyFilterBox->setBorderWidth(2);
  difficultyFilterBox->setVAlign(TextView::MIDDLE);
  difficultyFilterBox->setColor({239, 244, 251, 255});
  auto onDifficultyChanged = [this](const std::string &text) {
    difficultyText = text;
    reloadChartList();
  };
  difficultyFilterBox->onTextChanged(onDifficultyChanged);
  difficultyFilterBox->onSubmit(onDifficultyChanged);
  filterRow->addView(difficultyFilterBox);

  auto *filterLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  filterLabel->setText("Search / Difficulty");
  filterLabel->setColor({157, 177, 200, 255});
  left->addView(filterLabel);
  left->addView(filterRow);

  recyclerView->setFlex(1);
  recyclerView->clearBackgroundColor();
  recyclerView->setBorderColor(Color(63, 86, 113, 255));
  recyclerView->setBorderWidth(2);
  left->addView(recyclerView);
  rootLayout->addView(left);

  auto right = new View();
  right->setFlexDirection(FlexDirection::Column);
  right->setAlignItems(YGAlignCenter);
  right->setPadding(Edge::All, 24);
  right->setGap(18);
  right->setWidth(300);
  right->setBackgroundColor(kPanelFill);
  right->setBorderColor(Color(70, 95, 124, 255));
  right->setBorderWidth(2);

  auto *rightTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 38);
  rightTitle->setText("Ready");
  rightTitle->setColor({243, 247, 255, 255});
  rightTitle->setAlign(TextView::CENTER);
  right->addView(rightTitle);

  auto *rightSubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  rightSubtitle->setText("Preview, tweak, and start.");
  rightSubtitle->setColor({157, 177, 200, 255});
  rightSubtitle->setAlign(TextView::CENTER);
  right->addView(rightSubtitle);

  auto *gaugePanel = new View();
  gaugePanel->setFlexDirection(FlexDirection::Column);
  gaugePanel->setAlignItems(YGAlignStretch);
  gaugePanel->setWidth(252);
  gaugePanel->setGap(7);

  auto *gaugeLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  gaugeLabel->setText("Gauge");
  gaugeLabel->setColor({157, 177, 200, 255});
  gaugePanel->addView(gaugeLabel);

  auto makeGaugeRow = []() {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row);
    row->setAlignItems(YGAlignStretch);
    row->setGap(6);
    return row;
  };

  auto *gaugeRowA = makeGaugeRow();
  auto *gaugeRowB = makeGaugeRow();
  auto makeGaugeButton = [this](GaugeType type, bool autoShift) {
    auto *button = new Button();
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    text->setText(gaugeButtonLabel(type, autoShift));
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    button->setContentView(text);
    button->setHeight(40);
    button->setFlex(1);
    button->setStyledBorderWidth(2);
    button->setOnClickListener(
        [this, type, autoShift]() { setGaugeSelection(type, autoShift); });
    gaugeSelectionButtons.push_back({
        .button = button,
        .text = text,
        .type = type,
        .autoShift = autoShift,
    });
    return button;
  };

  gaugeRowA->addView(makeGaugeButton(GaugeType::AssistedEasy, false));
  gaugeRowA->addView(makeGaugeButton(GaugeType::Easy, false));
  gaugeRowA->addView(makeGaugeButton(GaugeType::Normal, false));
  gaugeRowB->addView(makeGaugeButton(GaugeType::Hard, false));
  gaugeRowB->addView(makeGaugeButton(GaugeType::ExHard, false));
  gaugeRowB->addView(makeGaugeButton(GaugeType::ExHard, true));
  gaugePanel->addView(gaugeRowA);
  gaugePanel->addView(gaugeRowB);
  right->addView(gaugePanel);
  refreshGaugeSelectionButtons();

  auto startButton = new Button(0, 0, 200, 100);
  auto buttonText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  buttonText->setText("Start");
  buttonText->setAlign(TextView::CENTER);
  buttonText->setVAlign(TextView::MIDDLE);
  startButton->setContentView(buttonText);
  startButton->setBackgroundColors(kPrimaryButtonNormal, kPrimaryButtonHover,
                                   kPrimaryButtonPressed);
  startButton->setBorderColors(Color(105, 162, 222, 255),
                               Color(133, 190, 244, 255),
                               Color(162, 212, 255, 255));
  startButton->setStyledBorderWidth(2);
  startButton->setOnClickListener([this, &context, buttonText]() {
    if (willStart.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    if (selected >= 0 && selected < recyclerView->size()) {
      const auto &selectedMeta = recyclerView->get(selected);
      if (selectedMeta.unavailable || selectedMeta.meta.BmsPath.empty()) {
        return;
      }
      willStart.store(true);
      buttonText->setText("Loading...");

      defer(
          [this, &context, buttonText]() {
            ImageView::dropAllCache();
            if (loadThread.joinable()) {
              loadThread.join();
            }
            auto *chart = loadedSelectedChart();
            if (chart == nullptr) {
              willStart.store(false);
              buttonText->setText("Start");
              return true;
            }
            context.jukebox.stop();
            context.sceneManager->changeScene(
                new GamePlayScene(
                    context, chart,
                    {
                        .startPosition = 0,
                        .autoKeySound = !context.settings.inputKeysoundEnabled,
                        .autoPlay = false,
                        .gaugeType = selectedGaugeType,
                        .gaugeAutoShift = selectedGaugeAutoShift,
                    }),
                true);
            willStart.store(false);
            buttonText->setText("Start");
            return true;
          },
          0, true);
    }
  });
  replayButtonSlot = new View();
  replayButtonSlot->setWidth(220)->setHeight(0);
  replayButtonSlot->setVisible(false);
  replayButtonSlot->setAlignItems(YGAlignStretch);

  replayButton = new Button(0, 0, 220, 64);
  replayButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
  replayButtonText->setText("Replay");
  replayButtonText->setAlign(TextView::CENTER);
  replayButtonText->setVAlign(TextView::MIDDLE);
  replayButton->setContentView(replayButtonText);
  replayButton->setBackgroundColors(Color(25, 58, 65, 216),
                                    Color(35, 82, 92, 228),
                                    Color(48, 111, 124, 236));
  replayButton->setBorderColors(Color(91, 174, 184, 255),
                                Color(116, 204, 214, 255),
                                Color(145, 232, 241, 255));
  replayButton->setStyledBorderWidth(2);
  replayButton->setOnClickListener([this]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    if (selected < 0 || selected >= recyclerView->size()) {
      return;
    }
    const auto &selectedMeta = recyclerView->get(selected);
    if (selectedMeta.unavailable || selectedMeta.meta.BmsPath.empty()) {
      return;
    }

    showReplayListModal(selectedMeta);
  });
  replayButtonSlot->addView(replayButton);

  replayStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  replayStatusText->setText("");
  replayStatusText->setColor({157, 177, 200, 255});
  replayStatusText->setAlign(TextView::CENTER);
  replayStatusText->setHeight(24);

  auto *jacketCard = new View();
  jacketCard->setWidth(220);
  jacketCard->setHeight(220);
  jacketCard->setAlignItems(YGAlignCenter);
  jacketCard->setJustifyContent(YGJustifyCenter);
  jacketCard->setBackgroundColor(kSurfaceFill);
  jacketCard->setBorderColor(Color(88, 115, 149, 255));
  jacketCard->setBorderWidth(2);
  jacketView->setWidth(220)->setHeight(220);
  jacketCard->addView(jacketView);
  startButton->setHeight(100);
  right->addView(jacketCard);
  right->addView(startButton);
  right->addView(replayButtonSlot);
  right->addView(replayStatusText);

  auto *settingsButton = new Button(0, 0, 220, 78);
  auto *settingsText = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
  settingsText->setText("Settings");
  settingsText->setAlign(TextView::CENTER);
  settingsText->setVAlign(TextView::MIDDLE);
  settingsButton->setContentView(settingsText);
  settingsButton->setBackgroundColors(
      kSecondaryButtonNormal, kSecondaryButtonHover, kSecondaryButtonPressed);
  settingsButton->setBorderColors(Color(174, 124, 91, 255),
                                  Color(207, 146, 105, 255),
                                  Color(232, 169, 122, 255));
  settingsButton->setStyledBorderWidth(2);
  settingsButton->setOnClickListener([this, &context]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    previewLoadCancelled = true;
    context.jukebox.stop();
    context.sceneManager->changeScene("Settings");
  });
  right->addView(settingsButton);

  rootLayout->addView(right);
  buildReplayModal();
  addView(rootLayout);
  reloadScoreClearRanks();
  reloadFolderItems();
  reloadChartList();
  rootLayout->applyYogaLayout();
}

void MainMenuScene::reloadFolderItems() {
  if (folderRecyclerView == nullptr) {
    return;
  }

  auto dbHelper = ChartDBHelper::GetInstance();
  std::vector<LibraryFolderItem> folders;

  int allSongCount = 0;
  allSongCount = dbHelper.CountAllChartMeta(db);

  folders.push_back({
      .key = "all",
      .label = "All songs",
      .type = LibraryFolderItem::Type::AllSongs,
      .depth = 0,
      .count = allSongCount,
  });

  const auto tables = dbHelper.SelectDifficultyTables(db);
  for (const auto &table : tables) {
    folders.push_back({
        .key = folderKeyForTable(table.id),
        .label = table.name,
        .type = LibraryFolderItem::Type::DifficultyTable,
        .depth = 0,
        .count = table.chartCount,
        .tableId = table.id,
    });

    const auto levels = dbHelper.SelectDifficultyLevels(db, table.id);
    for (const auto &level : levels) {
      folders.push_back({
          .key = folderKeyForLevel(level.tableId, level.level),
          .label = level.tableSymbol + level.level,
          .type = LibraryFolderItem::Type::DifficultyLevel,
          .depth = 1,
          .count = level.chartCount,
          .tableId = level.tableId,
          .tableLevel = level.level,
      });
    }
  }

  const auto courseGroups = dbHelper.SelectDifficultyCourseGroups(db);
  if (!courseGroups.empty()) {
    int coursesCount = 0;
    for (const auto &group : courseGroups) {
      coursesCount += group.matchedChartCount;
    }
    folders.push_back({
        .key = "courses",
        .label = "Courses",
        .type = LibraryFolderItem::Type::CoursesRoot,
        .depth = 0,
        .count = coursesCount,
    });

    for (const auto &group : courseGroups) {
      const std::string label = group.groupName.empty()
                                    ? group.tableName + " Courses"
                                    : group.groupName;
      folders.push_back({
          .key = folderKeyForCourseGroup(group.tableId, group.groupName),
          .label = label,
          .type = LibraryFolderItem::Type::CourseGroup,
          .depth = 1,
          .count = group.matchedChartCount,
          .courseTableId = group.tableId,
          .courseGroupName = group.groupName,
      });

      const auto courses =
          dbHelper.SelectDifficultyCourses(db, group.tableId, group.groupName);
      for (const auto &course : courses) {
        const std::string courseLabel =
            course.level.empty() ? course.name : course.level;
        folders.push_back({
            .key = folderKeyForCourse(course.id),
            .label = courseLabel,
            .type = LibraryFolderItem::Type::Course,
            .depth = 2,
            .count = course.matchedChartCount,
            .courseId = course.id,
            .courseTableId = course.tableId,
            .courseGroupName = course.groupName,
        });
      }
    }
  }

  for (auto &folder : folders) {
    folder.clearRank = clearRankForFolder(folder.key);
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

  folderRecyclerView->setItems(std::move(folders));
  folderRecyclerView->selectedIndex = activeIndex;
  auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
  if (selectedView != nullptr) {
    selectedView->onSelected();
  }
}

void MainMenuScene::reloadChartList() {
  if (recyclerView == nullptr) {
    return;
  }

  ChartMetaQuery query;
  query.keyword = searchText;
  query.difficultyText = difficultyText;

  switch (activeFolder.type) {
  case LibraryFolderItem::Type::DifficultyTable:
    query.tableId = activeFolder.tableId;
    break;
  case LibraryFolderItem::Type::DifficultyLevel:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    break;
  case LibraryFolderItem::Type::CoursesRoot:
    query.coursesOnly = true;
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

  const int count = ChartDBHelper::GetInstance().CountChartMeta(db, query);
  chartListCache.reset(db, query, count);
  refreshReplayAvailability(nullptr);
  recyclerView->setItemProvider(
      count, [this](int index) -> const ChartMetaRecord & {
        return chartListCache.get(index);
      });
}

void MainMenuScene::reloadScoreClearRanks() {
  scoreClearRanks = ScoreDBHelper::GetInstance().LoadBestClearRanks();
  scoreClearRanksRevision = ScoreDBHelper::GetInstance().GetRevision();
  folderClearRanks =
      main_menu_library::LoadFolderClearRanks(db, scoreClearRanks);
}

void MainMenuScene::refreshScoreClearRanksIfNeeded() {
  const std::uint64_t revision = ScoreDBHelper::GetInstance().GetRevision();
  if (scoreClearRanksRevision == 0 || revision == scoreClearRanksRevision) {
    return;
  }

  requestLibraryReload(true);
}

int MainMenuScene::clearRankForChart(const ChartMetaRecord &record) const {
  return scoreClearRanks.bestRankFor(record.meta);
}

int MainMenuScene::clearRankForFolder(const std::string &key) const {
  const auto it = folderClearRanks.find(key);
  return it == folderClearRanks.end() ? kNoClearTypeRank : it->second;
}

void MainMenuScene::requestLibraryReload(bool includeFolders) {
  if (includeFolders) {
    folderItemsReloadRequested = true;
  }
  chartListReloadRequested = true;
}

void MainMenuScene::applyPendingUiUpdates() {
  const bool shouldReloadFolders = folderItemsReloadRequested.exchange(false);
  const bool shouldReloadCharts = chartListReloadRequested.exchange(false);
  if (shouldReloadFolders) {
    reloadScoreClearRanks();
    reloadFolderItems();
  }
  if (shouldReloadFolders || shouldReloadCharts) {
    reloadChartList();
  }
}

void MainMenuScene::selectFolder(const LibraryFolderItem &item) {
  activeFolder = item;
  reloadChartList();
}

void MainMenuScene::setGaugeSelection(GaugeType gaugeType, bool autoShift) {
  selectedGaugeType = gaugeType;
  selectedGaugeAutoShift = autoShift;
  refreshGaugeSelectionButtons();
}

void MainMenuScene::refreshGaugeSelectionButtons() {
  for (auto &item : gaugeSelectionButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    const bool selected =
        item.autoShift == selectedGaugeAutoShift &&
        (item.autoShift || item.type == selectedGaugeType);
    if (selected) {
      const Color accent =
          item.autoShift
              ? Color(255, 205, 37, 242)
              : clearLampColorForRank(clearRankForGaugeType(item.type));
      item.button->setBackgroundColors(accent, accent,
                                       Color(accent.r, accent.g, accent.b, 255));
      item.button->setBorderColors(Color(255, 255, 255, 220),
                                   Color(255, 255, 255, 240),
                                   Color(255, 255, 255, 255));
      const bool darkText = item.autoShift || item.type == GaugeType::Hard ||
                            item.type == GaugeType::ExHard;
      item.text->setColor(darkText ? SDL_Color{14, 20, 28, 255}
                                   : SDL_Color{255, 255, 255, 255});
    } else {
      item.button->setBackgroundColors(Color(20, 31, 47, 214),
                                       Color(31, 48, 72, 226),
                                       Color(44, 67, 99, 236));
      item.button->setBorderColors(Color(76, 101, 130, 190),
                                   Color(106, 134, 166, 220),
                                   Color(134, 164, 198, 240));
      item.text->setColor({216, 227, 241, 255});
    }
  }
}

void MainMenuScene::refreshReplayAvailability(const ChartMetaRecord *record) {
  replaySummaries.clear();
  selectedReplayIndex = -1;
  if (record == nullptr || record->unavailable || record->meta.BmsPath.empty()) {
    setReplayButtonVisible(false);
    return;
  }

  replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record->meta);
  setReplayButtonVisible(!replaySummaries.empty());
}

void MainMenuScene::setReplayButtonVisible(bool visible) {
  if (replayButtonSlot == nullptr) {
    return;
  }

  replayButtonSlot->setVisible(visible);
  replayButtonSlot->setHeight(visible ? 64.0f : 0.0f);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::buildReplayModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;
  constexpr float kModalContentHeight = 418.0f;

  replayModalRoot =
      new ModalRootView(0, 0, rendering::window_width, rendering::window_height);
  replayModalRoot->setPositionType(YGPositionTypeAbsolute);
  replayModalRoot->setPosition(Edge::Left, 0);
  replayModalRoot->setPosition(Edge::Top, 0);
  replayModalRoot->setZIndex(1000);
  replayModalRoot->setVisible(false);
  replayModalRoot->setFlexDirection(FlexDirection::Column);
  replayModalRoot->setAlignItems(YGAlignCenter);
  replayModalRoot->setJustifyContent(YGJustifyCenter);
  replayModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(620)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, 22)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  replayModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  replayModalTitleText->setText("Replay");
  replayModalTitleText->setColor({245, 249, 255, 255});
  replayModalTitleText->setHeight(42);
  panel->addView(replayModalTitleText);

  replayModalContentFrame = new View();
  replayModalContentFrame->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setFlexShrink(0);
  panel->addView(replayModalContentFrame);

  replayListContent = new View();
  replayListContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(10);
  replayListView = new RecyclerView<ReplaySummary>(
      [](const ReplaySummary &a, const ReplaySummary &b) {
        return a.id == b.id;
      });
  replayListView->itemHeight = 74;
  replayListView->onCreateView = [](const ReplaySummary &) {
    return new ReplayListItemView();
  };
  replayListView->onBind = [](View *view, const ReplaySummary &item, int,
                              bool isSelected) {
    auto *itemView = dynamic_cast<ReplayListItemView *>(view);
    if (itemView == nullptr) {
      return;
    }
    itemView->setSummary(item);
    if (isSelected) {
      itemView->onSelected();
    } else {
      itemView->onUnselected();
    }
  };
  replayListView->onSelected = [this](const ReplaySummary &, int idx) {
    if (selectedReplayIndex >= 0 && replayListView != nullptr &&
        selectedReplayIndex < replayListView->size()) {
      if (auto *oldView = replayListView->getViewByIndex(selectedReplayIndex)) {
        oldView->onUnselected();
      }
    }
    selectedReplayIndex = idx;
    if (auto *newView = replayListView->getViewByIndex(idx)) {
      newView->onSelected();
    }
    refreshReplayModalActions();
  };
  replayListView->onUnselected = [this](const ReplaySummary &, int idx) {
    if (auto *view = replayListView->getViewByIndex(idx)) {
      view->onUnselected();
    }
  };
  replayListView->setFlex(1);
  replayListView->clearBackgroundColor();
  replayListView->setBorderColor(Color(55, 76, 102, 255));
  replayListView->setBorderWidth(2);
  replayListContent->addView(replayListView);
  replayModalContentFrame->addView(replayListContent);

  replayExportOptionsContent = new View();
  replayExportOptionsContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(18);
  replayExportOptionsContent->setVisible(false);

  auto makeOptionLabel = [](const std::string &text) {
    auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    label->setText(text);
    label->setColor({173, 193, 216, 255});
    label->setHeight(28);
    return label;
  };
  auto makeOptionRow = []() {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row);
    row->setAlignItems(YGAlignStretch);
    row->setGap(12);
    row->setHeight(58);
    return row;
  };
  auto makeModalButton = [](const std::string &label, int fontSize,
                            TextView **textOut) {
    auto *button = new Button(0, 0, 160, 58);
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", fontSize);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    button->setContentView(text);
    button->setStyledBorderWidth(2);
    if (textOut != nullptr) {
      *textOut = text;
    }
    return button;
  };

  replayExportOptionsContent->addView(makeOptionLabel("Frame Rate"));
  auto *fpsRow = makeOptionRow();
  replayFps60Button = makeModalButton("60 fps", 20, &replayFps60ButtonText);
  replayFps120Button = makeModalButton("120 fps", 20, &replayFps120ButtonText);
  replayFps60Button->setFlex(1);
  replayFps120Button->setFlex(1);
  replayFps60Button->setOnClickListener([this]() {
    selectedExportFps = 60;
    refreshReplayExportOptionButtons();
  });
  replayFps120Button->setOnClickListener([this]() {
    selectedExportFps = 120;
    refreshReplayExportOptionButtons();
  });
  fpsRow->addView(replayFps60Button);
  fpsRow->addView(replayFps120Button);
  replayExportOptionsContent->addView(fpsRow);

  replayExportOptionsContent->addView(makeOptionLabel("Resolution"));
  auto *resolutionRow = makeOptionRow();
  replayResolution1080Button =
      makeModalButton("1080p", 20, &replayResolution1080ButtonText);
  replayResolutionFullButton =
      makeModalButton("Full Resolution", 20, &replayResolutionFullButtonText);
  replayResolution1080Button->setFlex(1);
  replayResolutionFullButton->setFlex(1);
  replayResolution1080Button->setOnClickListener([this]() {
    selectedExportFullResolution = false;
    refreshReplayExportOptionButtons();
  });
  replayResolutionFullButton->setOnClickListener([this]() {
    selectedExportFullResolution = true;
    refreshReplayExportOptionButtons();
  });
  resolutionRow->addView(replayResolution1080Button);
  resolutionRow->addView(replayResolutionFullButton);
  replayExportOptionsContent->addView(resolutionRow);
  replayModalContentFrame->addView(replayExportOptionsContent);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  replayModalCloseButton =
      makeModalButton("Close", 20, &replayModalCloseButtonText);
  replayWatchButton = makeModalButton("Watch", 20, &replayWatchButtonText);
  replayModalExportButton =
      makeModalButton("Export", 20, &replayModalExportButtonText);
  replayModalCloseButton->setOnClickListener([this]() {
    if (replayExportOptionsContent != nullptr &&
        replayExportOptionsContent->getVisible()) {
      replayModalTitleText->setText("Replay");
      replayExportOptionsContent->setVisible(false);
      replayListContent->setVisible(true);
      const int previousSelection = selectedReplayIndex;
      const float previousScrollOffset = replayListView->scrollOffset;
      replayListView->setItems(replaySummaries);
      replayListView->scrollOffset = previousScrollOffset;
      selectedReplayIndex =
          previousSelection >= 0 &&
                  previousSelection < static_cast<int>(replaySummaries.size())
              ? previousSelection
              : -1;
      replayListView->selectedIndex = selectedReplayIndex;
      if (selectedReplayIndex >= 0) {
        if (auto *selectedView =
                replayListView->getViewByIndex(selectedReplayIndex)) {
          selectedView->onSelected();
        }
      }
      refreshReplayModalActions();
      return;
    }
    hideReplayModal();
  });
  replayWatchButton->setOnClickListener([this]() {
    if (selectedReplayIndex < 0 ||
        selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
      return;
    }
    startReplayPlayback(replayModalChart,
                        replaySummaries[selectedReplayIndex].id);
  });
  replayModalExportButton->setOnClickListener([this]() {
    if (selectedReplayIndex < 0 ||
        selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
      return;
    }
    if (replayExportOptionsContent != nullptr &&
        replayExportOptionsContent->getVisible()) {
      ReplayVideoExportOptions options;
      options.fps = selectedExportFps;
      if (!selectedExportFullResolution) {
        options.height = 1080;
      }
      startReplayVideoExport(replayModalChart,
                             replaySummaries[selectedReplayIndex].id, options);
      return;
    }
    showReplayExportOptions();
  });
  footer->addView(replayModalCloseButton);
  footer->addView(replayWatchButton);
  footer->addView(replayModalExportButton);
  panel->addView(footer);

  replayModalRoot->addView(panel);
  rootLayout->addView(replayModalRoot);
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
}

void MainMenuScene::showReplayListModal(const ChartMetaRecord &record) {
  if (replayModalRoot == nullptr || replayListView == nullptr) {
    return;
  }

  replayModalChart = record;
  replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record.meta);
  setReplayButtonVisible(!replaySummaries.empty());
  if (replaySummaries.empty()) {
    return;
  }

  selectedReplayIndex = -1;
  replayModalTitleText->setText("Replay");
  replayListContent->setVisible(true);
  replayExportOptionsContent->setVisible(false);
  replayListView->setItems(replaySummaries);
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayout();
}

void MainMenuScene::showReplayExportOptions() {
  if (replayModalRoot == nullptr || selectedReplayIndex < 0 ||
      selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
    return;
  }

  replayModalTitleText->setText("Export Options");
  replayListContent->setVisible(false);
  replayExportOptionsContent->setVisible(true);
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayout();
}

void MainMenuScene::hideReplayModal() {
  if (replayModalRoot == nullptr) {
    return;
  }
  replayModalRoot->setVisible(false);
  selectedReplayIndex = -1;
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Watch");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText("Export");
  }
}

void MainMenuScene::refreshReplayModalActions() {
  const bool hasSelection =
      selectedReplayIndex >= 0 &&
      selectedReplayIndex < static_cast<int>(replaySummaries.size());
  const bool optionsMode =
      replayExportOptionsContent != nullptr && replayExportOptionsContent->getVisible();

  if (replayModalCloseButtonText != nullptr) {
    replayModalCloseButtonText->setText(optionsMode ? "Back" : "Close");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText(optionsMode ? "Start Export"
                                                     : "Export");
  }

  if (replayWatchButton != nullptr) {
    replayWatchButton->setVisible(!optionsMode);
    replayWatchButton->setWidth(optionsMode ? 0.0f : 160.0f);
  }

  styleActionButton(replayModalCloseButton, replayModalCloseButtonText, true,
                    Color(47, 54, 70, 220), Color(62, 72, 92, 232),
                    Color(78, 90, 114, 242), Color(118, 137, 160, 220));
  styleActionButton(replayWatchButton, replayWatchButtonText,
                    hasSelection && !optionsMode, Color(29, 73, 120, 224),
                    Color(40, 96, 156, 236), Color(58, 129, 204, 246),
                    Color(105, 162, 222, 255));
  styleActionButton(replayModalExportButton, replayModalExportButtonText,
                    hasSelection, Color(47, 54, 88, 224),
                    Color(65, 75, 119, 236), Color(82, 94, 148, 246),
                    Color(126, 141, 219, 255));

  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayout();
  }
}

void MainMenuScene::refreshReplayExportOptionButtons() {
  styleOptionButton(replayFps60Button, replayFps60ButtonText,
                    selectedExportFps == 60);
  styleOptionButton(replayFps120Button, replayFps120ButtonText,
                    selectedExportFps == 120);
  styleOptionButton(replayResolution1080Button, replayResolution1080ButtonText,
                    !selectedExportFullResolution);
  styleOptionButton(replayResolutionFullButton, replayResolutionFullButtonText,
                    selectedExportFullResolution);
}

void MainMenuScene::startReplayPlayback(const ChartMetaRecord &record,
                                        int replayId) {
  if (willStart.load()) {
    return;
  }

  willStart.store(true);
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Loading...");
  }

  defer(
      [this, record, replayId]() {
        if (loadThread.joinable()) {
          loadThread.join();
        }

        auto replay = ReplayDBHelper::GetInstance().LoadReplay(replayId,
                                                               record.meta);
        if (!replay.has_value()) {
          willStart.store(false);
          if (replayWatchButtonText != nullptr) {
            replayWatchButtonText->setText("Watch");
          }
          refreshReplayAvailability(&record);
          return true;
        }

        auto *chart = loadedSelectedChart();
        if (chart == nullptr) {
          willStart.store(false);
          if (replayWatchButtonText != nullptr) {
            replayWatchButtonText->setText("Watch");
          }
          return true;
        }

        auto replayData =
            std::make_shared<ReplayData>(std::move(replay.value()));
        context.jukebox.stop();
        hideReplayModal();
        context.sceneManager->changeScene(
            new GamePlayScene(
                context, chart,
                {
                    .startPosition = 0,
                    .autoKeySound = false,
                    .autoPlay = false,
                    .gaugeType = replayData->initialGaugeType,
                    .gaugeAutoShift = replayData->gaugeAutoShift,
                    .replayData = replayData,
                }),
            true);
        willStart.store(false);
        return true;
      },
      0, true);
}

bms_parser::Chart *MainMenuScene::loadedSelectedChart() const {
  if (!selectedChartMediaReady.load()) {
    return nullptr;
  }
  return selectedChart.load();
}

void MainMenuScene::startReplayVideoExport(const ChartMetaRecord &record,
                                           int replayId,
                                           ReplayVideoExportOptions options) {
  if (replayExportInProgress.exchange(true)) {
    return;
  }
  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }

  willStart.store(true);
  previewLoadCancelled = true;
  selectedChartMediaReady.store(false);
  hideReplayModal();
  if (replayStatusText != nullptr) {
    replayStatusText->setText("Exporting...");
  }

  replayExportThread = std::jthread(
      [this, record, replayId, options](const std::stop_token &stopToken) {
        auto complete = [this](const ReplayVideoExportResult &result) {
          std::lock_guard<std::mutex> lock(replayExportResultMutex);
          pendingReplayExportResult = PendingReplayExportResult{
              .success = result.success,
              .outputPath = result.outputPath,
              .message = result.message,
          };
        };

        try {
          if (loadThread.joinable()) {
            loadThread.join();
          }
          context.jukebox.stop();
          if (stopToken.stop_requested()) {
            complete({.success = false, .message = "Replay export cancelled"});
            return;
          }

          auto replay =
              ReplayDBHelper::GetInstance().LoadReplay(replayId, record.meta);
          if (!replay.has_value()) {
            complete({.success = false, .message = "No Replay"});
            return;
          }

          bms_parser::Parser parser;
          bms_parser::Chart *parsedChart = nullptr;
          std::atomic_bool parseCancelled = false;
          parser.Parse(record.meta.BmsPath, &parsedChart, false, false,
                       parseCancelled);
          std::unique_ptr<bms_parser::Chart> chart(parsedChart);
          if (chart == nullptr) {
            complete({.success = false, .message = "No Chart"});
            return;
          }
          if (stopToken.stop_requested()) {
            complete({.success = false, .message = "Replay export cancelled"});
            return;
          }

          complete(ReplayVideoExporter::Export(context, chart.get(),
                                               replay.value(), options));
        } catch (const std::exception &e) {
          complete({.success = false, .message = e.what()});
        } catch (...) {
          complete({.success = false,
                    .message = "Unexpected replay export failure"});
        }
      });
}

void MainMenuScene::applyReplayVideoExportResult() {
  std::optional<PendingReplayExportResult> result;
  {
    std::lock_guard<std::mutex> lock(replayExportResultMutex);
    if (!pendingReplayExportResult.has_value()) {
      return;
    }
    result = std::move(pendingReplayExportResult);
    pendingReplayExportResult.reset();
  }

  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }
  replayExportInProgress = false;
  willStart.store(false);

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

  if (replayStatusText != nullptr) {
    if (result->success) {
      replayStatusText->setText(result->message == "Saved to Photos"
                                    ? "Saved"
                                    : "Exported");
    } else if (result->message == "No Replay") {
      replayStatusText->setText("No Replay");
    } else if (result->message == "No Chart") {
      replayStatusText->setText("No Chart");
    } else {
      replayStatusText->setText("Export Failed");
    }
  }

  if (result->success) {
    SDL_Log("Replay video exported: %s (%s)",
            result->outputPath.string().c_str(), result->message.c_str());
  } else {
    SDL_Log("Replay video export failed: %s (%s)", result->message.c_str(),
            result->outputPath.string().c_str());
  }

  defer(
      [this]() {
        if (!replayExportInProgress.load() && replayStatusText != nullptr) {
          replayStatusText->setText("");
        }
        return false;
      },
      result->success ? 1800 : 1400, true);
}

void MainMenuScene::update(float dt) {
  // Update the scene logic
  // std::cout << "Updating Main Menu Scene, dt: " << dt << std::endl;
  refreshScoreClearRanksIfNeeded();
  applyPendingUiUpdates();
  applyReplayVideoExportResult();
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
  if (replayModalRoot != nullptr) {
    replayModalRoot->setSize(rendering::window_width, rendering::window_height);
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
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::cleanupScene() {
  // Cleanup resources when exiting the scene
  previewLoadCancelled = true;
  if (replayExportThread.joinable()) {
    SDL_Log("Joining replayExportThread");
    replayExportThread.request_stop();
    replayExportThread.join();
  }
  if (checkEntriesThread.joinable()) {
    SDL_Log("Joining checkEntriesThread");
    checkEntriesThread.request_stop();
    checkEntriesThread.join();
  }

  if (loadThread.joinable()) {
    SDL_Log("Joining loadThread");
    loadThread.join();
  }
  delete selectedChart.exchange(nullptr);
  ChartDBHelper::GetInstance().Close(db);
  db = nullptr;
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  difficultyFilterBox = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayExportOptionsContent = nullptr;
  replayModalTitleText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
  pendingReplayExportResult.reset();
  replayExportInProgress = false;
  selectedChartMediaReady.store(false);
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  gaugeSelectionButtons.clear();
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}

void MainMenuScene::LoadCharts(ChartDBHelper &dbHelper, sqlite3 *db,
                               std::vector<path_t> &entries,
                               MainMenuScene &scene,
                               const std::stop_token &stop_token) {
  std::vector<bms_parser::ChartMeta> chartMetas;
  dbHelper.SelectAllChartMeta(db, chartMetas);
  // sort by title
  std::sort(chartMetas.begin(), chartMetas.end(),
            [](bms_parser::ChartMeta &a, bms_parser::ChartMeta &b) {
              return a.Title < b.Title;
            });
  std::vector<Diff> diffs;
  SDL_Log("Finding new bms files");
  std::unordered_set<path_t> oldFilesWs;

  for (auto &chartMeta : chartMetas) {
    if (stop_token.stop_requested()) {
      break;
    }
    // check file exists
    if (!std::filesystem::exists(chartMeta.BmsPath)) {
      diffs.push_back({chartMeta.BmsPath, DiffType::Deleted});
      continue;
    }
    oldFilesWs.insert(fspath_to_path_t(chartMeta.BmsPath));
    // std::cout << "Old file: " << chartMeta.BmsPath << std::endl;
    // std::cout << "Folder: " << chartMeta.Folder << std::endl;
  }
  for (auto &entry : entries) {
    if (stop_token.stop_requested()) {
      break;
    }
    FindNewBmsFiles(diffs, oldFilesWs, entry, stop_token);
  }

  SDL_Log("Found %zu new bms files", diffs.size());
  if (diffs.empty())
    return;
  std::atomic_bool is_committing(false);
  std::atomic_int success_count(0);
  dbHelper.BeginTransaction(db);
  parallel_for(diffs.size(), [&](int start, int end) {
    for (int i = start; i < end; i++) {
      if (stop_token.stop_requested()) {
        break;
      }
      auto &diff = diffs[i];
      if (diff.type == Added) {
        bms_parser::Parser parser;
        bms_parser::Chart *chart;
        std::atomic_bool cancel(false);
        bms_parser::ChartMeta chartMeta;
        // try {
        parser.Parse(diffs[i].path, &chart, false, true, cancel);
        // } catch (std::exception &e) {
        //   delete chart;
        //   SDL_Log("Error parsing %s:",
        //   path_t_to_utf8(diffs[i].path).c_str()); std::cerr << "Error parsing
        //   " << diffs[i].path << ": " << e.what()
        //             << std::endl;
        //   continue;
        // }

        if (chart == nullptr)
          continue;
        ++success_count;
        if (success_count % 1000 == 0 && !is_committing) {
          is_committing = true;
          dbHelper.CommitTransaction(db);
          dbHelper.BeginTransaction(db);
          is_committing = false;
        }
        dbHelper.InsertChartMeta(db, chart->Meta);
        delete chart;
      } else {
        dbHelper.DeleteChartMeta(db, diff.path);
      }
    }
  });
  dbHelper.CommitTransaction(db);
  SDL_Log("Inserted %d new charts", success_count.load());
  scene.requestLibraryReload(true);
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

        if (filename.size() > 4) {
          path_t ext = filename.substr(filename.size() - 4);
          std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
          if (ext == L".bms" || ext == L".bme" || ext == L".bml") {
            path_t dirPath;

            path_t fullPath = path.wstring() + L"\\" + filename;
            if (oldFilesWs.find(fullPath) == oldFilesWs.end()) {
              diffs.push_back({fullPath, Added});
            }
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
#elif TARGET_OS_OSX || TARGET_OS_LINUX
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
  DIR *dir = opendir(directoryPath.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (stop_token.stop_requested()) {
        closedir(dir);
        break;
      }
      resolveDType(directoryPath, entry);
      if (entry->d_type == DT_REG) {
        std::string filename = entry->d_name;
        if (filename.size() > 4) {
          std::string ext = filename.substr(filename.size() - 4);
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".bms" || ext == ".bme" || ext == ".bml") {
            std::filesystem::path fullPath = directoryPath / filename;
            if (oldFiles.find(fspath_to_path_t(fullPath)) == oldFiles.end()) {
              diffs.push_back({fullPath, Added});
            }
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
    closedir(dir);
  } else {
    SDL_Log("Failed to open directory: %s", directoryPath.c_str());
  }
}

#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
void MainMenuScene::FindFilesIOS(
    const std::filesystem::path &path, std::vector<Diff> &diffs,
    const std::unordered_set<path_t> &oldFilesWs,
    std::vector<std::filesystem::path> &directoriesToVisit,
    const std::stop_token &stop_token) {
  // use iosnatives
  /* TODO: read from BMS/
   * (or modify ChartDBHelper::ToAbsolutePath/ToRelativePath to allow document
   * root path)
   */
  auto files = ListDocumentFilesRecursively();
  SDL_Log("Found %d files", files.size());
  for (auto &file : files) {
    if (stop_token.stop_requested()) {
      break;
    }
    // SDL_Log("File: %s", file.c_str());
    if (file.size() > 4) {
      std::string ext = file.substr(file.size() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".bms" || ext == ".bme" || ext == ".bml") {
        path_t fullPath = GetIOSDocumentsPath() + "/" + file;
        if (oldFilesWs.find(fullPath) == oldFilesWs.end()) {
          diffs.push_back({fullPath, Added});
        }
      }
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
#elif TARGET_OS_OSX || TARGET_OS_LINUX
    FindFilesUnix(currentDir, diffs, oldFilesWs, directoriesToVisit,
                  stop_token);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    FindFilesIOS(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#endif
  }
}
