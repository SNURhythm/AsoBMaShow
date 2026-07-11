#pragma once

#include <cstddef>
#include <limits>
#include <span>
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

struct InputDeviceIdentityRemap {
  std::string fromStableId;
  std::string toStableId;
};

// Serial collision policy:
// - Startup batches assign deterministic path-suffixed IDs to every pathful
//   member of a known duplicate-serial group, independent of enumeration.
// - A sole serialized device uses its serial base and may change paths. If a
//   distinct live owner appears, the base is retired before path IDs publish.
// - The collision ledger is runtime-scoped. A fresh process seeing only one
//   former duplicate cannot distinguish it from the prior sole owner, so a
//   base binding follows that sole serial. A path-suffixed binding stays
//   missing until the duplicate set is known again; once a collision is known
//   in the current run, the base is never reused.
// - Truly indistinguishable pathless duplicates use runtime ordinals. A saved
//   ordinal follows the slot assigned on a later run, not a guaranteed unit.
class InputDeviceIdentity {
public:
  std::string connect(const SdlDeviceIdentityDescriptor &descriptor);
  std::vector<std::string>
  connectBatch(std::span<const SdlDeviceIdentityDescriptor> descriptors);
  std::vector<InputDeviceIdentityRemap> takeRemappings();
  bool disconnect(std::string_view stableId);
  [[nodiscard]] std::size_t activeOwnerCount(std::string_view stableId) const;

private:
  enum class IdentityKind { Serial, Path, Name };

  struct ClassifiedIdentity {
    IdentityKind kind = IdentityKind::Name;
    std::string base;
    std::string evidence;
    std::string exactPath;
  };

  struct ActiveIdentity {
    IdentityKind kind = IdentityKind::Name;
    std::string base;
    std::string evidence;
    std::string exactPath;
    std::size_t owners = 1;
    std::string ordinalPool;
    std::size_t ordinalIndex = std::numeric_limits<std::size_t>::max();
  };

  struct SerialCollisionLedger {
    bool collisionKnown = false;
  };

  struct ReservedOrdinal {
    std::string stableId;
    std::string pool;
    std::size_t index = 0;
  };

  ClassifiedIdentity
  classify(const SdlDeviceIdentityDescriptor &descriptor) const;
  std::string connectClassified(const ClassifiedIdentity &identity);
  std::string connectKnownSerialCollision(const ClassifiedIdentity &identity);
  void establishSerialCollision(std::string_view serialBase);
  ReservedOrdinal reserveOrdinal(std::string_view base, std::string_view marker,
                                 std::size_t firstOrdinal);
  void
  activate(std::string stableId, const ClassifiedIdentity &identity,
           std::string ordinalPool = {},
           std::size_t ordinalIndex = std::numeric_limits<std::size_t>::max());

  std::unordered_map<std::string, ActiveIdentity> activeIdentities_;
  std::unordered_map<std::string, SerialCollisionLedger> serialLedgers_;
  std::unordered_map<std::string, std::vector<bool>> ordinalSlots_;
  std::vector<InputDeviceIdentityRemap> pendingRemappings_;
};
