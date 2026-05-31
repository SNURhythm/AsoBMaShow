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
#include <iostream>

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
  replayExportButtonText = nullptr;
  pendingReplayExportResult.reset();
  replayExportInProgress = false;
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
    if (willStart)
      return;
    const auto &meta = item.meta;
    auto selectedView = recyclerView->getViewByIndex(idx);
    SDL_Log("Selected: %s; path: %s", meta.Title.c_str(),
            path_t_to_utf8(meta.Folder / meta.BmsPath).c_str());
    if (selectedView) {
      selectedView->onSelected();
    }
    previewLoadCancelled = true;
    if (loadThread.joinable()) {
      SDL_Log("Joining preview thread");
      loadThread.join();
    }
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
      // dumb implementation of debounce
      for (int i = 0; i < 50; i++) {
        if (previewLoadCancelled) {
          return;
        }
        if (willStart)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      context.jukebox.stop();
      bms_parser::Parser parser;
      bms_parser::Chart *chart = nullptr;

      try {
        SDL_Log("Parsing %s", path_t_to_utf8(meta.BmsPath).c_str());
        parser.Parse(meta.BmsPath, &chart, false, false, previewLoadCancelled);
        SDL_Log("Parsed %s", path_t_to_utf8(meta.BmsPath).c_str());
      } catch (std::exception &e) {
        delete chart;
        SDL_Log("Error parsing %s: %s", path_t_to_utf8(meta.BmsPath).c_str(),
                e.what());
        return;
      }
      if (chart == nullptr) {
        SDL_Log("Chart is null");
        return;
      }
      selectedChart = chart;

      context.jukebox.loadChart(*chart, true, previewLoadCancelled);
      if (previewLoadCancelled) {
        return;
      }
      if (!willStart) {
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
    SDL_Log("Start button clicked");
    if (willStart) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    SDL_Log("Selected: %d", selected);
    if (selected >= 0 && selected < recyclerView->size()) {
      const auto &selectedMeta = recyclerView->get(selected);
      if (selectedMeta.unavailable || selectedMeta.meta.BmsPath.empty()) {
        return;
      }
      willStart = true;
      buttonText->setText("Loading...");

      defer(
          [this, &context, buttonText]() {
            SDL_Log("Starting game play scene");
            ImageView::dropAllCache();
            if (loadThread.joinable()) {
              loadThread.join();
            }
            if (selectedChart.load() == nullptr) {
              willStart = false;
              buttonText->setText("Start");
              return true;
            }
            context.jukebox.stop();
            context.sceneManager->changeScene(
                new GamePlayScene(
                    context, selectedChart,
                    {
                        .startPosition = 0,
                        .autoKeySound = !context.settings.inputKeysoundEnabled,
                        .autoPlay = false,
                        .gaugeType = selectedGaugeType,
                        .gaugeAutoShift = selectedGaugeAutoShift,
                    }),
                true);
            willStart = false;
            buttonText->setText("Start");
            return true;
          },
          0, true);
    }
  });
  auto replayButton = new Button(0, 0, 220, 64);
  auto replayButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
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
  replayButton->setOnClickListener([this, &context, replayButtonText]() {
    SDL_Log("Replay button clicked");
    if (willStart) {
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

    willStart = true;
    replayButtonText->setText("Loading...");
    defer(
        [this, &context, replayButtonText, selectedMeta]() {
          if (loadThread.joinable()) {
            loadThread.join();
          }

          auto replay =
              ReplayDBHelper::GetInstance().LoadLatestReplay(selectedMeta.meta);
          if (!replay.has_value()) {
            willStart = false;
            replayButtonText->setText("No Replay");
            defer(
                [replayButtonText]() {
                  replayButtonText->setText("Replay");
                  return false;
                },
                1200, true);
            return true;
          }

          if (selectedChart.load() == nullptr) {
            willStart = false;
            replayButtonText->setText("Replay");
            return true;
          }

          auto replayData =
              std::make_shared<ReplayData>(std::move(replay.value()));
          context.jukebox.stop();
          context.sceneManager->changeScene(
              new GamePlayScene(
                  context, selectedChart,
                  {
                      .startPosition = 0,
                      .autoKeySound = false,
                      .autoPlay = false,
                      .gaugeType = replayData->initialGaugeType,
                      .gaugeAutoShift = replayData->gaugeAutoShift,
                      .replayData = replayData,
                  }),
              true);
          willStart = false;
          replayButtonText->setText("Replay");
          return true;
        },
        0, true);
  });

  auto exportButton = new Button(0, 0, 220, 64);
  auto exportButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  replayExportButtonText = exportButtonText;
  exportButtonText->setText("Export MP4");
  exportButtonText->setAlign(TextView::CENTER);
  exportButtonText->setVAlign(TextView::MIDDLE);
  exportButton->setContentView(exportButtonText);
  exportButton->setBackgroundColors(Color(47, 54, 88, 216),
                                    Color(65, 75, 119, 228),
                                    Color(82, 94, 148, 236));
  exportButton->setBorderColors(Color(126, 141, 219, 255),
                                Color(151, 165, 239, 255),
                                Color(180, 191, 255, 255));
  exportButton->setStyledBorderWidth(2);
  exportButton->setOnClickListener([this]() {
    SDL_Log("Replay export button clicked");
    if (willStart || replayExportInProgress.load()) {
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

    startReplayVideoExport(selectedMeta);
  });
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
  right->addView(replayButton);
  right->addView(exportButton);

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
    if (willStart || replayExportInProgress.load()) {
      return;
    }
    previewLoadCancelled = true;
    context.jukebox.stop();
    context.sceneManager->changeScene("Settings");
  });
  right->addView(settingsButton);

  rootLayout->addView(right);
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

void MainMenuScene::startReplayVideoExport(const ChartMetaRecord &record) {
  if (replayExportInProgress.exchange(true)) {
    return;
  }
  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }

  willStart = true;
  previewLoadCancelled = true;
  if (replayExportButtonText != nullptr) {
    replayExportButtonText->setText("Exporting...");
  }

  replayExportThread = std::jthread(
      [this, record](const std::stop_token &stopToken) {
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
              ReplayDBHelper::GetInstance().LoadLatestReplay(record.meta);
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
                                               replay.value()));
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
  willStart = false;

  if (replayExportButtonText != nullptr) {
    if (result->success) {
      replayExportButtonText->setText(result->message == "Saved to Photos"
                                          ? "Saved"
                                          : "Exported");
    } else if (result->message == "No Replay") {
      replayExportButtonText->setText("No Replay");
    } else if (result->message == "No Chart") {
      replayExportButtonText->setText("No Chart");
    } else {
      replayExportButtonText->setText("Export Failed");
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
        if (!replayExportInProgress.load() &&
            replayExportButtonText != nullptr) {
          replayExportButtonText->setText("Export MP4");
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
  replayExportButtonText = nullptr;
  pendingReplayExportResult.reset();
  replayExportInProgress = false;
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
