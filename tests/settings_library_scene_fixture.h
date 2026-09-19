// Production launch/delivery methods and the real task/URL policy; view,
// repository, and network-import doubles control the asynchronous boundaries.
#include "scene/DifficultyTableUrlCompletion.h"

namespace {
struct SDL_Color { unsigned char r, g, b, a; };
struct LibraryText {
  std::thread::id owner = std::this_thread::get_id();
  std::string text;
  SDL_Color color{};
  std::string getText() const { assert(owner == std::this_thread::get_id()); return text; }
  void setText(const std::string &value) { assert(owner == std::this_thread::get_id()); text = value; }
  void setEditingText(const std::string &value) { setText(value); }
  void setColor(SDL_Color value) { assert(owner == std::this_thread::get_id()); color = value; }
};
struct DifficultyTableImportProgress {
  int current, total;
  std::string tableName;
};
struct LibraryOperations {
  bool open = true;
  bool succeed = true;
  std::string error;
  std::string submittedUrl;
  int updatedId = 0, deletedId = 0, imports = 0;
  Gate *importGate = nullptr;
  std::thread::id applicationThread = std::this_thread::get_id();
  bool DeleteDifficultyTable(int id) { deletedId = id; return succeed; }
};
struct LibrarySession {
  LibraryOperations &operations;
  bool DeleteDifficultyTable(int id) { return operations.DeleteDifficultyTable(id); }
};
struct LibraryRepository {
  LibraryOperations &operations;
  std::optional<LibrarySession> OpenSession() {
    assert(operations.applicationThread != std::this_thread::get_id());
    if (!operations.open) { return std::nullopt; }
    return LibrarySession{operations};
  }
  std::uint64_t GetLibraryRevision() const { return 17; }
};
struct DifficultyTableImporter {
  bool ImportFromUrl(LibrarySession &session, const std::string &url,
                     std::string *error,
                     std::function<void(const DifficultyTableImportProgress &)> progress) {
    auto &operations = session.operations;
    ++operations.imports;
    operations.submittedUrl = url;
    progress({2, 3, "Downloading table"});
    if (operations.importGate) { operations.importGate->block(); }
    *error = operations.error;
    return operations.succeed;
  }
  bool UpdateFromSourceUrl(LibrarySession &session, int id, std::string *error) {
    session.operations.updatedId = id;
    *error = session.operations.error;
    return session.operations.succeed;
  }
};

class SettingsScene {
public:
  explicit SettingsScene(LibraryOperations &operations) : context{{operations}} {}
  ~SettingsScene() { libraryTask.stopAndWait(); }
  struct { LibraryRepository chartRepository; } context;
  SettingsLibraryTask libraryTask;
  LibraryText tableStatus, folderStatus, input;
  LibraryText *difficultyTableStatusText = &tableStatus;
  LibraryText *chartFolderStatusText = &folderStatus;
  LibraryText *tableUrlInput = &input;
  std::string difficultyTableStatusMessage, chartFolderStatusMessage, tableUrlText;
  SDL_Color difficultyTableStatusColor{}, chartFolderStatusColor{};
  int pendingDeleteDifficultyTableId = 0;
  std::string pendingDeleteChartEntryPath;
  bool difficultyTableImportModalVisible = false;
  bool difficultyTableImportFinished = false, difficultyTableImportSucceeded = false;
  int difficultyTableImportCurrent = 0, difficultyTableImportTotal = 0;
  std::string difficultyTableImportName, difficultyTableImportStatusMessage;
  std::uint64_t observedLibraryRevision = 0;
  int lastLayoutWidth = 100;
  int tableReloads = 0, folderReloads = 0, modalRefreshes = 0;
  void loadDifficultyTables() { ++tableReloads; }
  void loadChartEntries() { ++folderReloads; }
  void refreshDifficultyTableImportModal() { ++modalRefreshes; }
  void applyPendingDifficultyTableUpdates();
  void addDifficultyTableFromUrl();
  void updateDifficultyTableFromSource(int tableId);
  void deleteDifficultyTable(int tableId);
  void hideDifficultyTableImportModal();
};

#include "settings_library_scene_methods.inc"

void testSceneImportProgressAndSuccessfulUrlCompletion() {
  LibraryOperations operations;
  Gate gate;
  operations.importGate = &gate;
  SettingsScene scene(operations);
  scene.input.text = scene.tableUrlText = "submitted-url";
  scene.addDifficultyTableFromUrl();
  gate.wait();
  assert(scene.tableStatus.text == "Adding table...");
  assert(scene.difficultyTableImportName == "submitted-url");
  scene.addDifficultyTableFromUrl();
  scene.updateDifficultyTableFromSource(7);
  scene.hideDifficultyTableImportModal();
  assert(scene.difficultyTableImportModalVisible && operations.imports == 1);
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.difficultyTableImportCurrent == 2 && scene.difficultyTableImportTotal == 3);
  assert(scene.difficultyTableImportName == "Downloading table");
  assert(!scene.difficultyTableImportFinished && scene.tableReloads == 0);
  gate.release.set_value();
  waitIdle(scene.libraryTask);
  assert(scene.tableStatus.text == "Adding table...");
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.difficultyTableImportFinished && scene.difficultyTableImportSucceeded);
  assert(scene.tableStatus.text == "Table added." && scene.tableStatus.color.g > scene.tableStatus.color.r);
  assert(scene.tableUrlText.empty() && scene.input.text.empty());
  assert(scene.tableReloads == 1 && scene.folderReloads == 1);
  assert(scene.observedLibraryRevision == 17 && scene.lastLayoutWidth == -1);
  const auto refreshes = scene.modalRefreshes;
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.modalRefreshes == refreshes && scene.tableReloads == 1);
  scene.hideDifficultyTableImportModal();
  assert(!scene.difficultyTableImportModalVisible);
}

void testScenePreservesEditedUrlAndReportsDatabaseFailure() {
  LibraryOperations operations;
  SettingsScene scene(operations);
  scene.input.text = scene.tableUrlText = "submitted-url";
  scene.addDifficultyTableFromUrl();
  waitIdle(scene.libraryTask);
  scene.input.text = scene.tableUrlText = "new-url";
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.input.text == "new-url" && scene.tableUrlText == "new-url");

  operations.open = false;
  scene.addDifficultyTableFromUrl();
  waitIdle(scene.libraryTask);
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.tableStatus.text == "Could not open chart database.");
  assert(scene.difficultyTableImportFinished && !scene.difficultyTableImportSucceeded);
  assert(scene.tableStatus.color.r > scene.tableStatus.color.g);
  assert(scene.input.text == "new-url" && scene.tableReloads == 1);
}

void testSceneTableUpdateDeletionConfirmationAndAbsentViews() {
  LibraryOperations operations;
  SettingsScene scene(operations);
  scene.updateDifficultyTableFromSource(0);
  assert(!scene.libraryTask.running() && operations.updatedId == 0);
  scene.updateDifficultyTableFromSource(7);
  waitIdle(scene.libraryTask);
  scene.applyPendingDifficultyTableUpdates();
  assert(operations.updatedId == 7 && scene.tableStatus.text == "Table updated.");
  scene.deleteDifficultyTable(9);
  assert(scene.pendingDeleteDifficultyTableId == 9 && operations.deletedId == 0);
  scene.deleteDifficultyTable(9);
  waitIdle(scene.libraryTask);
  scene.applyPendingDifficultyTableUpdates();
  assert(operations.deletedId == 9 && scene.tableStatus.text == "Table deleted.");
  assert(scene.pendingDeleteDifficultyTableId == 0 && scene.tableReloads == 2);

  scene.difficultyTableStatusText = scene.chartFolderStatusText = scene.tableUrlInput = nullptr;
  operations.succeed = false;
  operations.error = "Source unavailable";
  scene.updateDifficultyTableFromSource(8);
  waitIdle(scene.libraryTask);
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.difficultyTableStatusMessage == "Source unavailable" && scene.tableReloads == 2);
  assert(scene.libraryTask.start([](const auto &, const Task::Publisher &updates) {
    updates.folderStatus("Remove failed.", false, true);
  }));
  waitIdle(scene.libraryTask);
  scene.applyPendingDifficultyTableUpdates();
  assert(scene.chartFolderStatusMessage == "Remove failed." && scene.tableReloads == 3);
}
} // namespace
