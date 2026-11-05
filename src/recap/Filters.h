#pragma once
#include "PlayRecap.h"
#include <algorithm>
#include <unordered_set>

namespace recap {
// An absent hash restriction means all difficulties; an empty set means none
// of the active table ranges matched. Filtering never changes period bounds.
inline void filterSnapshot(Snapshot &snapshot, bool includeNew,
    const std::optional<std::unordered_set<std::string>> &hashes) {
  for (auto &section : snapshot.sections)
    std::erase_if(section,[&](const Upgrade &upgrade) {
      return (!includeNew && !upgrade.before) || (hashes && !hashes->contains(upgrade.hash));
    });
}
} // namespace recap
