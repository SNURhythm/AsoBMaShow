#pragma once
#include "ChartGoal.h"
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace chart_goals {
// Cursor movement only checks the cached set; recommendations read best-score
// summaries once per profile, score, or library revision.
class RecommendationIndex {
public:
  template<class Loader>
  void refresh(const std::string &profile, std::uint64_t scores,
               std::uint64_t library, Loader load) {
    const auto revision = std::tuple{profile, scores, library};
    if (revision_ == revision) return;
    hashes_.clear();
    revision_.reset();
    hashes_ = load();
    revision_ = revision;
  }
  bool contains(std::string_view hash) const {
    if (hash.size() != 64) return false;
    try { return hashes_.contains(normalizedHash(hash)); }
    catch (const std::invalid_argument &) { return false; }
  }
private:
  std::optional<std::tuple<std::string, std::uint64_t, std::uint64_t>> revision_;
  std::unordered_set<std::string> hashes_;
};
}
