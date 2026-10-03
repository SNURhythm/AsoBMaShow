#include <cassert>
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>

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
    std::atomic<std::uint64_t> irRankingEvidenceRevision{0};
  } context;
  std::uint64_t libraryRevision_ = 0;
  std::uint64_t scoreRevision_ = 0;
  std::uint64_t irRankingEvidenceRevision_ = 0;
  std::map<int, int> rankingCache_;
  std::optional<int> rankingRequest_ = 1;
  std::int64_t rankingLoadAtMicros_ = -1;
  std::int64_t elapsedMicros() const { return 1000; }
  bool rankingModalOpen = true;
  int reloads = 0;
  int selectionRefreshes = 0;
  void reloadLibrary() {
    ++reloads;
    libraryRevision_ = context.chartRepository.GetLibraryRevision();
    scoreRevision_ = context.scoreRepository.GetRevision();
  }
  void selectedBarMoved() { ++selectionRefreshes; rankingModalOpen = false; }
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
  scene.rankingCache_[1] = 123;
  scene.rankingModalOpen = true;
  ++scene.context.irRankingEvidenceRevision;
  scene.refreshRepositoryRevisions();
  assert(scene.rankingCache_.empty() && scene.selectionRefreshes == 3 &&
         scene.reloads == 3 &&
         "a completed upload invalidates only ranking data without resetting selection");
  assert(scene.rankingModalOpen && scene.rankingLoadAtMicros_ == 999);
  scene.rankingCache_[1] = 456;
  scene.refreshRepositoryRevisions();
  assert(scene.rankingCache_.at(1) == 456 && scene.selectionRefreshes == 3);
}
