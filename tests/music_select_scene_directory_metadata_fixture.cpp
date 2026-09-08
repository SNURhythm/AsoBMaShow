#include <cassert>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct MusicSelectRepositoryMetadata {
  struct Folder { std::string path; int addDateSeconds; };
  std::vector<Folder> folders;
};
struct Directory { std::filesystem::path directoryPath; std::string id; };
struct MusicSelectBarManagerConfig {
  std::string modeFilter;
  std::string difficultyFilter;
  std::string sortId;
};
struct Content {
  std::vector<int> children;
  std::shared_ptr<int> provider;
};
MusicSelectRepositoryMetadata projectedMetadata;
bool useProvider = false;

Content loadMusicSelectPhysicalDirectory(
    int, const MusicSelectRepositoryMetadata &metadata, const Directory &,
    int, int, const std::filesystem::path &, const MusicSelectBarManagerConfig &,
    int) {
  projectedMetadata = metadata;
  return {{123}, useProvider ? std::make_shared<int>(456) : nullptr};
}

struct SceneFixture {
  struct Context {
    int chartRepository = 0;
    struct Replay {
      std::filesystem::path GetResolvedProfileRoot() { return "/profile"; }
    } replayRepository;
    struct Settings {
      std::string skinModeFilterName = "ALL";
      std::string skinDifficultyFilterName = "ALL";
      std::string skinSortId = "TITLE";
    } settings;
  } context;
  struct Bars {
    int installed = 0;
    bool installRowProvider(const std::string &, std::shared_ptr<int> provider) {
      installed = *provider;
      return true;
    }
    bool installChildren(const std::string &, std::vector<int> children) {
      installed = children.front();
      return true;
    }
  } bars_;
  std::shared_ptr<MusicSelectRepositoryMetadata> repositoryMetadata_ =
      std::make_shared<MusicSelectRepositoryMetadata>();
  int scoreCache_ = 0;
  int clearRankCache_ = 0;
  bool load(const Directory &directory) {
    const int selectedLongNoteMode = 2;
    SCENE_FOLDER_BRANCH
  }
};

int main() {
  SceneFixture scene;
  scene.repositoryMetadata_->folders = {{"category/empty", 123},
                                        {"category/populated", 456}};
  assert(scene.load({"category", "folder:category"}));
  assert(scene.bars_.installed == 123);
  assert(projectedMetadata.folders.size() == 2);
  assert(projectedMetadata.folders.front().path == "category/empty");
  assert(projectedMetadata.folders.front().addDateSeconds == 123);
  useProvider = true;
  assert(scene.load({"category", "folder:category"}));
  assert(scene.bars_.installed == 456);
}
