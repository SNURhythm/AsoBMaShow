#pragma once
#include "FolderGoal.h"
#include <array>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace folder_goals {
struct Status { bool present=false, achieved=false; };
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
