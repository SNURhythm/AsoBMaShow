#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

enum class MusicSelectRankingState { Finish, Fail };
struct MusicSelectRankingSnapshot {
  MusicSelectRankingState state = MusicSelectRankingState::Finish;
  int pendingDurationMillis = 0;
};
namespace ir {
enum class IrRankingSnapshotState { Succeeded };
struct Snapshot {
  int generation = 1;
  int revision = 1;
  IrRankingSnapshotState state = IrRankingSnapshotState::Succeeded;
  struct Ranking { bool nextPageToken = false; };
  std::optional<Ranking> ranking;
  bool loadingNextPage = false;
  bool paginationBlocked = false;
};
struct Service {
  Snapshot value;
  int open(int) { return 1; }
  Snapshot snapshot() { return value; }
  bool loadNextPage(int) { return false; }
};
}
MusicSelectRankingSnapshot projectMusicSelectRanking(const ir::Snapshot &, int) { return {}; }
std::int64_t nowMillis = 0;
std::int64_t unixMillis() { return nowMillis; }
struct MusicSelectScene {
  struct Context { ir::Service *irRankingService; } context;
  struct CachedRanking {
    MusicSelectRankingSnapshot snapshot;
    std::int64_t updatedUnixMillis = 0;
  };
  MusicSelectRankingSnapshot ranking_;
  std::optional<int> rankingRequest_ = 1;
  std::map<std::string, CachedRanking, std::less<>> rankingCache_;
  std::string rankingCacheKey_;
  int rankingGeneration_ = 1;
  int rankingRevision_ = 0;
  int rankingLoadAtMicros_ = -1;
  int rankingOffset_ = 0;
  int elapsedMicros() { return 1; }
  void setRanking(MusicSelectRankingSnapshot snapshot) { ranking_ = snapshot; }
  void updateRanking();
};
SCENE_METHODS
int main() {
  ir::Service service;
  MusicSelectScene scene{{&service}};
  auto finish = [&](const std::string &key) {
    scene.rankingCacheKey_ = key;
    ++service.value.revision;
    ++nowMillis;
    scene.updateRanking();
  };
  for (int index = 0; index < 64; ++index) finish(std::to_string(index));
  assert(scene.rankingCache_.size() == 64);
  finish("0");
  assert(scene.rankingCache_.size() == 64 && "refresh must not evict an unrelated chart");
  finish("64");
  assert(scene.rankingCache_.size() == 64 && "scene cache must remain bounded");
  assert(scene.rankingCache_.contains("0") && !scene.rankingCache_.contains("1"));
  assert(scene.rankingCache_.contains("64"));
  for (int index = 65; index < 200; ++index) finish(std::to_string(index));
  assert(scene.rankingCache_.size() == 64);
  assert(!scene.rankingCache_.contains("0") && scene.rankingCache_.contains("199"));
}
