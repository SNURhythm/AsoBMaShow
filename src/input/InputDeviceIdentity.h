#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SdlDeviceIdentityDescriptor {
  std::string guid;
  std::string serial;
  std::string path;
  std::string name;
};

class InputDeviceIdentity {
public:
  std::string connect(const SdlDeviceIdentityDescriptor &descriptor);
  bool disconnect(std::string_view stableId);
  [[nodiscard]] std::size_t activeOwnerCount(std::string_view stableId) const;

private:
  struct ActiveIdentity {
    std::string evidence;
    std::size_t owners = 1;
    std::string ordinalPool;
    std::size_t ordinalIndex = std::numeric_limits<std::size_t>::max();
  };

  std::string connectWithEvidence(
      std::string base, std::string evidence,
      std::optional<std::string> duplicatePathHash = std::nullopt);
  std::string connectDistinct(std::string base, std::string_view marker,
                              std::size_t firstOrdinal);
  std::string allocateOrdinal(std::string_view base, std::string_view marker,
                              std::size_t firstOrdinal);
  void
  activate(std::string stableId, std::string evidence,
           std::string ordinalPool = {},
           std::size_t ordinalIndex = std::numeric_limits<std::size_t>::max());

  std::unordered_map<std::string, ActiveIdentity> activeIdentities_;
  std::unordered_map<std::string, std::string> preferredByEvidence_;
  std::unordered_map<std::string, std::string> reservedEvidenceByStableId_;
  std::unordered_map<std::string, std::vector<bool>> ordinalSlots_;
};
