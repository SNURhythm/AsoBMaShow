#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
namespace chart_goals { struct Target { int score; }; }
struct ResultPacemakerData { int targetScore; };
struct Meta { int LnMode=2; };
struct Replay { Meta chartMeta; };
struct LocalResultSource {
  Meta meta; int resultState=0, previousBest=1000;
  std::string pacemakerTarget="GOAL";
  std::optional<ResultPacemakerData> pacemakerOverride;
  std::optional<chart_goals::Target> pacemakerGoalTarget;
  std::optional<Replay> presentationReplay, retryData;
  bool autoPlayResult=false;
  struct { bool enabled=false; } practiceOptions;
};
namespace pacemaker {
const std::string kTargetGoal="GOAL";
std::string normalizeTargetId(const std::string &id) { return id; }
}
struct Scores { int reads=0, mode=0; std::int64_t id=0; int nextScore=1778; };
namespace chart_goals {
std::optional<Target> loadPacemakerTarget(Scores &scores, const Meta &, int mode,
                                         std::int64_t id, std::string &) {
  ++scores.reads; scores.mode=mode; scores.id=id; return Target{scores.nextScore};
}
}
namespace result_presentation {
std::optional<ResultPacemakerData> pacemakerDataForResult(
    const Meta &, int, const std::string &, int best, const Replay * = nullptr,
    const chart_goals::Target *goal=nullptr) {
  return ResultPacemakerData{goal ? goal->score : best};
}
}
struct ResultScene {
  LocalResultSource local;
  struct { Scores scoreRepository; struct { std::int64_t selectedPacemakerGoalId=42; } settings; } context;
  bool course=false;
  LocalResultSource *localSource() { return &local; }
  const LocalResultSource *localSource() const { return &local; }
  bool isCourseStageResult() const { return course; }
  bool isCourseFinalResult() const { return false; }
  void initializeGoalPacemaker();
  std::optional<ResultPacemakerData> pacemakerDataForCurrentResult() const;
};
METHODS
int main() {
  ResultScene scene;
  scene.local.pacemakerGoalTarget=chart_goals::Target{1756};
  assert(scene.pacemakerDataForCurrentResult()->targetScore==1756);
  scene.local.pacemakerGoalTarget.reset();
  scene.local.retryData=Replay{Meta{3}};
  scene.initializeGoalPacemaker();
  assert(scene.context.scoreRepository.reads==1 && scene.context.scoreRepository.mode==3);
  assert(scene.context.scoreRepository.id==42);
  scene.context.scoreRepository.nextScore=1900;
  assert(scene.pacemakerDataForCurrentResult()->targetScore==1778);
  assert(scene.pacemakerDataForCurrentResult()->targetScore==1778);
  assert(scene.context.scoreRepository.reads==1);
  scene.local.pacemakerOverride=ResultPacemakerData{1600};
  scene.initializeGoalPacemaker();
  assert(scene.context.scoreRepository.reads==1);
  assert(scene.pacemakerDataForCurrentResult()->targetScore==1600);
  for (int excluded=0; excluded<4; ++excluded) {
    ResultScene other;
    other.course=excluded==0;
    other.local.autoPlayResult=excluded==1;
    other.local.practiceOptions.enabled=excluded==2;
    if (excluded==3) other.local.pacemakerTarget="AAA";
    other.initializeGoalPacemaker();
    assert(other.context.scoreRepository.reads==0);
  }
}
