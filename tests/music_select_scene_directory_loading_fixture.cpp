#include "REPOSITORY_ROOT/src/music_select/MusicSelectDirectoryRequest.h"
#include "REPOSITORY_ROOT/src/music_select/MusicSelectExternalActions.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void SDL_Log(const char *, ...) {}
namespace audio::diag { void SelectAudioLog(const char *) {} }

namespace long_note_mode {
int valueFromId(const std::string &) { return 0; }
}

namespace skin {
enum class MusicSelectSkinActionKind { Event };
struct MusicSelectSkinAction {
  MusicSelectSkinActionKind kind;
  struct { int value; } selector;
  std::array<int, 2> arguments;
};
struct MusicSelectSkinPointerResult {
  std::optional<int> focusedStringWriter;
  bool closeDirectory = false;
  std::optional<std::size_t> selectIndex;
};
}

MusicSelectBar folder(std::string id, std::string title) {
  return {.id = {std::move(id)},
          .kind = skin::MusicSelectBarKind::Folder,
          .title = std::move(title),
          .childrenLoaded = false};
}

MusicSelectProjection rootProjection() {
  MusicSelectProjection projection;
  projection.bars = {folder("folder:a", "A"), folder("folder:b", "B")};
  projection.root = {{"folder:a"}, {"folder:b"}};
  return projection;
}

MusicSelectBar song() {
  MusicSelectBar result{.id = {"song:loaded"},
                         .kind = skin::MusicSelectBarKind::Song,
                         .title = "Loaded song",
                         .chart = ChartMetaRecord{}};
  result.chart->meta.BmsPath = "/songs/loaded.bms";
  return result;
}

struct MusicSelectRepositoryMetadata {};

struct FixtureRepository {
  int normalLoads = 0;
  int autoplayLoads = 0;
  bool emptyAutoplay = false;
  bool failNormal = false;
};

struct FailedRows : MusicSelectRowProvider {
  MusicSelectBar unavailable{.id = {"unavailable:0"}};
  std::string error = "transient middle page failure";
  std::shared_ptr<MusicSelectRowProvider> clone() const override {
    return std::make_shared<FailedRows>(*this);
  }
  std::size_t size() const noexcept override { return 1; }
  const MusicSelectBar &at(std::size_t) const override { return unavailable; }
  std::optional<std::size_t> indexOf(const MusicSelectBarId &) const override {
    return std::nullopt;
  }
  std::pair<std::string, std::string> configure(
      const std::string &, const std::string &, const std::string &) override {
    return {"ALL", "ALL"};
  }
  const std::string &diagnostic() const noexcept { return error; }
};

class MusicSelectDirectoryLoader {
public:
  struct Content {
    std::vector<MusicSelectBar> children;
    std::shared_ptr<MusicSelectRowProvider> provider;
  };
  struct Result {
    MusicSelectBarId id;
    std::uint64_t generation = 0;
    Content content;
    std::string error;
  };
  using Processor = std::function<Content(std::stop_token)>;

  std::uint64_t request(MusicSelectBarId id, Processor process) {
    ++requests;
    pending_ = Pending{std::move(id), ++generation_, std::move(process)};
    results_.clear();
    return generation_;
  }

  void cancel() {
    ++generation_;
    pending_.reset();
    results_.clear();
  }

  std::vector<Result> takeResults() { return std::exchange(results_, {}); }

  void completePending() {
    expect(pending_.has_value(), "fixture requires a pending asynchronous request");
    const auto pending = std::exchange(pending_, std::nullopt);
    try {
      results_.push_back({pending->id, pending->generation, pending->process({}), {}});
    } catch (const std::exception &error) {
      results_.push_back({pending->id, pending->generation, {}, error.what()});
    }
  }

  int requests = 0;

private:
  struct Pending {
    MusicSelectBarId id;
    std::uint64_t generation;
    Processor process;
  };
  std::uint64_t generation_ = 0;
  std::optional<Pending> pending_;
  std::vector<Result> results_;
};

MusicSelectDirectoryLoader::Content loadMusicSelectPhysicalDirectory(
    FixtureRepository &repository, const MusicSelectRepositoryMetadata &,
    const MusicSelectBar &, int, int, const std::filesystem::path &,
    const MusicSelectBarManagerConfig &, int, std::stop_token) {
  ++repository.normalLoads;
  if (repository.failNormal) throw std::runtime_error("retry failed");
  return {.children = {song()}};
}

MusicSelectDirectoryLoader::Content loadMusicSelectPhysicalDirectoryAutoplay(
    FixtureRepository &repository, const MusicSelectBar &, int, std::stop_token) {
  ++repository.autoplayLoads;
  if (repository.emptyAutoplay) return {};
  return {.children = {song()}};
}

struct SystemSound {
  void playFolderOpen() {}
  void playFolderClose() {}
};

struct FixturePreview {
  void reset() {}
  void silence() {}
  void resumeDefaultBgm() {}
};

struct MusicSelectScene {
  struct Context {
    std::atomic_bool appInBackground = false;
    FixtureRepository chartRepository;
    struct Replay {
      std::filesystem::path GetResolvedProfileRoot() const { return "/profile"; }
    } replayRepository;
    struct Settings {
      std::string skinModeFilterName = "ALL";
      std::string skinDifficultyFilterName = "ALL";
      std::string skinSortId = "TITLE";
      std::string selectedLnMode = "LN";
    } settings;
  } context;

  MusicSelectBarManager bars_{rootProjection()};
  bool chartSession_ = true;
  bool launching_ = false;
  bool failed_ = false;
  bool sceneActive_ = true;
  FixturePreview previewController_;
  FixturePreview *previewAudio_ = nullptr;
  int scoreCache_ = 0;
  int clearRankCache_ = 0;
  std::uint64_t libraryRevision_ = 20;
  std::uint64_t scoreRevision_ = 30;
  std::shared_ptr<MusicSelectRepositoryMetadata> repositoryMetadata_ =
      std::make_shared<MusicSelectRepositoryMetadata>();
  std::unique_ptr<MusicSelectDirectoryLoader> directoryLoader_;
  std::optional<MusicSelectDirectoryRequest> directoryRequest_;
  std::vector<MusicSelectBarId> restoreDirectories_;
  std::vector<MusicSelectBar> restoreDirectoryBars_;
  std::optional<MusicSelectBarId> restoreSelection_;
  SystemSound *systemSound_ = nullptr;
  std::string directoryStatusMessage_;
  std::vector<MusicSelectBar> launchedPlaylists;

  void requestDirectoryLoad(const MusicSelectBar &, bool autoplay = false);
  void openSelected();
  void applyDirectoryLoads();
  void cancelDirectoryLoad();
  void continueDirectoryRestore();
  void onApplicationBackgroundChanged(bool background);
  bool openDirectory(const MusicSelectBar &);
  void launchSelectedDirectoryAutoplay();
  void launchDirectoryAutoplay(const MusicSelectBar &);
  void applySkinPointerResult(const skin::MusicSelectSkinPointerResult &,
                              MusicSelectPointerOrigin);

  void showDirectoryStatus(std::string message) {
    directoryStatusMessage_ = std::move(message);
  }
  void executeEvent(const skin::MusicSelectSkinAction &) {}
  void syncResolvedFilters() {}
  void configureSoundServices() {}
  void beginSkinTextEditing(int) {}
  void launchSelected(bool autoplay = false, bool practice = false);
  bool openSameFolder(bool) { return false; }
  bool loadDirectoryChildren(const MusicSelectBar &directory) {
    return bars_.installChildren(directory.id, {song()});
  }
  void launchCourse(const MusicSelectBar &playlist, bool autoplay) {
    expect(autoplay, "directory completion must request autoplay");
    launchedPlaylists.push_back(playlist);
  }
  void closeDirectory();
  void selectedBarMoved() {
    const auto snapshot = bars_.readView();
    SELECTED_MOVE_GUARD
  }
  void reloadLibrary(bool preserveDirectory) {
    RELOAD_CAPTURE
    bars_.refresh(rootProjection());
    RELOAD_RESTORE
  }
};

SCENE_METHODS

void testFailedPageRecovery() {
  MusicSelectScene scene;
  const auto directory = scene.bars_.readView().rowAt(0);
  auto failed = std::make_shared<FailedRows>();
  expect(scene.bars_.installRowProvider(directory.id, failed) &&
             scene.bars_.open(directory.id), "fixture opens failed provider");
  const auto retained = scene.bars_.songListFrame();
  for (int frame = 0; frame < 100; ++frame) scene.applyDirectoryLoads();
  expect(scene.directoryStatusMessage_.find("retry") != std::string::npos &&
             !scene.directoryLoader_, "page diagnosis is visible without automatic retry storms");
  scene.applySkinPointerResult({.selectIndex = 0}, MusicSelectPointerOrigin::Mouse);
  expect(scene.directoryRequest_ && scene.directoryLoader_->requests == 1,
         "activating unavailable row requests fresh asynchronous snapshot");
  scene.openSelected();
  expect(scene.directoryLoader_->requests == 1, "pending explicit retries coalesce");
  scene.context.chartRepository.failNormal = true;
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  for (int frame = 0; frame < 100; ++frame) scene.applyDirectoryLoads();
  expect(scene.directoryLoader_->requests == 1 && !scene.directoryStatusMessage_.empty(),
         "repeated storage failure waits for another explicit retry");
  scene.openSelected();
  scene.closeDirectory();
  expect(!scene.directoryRequest_ && scene.bars_.readView().rowProvider == failed,
         "back cancels reload without replacing the retained failed view");
  scene.openSelected();
  ++scene.libraryRevision_;
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  expect(scene.bars_.readView().rowProvider == failed,
         "stale-revision retry completion cannot replace visible rows");
  scene.context.chartRepository.failNormal = false;
  scene.openSelected();
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  const auto recovered = scene.bars_.readView();
  expect(recovered.directory == std::vector<MusicSelectBarId>{directory.id} &&
             recovered.rowAt(0).chart && scene.directoryStatusMessage_.empty(),
         "retry publishes fresh rows without duplicating the directory stack");
  expect(!retained.at(0).exists && retained.rowProvider == failed,
         "retained frame keeps original failed provider after replacement");
  expect(scene.bars_.installRowProvider(directory.id, failed), "restore failed provider");
  scene.closeDirectory();
  const auto root = scene.bars_.readView();
  expect(!root.rowAt(root.selectedIndex).childrenLoaded,
         "leaving failed provider invalidates cached directory content");
  expect(scene.directoryStatusMessage_.empty(), "leaving clears stale failure diagnosis");
  scene.openSelected();
  expect(scene.directoryRequest_.has_value(), "reopening requests a fresh snapshot");
}



void testAutoplayCompletion() {
  MusicSelectScene scene;
  scene.launchSelectedDirectoryAutoplay();
  expect(scene.directoryLoader_ && scene.directoryLoader_->requests == 1,
         "unloaded directory autoplay must begin one asynchronous request");
  expect(scene.launchedPlaylists.empty(), "autoplay must wait for directory results");
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  expect(scene.launchedPlaylists.size() == 1 &&
             scene.directoryLoader_->requests == 1 && !scene.directoryRequest_,
         "completed directory autoplay must launch once instead of requesting another load");
  const auto &playlist = scene.launchedPlaylists.front();
  expect(playlist.courseCharts.size() == 1 &&
             playlist.courseCharts.front().meta.BmsPath == "/songs/loaded.bms",
         "autoplay must launch the installed directory's actual chart records");
  scene.applyDirectoryLoads();
  expect(scene.launchedPlaylists.size() == 1,
         "a completed autoplay result must not launch twice");
}

void testPointerTargetsClickedFolder() {
  MusicSelectScene scene;
  scene.applySkinPointerResult({.selectIndex = 1}, MusicSelectPointerOrigin::Mouse);
  expect(scene.directoryRequest_.has_value() &&
             scene.directoryRequest_->directory.id.value == "folder:b" &&
             scene.bars_.readView().selectedIndex == 1,
         "non-center folder activation must select the clicked folder before requesting its load");
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  expect(scene.bars_.readView().directory == std::vector<MusicSelectBarId>{{"folder:b"}},
         "the clicked directory result must survive request validation and open");
}

void testEmptyCategoryAutoplay() {
  MusicSelectScene scene;
  scene.context.chartRepository.emptyAutoplay = true;
  scene.launchSelectedDirectoryAutoplay();
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  expect(scene.launchedPlaylists.empty(), "empty category autoplay must not launch a playlist");
  expect(scene.bars_.select({"folder:b"}) && scene.bars_.open({"folder:b"}) == false,
         "rebuilding the visible root must keep the unrelated unloaded folder closed");
  const auto root = scene.bars_.readView();
  expect(!root.rowAt(0).childrenLoaded,
         "empty category autoplay must not mark its unqueried child folders loaded");
  scene.cancelDirectoryLoad();
  expect(scene.bars_.select({"folder:a"}), "category remains selectable after empty autoplay");
  const auto category = scene.bars_.readView().rowAt(0);
  (void)scene.openDirectory(category);
  expect(scene.directoryRequest_ && !scene.directoryRequest_->autoplay,
         "normal category opening must still request directory children after empty autoplay");
}

void testLatestRequestIntent() {
  for (const bool firstAutoplay : {false, true}) {
    MusicSelectScene scene;
    const auto directory = scene.bars_.readView().rowAt(0);
    scene.requestDirectoryLoad(directory, firstAutoplay);
    const auto initialGeneration = scene.directoryRequest_->generation;
    scene.requestDirectoryLoad(directory, firstAutoplay);
    expect(scene.directoryRequest_->generation == initialGeneration,
           "identical directory request intent must not restart an in-flight load");
    scene.requestDirectoryLoad(directory, !firstAutoplay);
    expect(scene.directoryRequest_->autoplay == !firstAutoplay,
           "same-directory requests must retain the latest open-versus-autoplay intent");
    scene.directoryLoader_->completePending();
    expect(scene.context.chartRepository.autoplayLoads == (firstAutoplay ? 0 : 1) &&
               scene.context.chartRepository.normalLoads == (firstAutoplay ? 1 : 0),
           "the pending worker must execute the latest intent's physical-directory query");
  }
}

void testRestoreSurvivesAnotherRevision() {
  for (const bool partiallyOpened : {false, true}) {
    MusicSelectScene scene;
    const auto ancestor = folder("folder:a", "A");
    const auto nested = folder("folder:a/nested", "Nested");
    const auto leaf = folder("folder:a/nested/leaf", "Leaf");
    scene.restoreDirectoryBars_ = {ancestor, nested, leaf};
    scene.restoreDirectories_ = {ancestor.id, nested.id, leaf.id};
    scene.restoreSelection_ = MusicSelectBarId{"song:remembered"};
    if (partiallyOpened) {
      expect(scene.bars_.installChildren(ancestor.id, {nested}) &&
                 scene.bars_.open(ancestor.id), "restore fixture opens its first ancestor");
      scene.restoreDirectories_.erase(scene.restoreDirectories_.begin());
    }
    scene.continueDirectoryRestore();
    expect(scene.directoryRequest_.has_value(),
           "restore must be waiting for asynchronous directory content");
    ++scene.libraryRevision_;
    scene.reloadLibrary(true);
    expect(scene.restoreSelection_ == MusicSelectBarId{"song:remembered"},
           "a second revision must preserve the original song selection during asynchronous restore");
    expect(scene.restoreDirectories_ ==
               std::vector<MusicSelectBarId>{ancestor.id, nested.id, leaf.id} &&
               scene.restoreDirectoryBars_.size() == 3,
           "a second revision must preserve both opened ancestors and queued restore descendants");
    expect(scene.directoryRequest_ && scene.directoryRequest_->directory.id == ancestor.id,
           "restoration after another revision must restart at the fresh root ancestor");
  }
}

void testForegroundResumesDirectoryRestore() {
  MusicSelectScene scene;
  const auto directory = scene.bars_.readView().rowAt(0);
  expect(scene.bars_.installChildren(directory.id, {song()}) &&
             scene.bars_.open(directory.id) && scene.bars_.select(song().id),
         "background restore fixture starts inside the remembered directory");
  scene.context.appInBackground = true;
  scene.onApplicationBackgroundChanged(true);
  scene.reloadLibrary(true);
  expect(!scene.directoryRequest_ && !scene.directoryLoader_ &&
             scene.restoreDirectories_ == std::vector<MusicSelectBarId>{directory.id} &&
             scene.restoreSelection_ == song().id,
         "background reload must defer the original directory and selection without loading");
  scene.context.appInBackground = false;
  scene.onApplicationBackgroundChanged(false);
  expect(scene.directoryRequest_ && scene.directoryLoader_ &&
             scene.directoryRequest_->directory.id == directory.id,
         "foreground must restart the directory restore deferred while backgrounded");
  scene.directoryLoader_->completePending();
  scene.applyDirectoryLoads();
  const auto restored = scene.bars_.readView();
  expect(restored.directory == std::vector<MusicSelectBarId>{directory.id} &&
             restored.selectedIndex < restored.rowCount() &&
             restored.rowAt(restored.selectedIndex).id == song().id &&
             scene.restoreDirectories_.empty() && !scene.restoreSelection_ &&
             !scene.directoryRequest_,
         "foreground completion must restore the original directory and selected song");
}

void testFailedSceneCancelsReadyAutoplay() {
  MusicSelectScene scene;
  scene.launchSelectedDirectoryAutoplay();
  expect(scene.directoryRequest_ && scene.directoryLoader_,
         "error fixture starts with a pending directory autoplay request");
  scene.directoryLoader_->completePending();
  scene.failed_ = true;
  scene.applyDirectoryLoads();
  expect(scene.launchedPlaylists.empty(),
         "a failed scene must not launch an already completed directory autoplay");
  expect(!scene.directoryRequest_ && scene.directoryStatusMessage_.empty() &&
             scene.directoryLoader_->takeResults().empty(),
         "a failed scene must cancel its directory request and discard ready results");
  expect(!scene.bars_.readView().rowAt(0).childrenLoaded &&
             scene.bars_.childrenOf({"folder:a"}).empty(),
         "a failed scene must not install completed directory content");
  scene.applyDirectoryLoads();
  expect(scene.launchedPlaylists.empty(),
         "discarded autoplay results must not launch on later updates");
}

int main() {
  SCENE_TEST();
}
