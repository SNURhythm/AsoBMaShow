#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
