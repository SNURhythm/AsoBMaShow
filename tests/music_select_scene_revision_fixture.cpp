#include <cassert>
#include <cstdint>

struct Repository {
  std::uint64_t revision = 0;
  std::uint64_t GetRevision() const { return revision; }
  std::uint64_t GetLibraryRevision() const { return revision; }
};

struct MusicSelectScene {
  struct Modal {
    bool active = true;
    bool inProgress() const { return active; }
  };
  Modal *archiveUnzipModal_ = nullptr;
  struct Context {
    Repository chartRepository;
    Repository scoreRepository;
  } context;
  std::uint64_t libraryRevision_ = 0;
  std::uint64_t scoreRevision_ = 0;
  int reloads = 0;
  int selectionRefreshes = 0;
  void reloadLibrary() {
    ++reloads;
    libraryRevision_ = context.chartRepository.GetLibraryRevision();
    scoreRevision_ = context.scoreRepository.GetRevision();
  }
  void selectedBarMoved() { ++selectionRefreshes; }
  void refreshRepositoryRevisions();
};

SCENE_METHODS

int main() {
  MusicSelectScene scene;
  scene.refreshRepositoryRevisions();
  assert(scene.reloads == 0);
  ++scene.context.scoreRepository.revision;
  scene.refreshRepositoryRevisions();
  assert(scene.reloads == 1 && scene.selectionRefreshes == 1 &&
         "IR score writes must refresh selector projections without a library write");
  for (int frame = 0; frame < 100; ++frame) scene.refreshRepositoryRevisions();
  assert(scene.reloads == 1 && "unchanged revisions must not reload per frame");
  ++scene.context.scoreRepository.revision;
  ++scene.context.chartRepository.revision;
  scene.refreshRepositoryRevisions();
  assert(scene.reloads == 2 && scene.selectionRefreshes == 2);
  MusicSelectScene::Modal modal;
  scene.archiveUnzipModal_ = &modal;
  for (int archive = 0; archive < 5; ++archive) {
    ++scene.context.chartRepository.revision;
    scene.refreshRepositoryRevisions();
    assert(scene.reloads == 2);
  }
  modal.active = false;
  scene.refreshRepositoryRevisions();
  scene.refreshRepositoryRevisions();
  assert(scene.reloads == 3 && scene.selectionRefreshes == 3);
}
