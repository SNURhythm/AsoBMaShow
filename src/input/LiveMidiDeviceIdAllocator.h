#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class LiveMidiDeviceRefreshActionKind {
  Remove,
  Add,
};

struct LiveMidiDeviceRefreshAction {
  LiveMidiDeviceRefreshActionKind kind;
  std::uintptr_t key;
};

[[nodiscard]] std::vector<LiveMidiDeviceRefreshAction>
planLiveMidiDeviceRefresh(std::span<const std::uintptr_t> existingKeys,
                          std::span<const std::uintptr_t> currentKeys);

class LiveMidiDeviceIdAllocator {
public:
  [[nodiscard]] std::string claim(std::uintptr_t key,
                                  std::string preferredId);
  void release(std::uintptr_t key);
  void clear();

private:
  std::unordered_map<std::uintptr_t, std::string> claimedByKey_;
  std::unordered_set<std::string> claimedIds_;
};
