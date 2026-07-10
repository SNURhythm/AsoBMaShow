#include "LiveMidiDeviceIdAllocator.h"

#include <cstddef>
#include <utility>

std::string LiveMidiDeviceIdAllocator::claim(std::uintptr_t key,
                                             std::string preferredId) {
  const auto existing = claimedByKey_.find(key);
  if (existing != claimedByKey_.end()) {
    return existing->second;
  }

  std::string claimedId = preferredId;
  for (std::size_t ordinal = 2; claimedIds_.contains(claimedId); ++ordinal) {
    claimedId = preferredId + ":session:" + std::to_string(ordinal);
  }
  claimedIds_.insert(claimedId);
  claimedByKey_.emplace(key, claimedId);
  return claimedId;
}

void LiveMidiDeviceIdAllocator::release(std::uintptr_t key) {
  const auto existing = claimedByKey_.find(key);
  if (existing == claimedByKey_.end()) {
    return;
  }
  claimedIds_.erase(existing->second);
  claimedByKey_.erase(existing);
}

void LiveMidiDeviceIdAllocator::clear() {
  claimedByKey_.clear();
  claimedIds_.clear();
}
