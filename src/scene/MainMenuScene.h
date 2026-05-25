#pragma once
#include "../view/RecyclerView.h"
#include "Scene.h"
#include "../ChartDBHelper.h"
#include "../path.h"
#include "../view/ImageView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <vector>
#include "../targets.h"
#include "../audio/Jukebox.h"
#include "../video/VideoPlayer.h"
#include <atomic>
#include <stop_token>

class MainMenuScene : public Scene {
public:
  inline explicit MainMenuScene(ApplicationContext &context) : Scene(context) {}
  void init() override;

  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  sqlite3 *db;
  std::atomic_bool previewLoadCancelled = false;
  bool willStart = false;
  std::atomic<bms_parser::Chart *> selectedChart{nullptr};

  std::thread loadThread;
  std::jthread checkEntriesThread;
  std::jthread tableImportThread;
  std::atomic_bool tableImportRunning = false;
  struct LibraryFolderItem {
    enum class Type {
      AllSongs,
      DifficultyTable,
      DifficultyLevel,
      CoursesRoot,
      CourseGroup,
      Course
    };

    std::string key;
    std::string label;
    Type type = Type::AllSongs;
    int depth = 0;
    int count = -1;
    int tableId = 0;
    std::string tableLevel;
    int courseId = 0;
    int courseTableId = 0;
    std::string courseGroupName;
  };

  RecyclerView<bms_parser::ChartMeta> *recyclerView = nullptr;
  RecyclerView<LibraryFolderItem> *folderRecyclerView = nullptr;
  View *rootLayout = nullptr;
  ImageView *jacketView = nullptr;
  TextInputBox *searchBox = nullptr;
  TextInputBox *difficultyFilterBox = nullptr;
  TextInputBox *tableUrlInput = nullptr;
  TextView *tableImportStatus = nullptr;

  LibraryFolderItem activeFolder;
  std::string searchText;
  std::string difficultyText;
  std::string tableUrlText;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;

  void initView(ApplicationContext &context);
  void reloadFolderItems();
  void reloadChartList();
  void selectFolder(const LibraryFolderItem &item);
  void importDifficultyTableFromUrl();
  static void CheckEntries(const std::stop_token &stop_token,
                           ApplicationContext &context, MainMenuScene &scene);

  static void LoadCharts(ChartDBHelper &dbHelper, sqlite3 *db,
                         std::vector<path_t> &entries, MainMenuScene &scene,
                         const std::stop_token &stop_token);
  enum DiffType { Deleted, Added };
  struct Diff {
    std::filesystem::path path;
    DiffType type;
  };
#ifdef _WIN32
  static void FindFilesWin(const std::filesystem::path &path,
                           std::vector<Diff> &diffs,
                           const std::unordered_set<path_t> &oldFilesWs,
                           std::vector<path_t> &directoriesToVisit,
                           const std::stop_token &stop_token);
#elif TARGET_OS_OSX || TARGET_OS_LINUX
  static void
  FindFilesUnix(const std::filesystem::path &path, std::vector<Diff> &diffs,
                const std::unordered_set<path_t> &oldFilesWs,
                std::vector<std::filesystem::path> &directoriesToVisit,
                const std::stop_token &stop_token);
#elif TARGET_OS_IPHONE
  static void
  FindFilesIOS(const std::filesystem::path &path, std::vector<Diff> &diffs,
               const std::unordered_set<path_t> &oldFilesWs,
               std::vector<std::filesystem::path> &directoriesToVisit,
               const std::stop_token &stop_token);
#endif
  static void FindNewBmsFiles(std::vector<Diff> &diffs,
                              const std::unordered_set<path_t> &oldFilesWs,
                              const std::filesystem::path &path,
                              const std::stop_token &stop_token);
  static void resolveDType(const std::filesystem::path &directoryPath,
                           struct dirent *entry);
};
