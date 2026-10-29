#pragma once
#include "FolderGoal.h"
#include <array>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace folder_goals {
struct PendingTarget { Target target; double percent=0; };
struct Status {
  bool present=false, achieved=false;
  int pendingLampCount=0, pendingExCount=0, pendingCoverageCount=0;
  std::optional<PendingTarget> pendingLamp, pendingEx, pendingCoverage;
};
// Selection is deliberately absent from this key: cursor motion must never
// scan score summaries or reaggregate large collections.
class StatusIndex {
public:
  template<class Loader>
  bool refresh(const std::string &profile,std::uint64_t scores,std::uint64_t library,Loader load) {
    const auto revision=std::tuple{profile,scores,library};
    if (revision_==revision) return true;
    auto rows=load();
    values_.clear(); revision_.reset();
    if (!rows) return false;
    for (const auto &row:*rows) {
      auto &status=values_[sourceKey(row.goal.source)][row.goal.longNoteMode-1];
      status.achieved=(!status.present || status.achieved) && row.progress.achieved;
      status.present=true;
      if (!row.progress.achieved) {
        ++(row.goal.target.type==Type::Lamp ? status.pendingLampCount :
            row.goal.target.type==Type::AverageEx ? status.pendingExCount : status.pendingCoverageCount);
        auto &pending=row.goal.target.type==Type::Lamp ? status.pendingLamp :
            row.goal.target.type==Type::AverageEx ? status.pendingEx : status.pendingCoverage;
        if (!pending) pending=PendingTarget{row.goal.target,row.progress.percent};
      }
    }
    revision_=revision;
    return true;
  }
  Status status(const Source &source,int mode) const {
    if (mode<1 || mode>3) return {};
    const auto found=values_.find(sourceKey(source));
    return found==values_.end() ? Status{} : found->second[mode-1];
  }
private:
  std::optional<std::tuple<std::string,std::uint64_t,std::uint64_t>> revision_;
  std::map<std::string,std::array<Status,3>> values_;
};
}
