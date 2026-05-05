#include "MainMenuScene.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include "../view/ChartListItemView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../video/transcode.h"
#include "../view/Button.h"
#include "play/GamePlayScene.h"
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
#include <iostream>

void MainMenuScene::init() {
  // Initialize the scene
  db = ChartDBHelper::GetInstance().Connect();
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
  checkEntriesThread =
      std::jthread(CheckEntries, std::ref(context), std::ref(*this));
}

void MainMenuScene::CheckEntries(const std::stop_token &stop_token,
                                 ApplicationContext &context,
                                 MainMenuScene &scene) {
  auto dbHelper = ChartDBHelper::GetInstance();
  auto db = dbHelper.Connect();
  dbHelper.CreateChartMetaTable(db);
  dbHelper.CreateEntriesTable(db);
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

  const Color kBackdropTint(10, 18, 30, 100);
  const Color kPanelFill(17, 27, 42, 100);
  const Color kSurfaceFill(11, 18, 30, 130);
  const Color kPrimaryButtonNormal(29, 73, 120, 216);
  const Color kPrimaryButtonHover(40, 96, 156, 228);
  const Color kPrimaryButtonPressed(58, 129, 204, 236);
  const Color kSecondaryButtonNormal(76, 49, 36, 208);
  const Color kSecondaryButtonHover(101, 65, 47, 220);
  const Color kSecondaryButtonPressed(133, 87, 63, 232);

  recyclerView = new RecyclerView<bms_parser::ChartMeta>(
      [](const bms_parser::ChartMeta &a, const bms_parser::ChartMeta &b) {
        return a.SHA256 == b.SHA256;
      });
  auto dbHelper = ChartDBHelper::GetInstance();

  recyclerView->onCreateView = [this](const bms_parser::ChartMeta &item) {
    return new ChartListItemView(0, 0, rendering::window_width, 100, item);
  };
  recyclerView->itemHeight = 100;
  recyclerView->onBind = [this](View *view, const bms_parser::ChartMeta &item,
                                int idx, bool isSelected) {
    auto *chartListItemView = dynamic_cast<ChartListItemView *>(view);
    chartListItemView->setMeta(item);
    if (isSelected) {
      chartListItemView->onSelected();
    } else {
      chartListItemView->onUnselected();
    }
  };

  auto jacketView = new ImageView(0, 0, 0, 0);
  recyclerView->onSelected = [this, &context, jacketView](
                                 const bms_parser::ChartMeta &item, int idx) {
    if (willStart)
      return;
    auto selectedView = recyclerView->getViewByIndex(idx);
    SDL_Log("Selected: %s; path: %s", item.Title.c_str(),
            path_t_to_utf8(item.Folder / item.BmsPath).c_str());
    if (selectedView) {
      selectedView->onSelected();
    }
    jacketView->setImage(item.Folder / item.StageFile);
    previewLoadCancelled = true;
    if (loadThread.joinable()) {
      SDL_Log("Joining preview thread");
      loadThread.join();
    }
    if (selectedChart) {
      delete selectedChart;
      selectedChart = nullptr;
    }
    loadThread = std::thread([this, item, &context]() {
      SDL_Log("Previewing %s", path_t_to_utf8(item.BmsPath).c_str());

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
        SDL_Log("Parsing %s", path_t_to_utf8(item.BmsPath).c_str());
        parser.Parse(item.BmsPath, &chart, false, false, previewLoadCancelled);
        SDL_Log("Parsed %s", path_t_to_utf8(item.BmsPath).c_str());
      } catch (std::exception &e) {
        delete chart;
        SDL_Log("Error parsing %s: %s", path_t_to_utf8(item.BmsPath).c_str(),
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
  recyclerView->onUnselected = [this](const bms_parser::ChartMeta &item,
                                      int idx) {
    auto unselectedView = recyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Row);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setGap(24);
  rootLayout->setPadding(Edge::All, 28);
  rootLayout->setBackgroundColor(kBackdropTint);
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
  librarySubtitle->setText("Search your library and preview charts before starting.");
  librarySubtitle->setColor({157, 177, 200, 255});
  left->addView(librarySubtitle);

  auto *searchBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 32);
  searchBox->setText("");
  searchBox->setHeight(56);
  searchBox->setBackgroundColor(kSurfaceFill);
  searchBox->setBorderColor(Color(88, 115, 149, 255));
  searchBox->setBorderWidth(2);
  searchBox->setVAlign(TextView::MIDDLE);
  searchBox->setColor({239, 244, 251, 255});
  searchBox->onSubmit([this, &context, searchBox](const std::string &text) {
    auto dbHelper = ChartDBHelper::GetInstance();
    if (text.empty()) {
      // TODO: handle deleted files
      std::vector<bms_parser::ChartMeta> chartMetas;
      dbHelper.SelectAllChartMeta(db, chartMetas);
      recyclerView->setItems(std::move(chartMetas));
    } else {
      std::vector<bms_parser::ChartMeta> chartMetas;
      dbHelper.SearchChartMeta(db, text, chartMetas);
      recyclerView->setItems(std::move(chartMetas));
    }
  });
  left->addView(searchBox);

  recyclerView->setFlex(1);
  recyclerView->setBackgroundColor(kSurfaceFill);
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
    auto selected = recyclerView->selectedIndex;
    SDL_Log("Selected: %d", selected);
    if (selected >= 0) {
      willStart = true;
      buttonText->setText("Loading...");

      defer(
          [this, &context, buttonText]() {
            SDL_Log("Starting game play scene");
            ImageView::dropAllCache();
            if (loadThread.joinable()) {
              loadThread.join();
            }
            context.jukebox.stop();
            context.sceneManager->changeScene(
                new GamePlayScene(context, selectedChart,
                                  {
                                      .startPosition = 0,
                                      .autoKeySound =
                                          !context.settings.inputKeysoundEnabled,
                                      .autoPlay = false,
                                  }),
                true);
            willStart = false;
            buttonText->setText("Start");
            return true;
          },
          0, true);
    }
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

  auto *settingsButton = new Button(0, 0, 220, 78);
  auto *settingsText = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
  settingsText->setText("Settings");
  settingsText->setAlign(TextView::CENTER);
  settingsText->setVAlign(TextView::MIDDLE);
  settingsButton->setContentView(settingsText);
  settingsButton->setBackgroundColors(kSecondaryButtonNormal,
                                      kSecondaryButtonHover,
                                      kSecondaryButtonPressed);
  settingsButton->setBorderColors(Color(174, 124, 91, 255),
                                  Color(207, 146, 105, 255),
                                  Color(232, 169, 122, 255));
  settingsButton->setStyledBorderWidth(2);
  settingsButton->setOnClickListener([this, &context]() {
    previewLoadCancelled = true;
    context.jukebox.stop();
    context.sceneManager->changeScene("Settings");
  });
  right->addView(settingsButton);
  rootLayout->addView(right);
  addView(rootLayout);
  std::vector<bms_parser::ChartMeta> chartMetas;
  dbHelper.SelectAllChartMeta(db, chartMetas);
  recyclerView->setItems(std::move(chartMetas));
  rootLayout->applyYogaLayout();
}

void MainMenuScene::update(float dt) {
  // Update the scene logic
  // std::cout << "Updating Main Menu Scene, dt: " << dt << std::endl;
}

void MainMenuScene::renderScene() {
  // Render the scene
  // SDL_Log("Rendering Main Menu Scene");
  rootLayout->setSize(rendering::window_width, rendering::window_height);
}

void MainMenuScene::cleanupScene() {
  // Cleanup resources when exiting the scene
  ChartDBHelper::GetInstance().Close(db);
  if (checkEntriesThread.joinable()) {
    SDL_Log("Joining checkEntriesThread");
    checkEntriesThread.request_stop();
    checkEntriesThread.join();
  }

  if (loadThread.joinable()) {
    SDL_Log("Joining loadThread");
    loadThread.join();
  }
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
  // set items
  chartMetas.clear();
  dbHelper.SelectAllChartMeta(db, chartMetas);

  scene.recyclerView->setItems(std::move(chartMetas));
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
