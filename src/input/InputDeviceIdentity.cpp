#include "InputDeviceIdentity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

namespace {

std::string trim(std::string_view value) {
  const auto first = std::ranges::find_if(
      value, [](unsigned char c) { return !std::isspace(c); });
  const auto last =
      std::ranges::find_if(value | std::views::reverse,
                           [](unsigned char c) { return !std::isspace(c); });
  if (first == value.end()) {
    return {};
  }
  return std::string(first, last.base());
}

std::string normalizeGuid(std::string_view value) {
  std::string result;
  for (const unsigned char c : trim(value)) {
    if (std::isalnum(c)) {
      result.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return result.empty() ? "unknown" : result;
}

bool isUnreserved(unsigned char value) {
  return value < 0x80U && (std::isalnum(value) || value == '-' ||
                           value == '.' || value == '_' || value == '~');
}

void appendEscapedByte(std::string &result, unsigned char value) {
  constexpr std::array hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                              '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  result.push_back('%');
  result.push_back(hex[value >> 4]);
  result.push_back(hex[value & 0x0fU]);
}

std::string percentEncode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char byte : value) {
    if (isUnreserved(byte)) {
      result.push_back(static_cast<char>(byte));
    } else {
      appendEscapedByte(result, byte);
    }
  }
  return result;
}

std::string normalizeName(std::string_view value) {
  std::string result;
  bool pendingSeparator = false;
  for (const unsigned char c : trim(value)) {
    if (c >= 0x80U) {
      if (pendingSeparator && !result.empty()) {
        result.push_back('-');
      }
      appendEscapedByte(result, c);
      pendingSeparator = false;
    } else if (std::isalnum(c)) {
      if (pendingSeparator && !result.empty()) {
        result.push_back('-');
      }
      result.push_back(static_cast<char>(std::tolower(c)));
      pendingSeparator = false;
    } else {
      pendingSeparator = true;
    }
  }
  return result.empty() ? "unknown" : result;
}

constexpr std::array<std::uint32_t, 64> sha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::string sha256(std::string_view value) {
  std::vector<std::uint8_t> bytes(value.begin(), value.end());
  const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
  bytes.push_back(0x80U);
  while (bytes.size() % 64 != 56) {
    bytes.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>(bitLength >> shift));
  }

  std::array<std::uint32_t, 8> hash = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                       0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                       0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint32_t, 64> words{};

  for (std::size_t chunk = 0; chunk < bytes.size(); chunk += 64) {
    for (std::size_t i = 0; i < 16; ++i) {
      const std::size_t offset = chunk + i * 4;
      words[i] = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                 (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                 (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                 static_cast<std::uint32_t>(bytes[offset + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      const std::uint32_t s0 = std::rotr(words[i - 15], 7) ^
                               std::rotr(words[i - 15], 18) ^
                               (words[i - 15] >> 3);
      const std::uint32_t s1 = std::rotr(words[i - 2], 17) ^
                               std::rotr(words[i - 2], 19) ^
                               (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    std::uint32_t f = hash[5];
    std::uint32_t g = hash[6];
    std::uint32_t h = hash[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t sum1 =
          std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t temp1 =
          h + sum1 + choice + sha256Constants[i] + words[i];
      const std::uint32_t sum0 =
          std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = sum0 + majority;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint32_t word : hash) {
    output << std::setw(8) << word;
  }
  return output.str();
}

} // namespace

InputDeviceIdentity::ClassifiedIdentity InputDeviceIdentity::classify(
    const SdlDeviceIdentityDescriptor &descriptor) const {
  const std::string prefix = "sdl:" + normalizeGuid(descriptor.guid) + ':';
  const std::string serial = trim(descriptor.serial);
  if (!serial.empty()) {
    const std::string base = prefix + "serial:" + percentEncode(serial);
    return {.kind = IdentityKind::Serial,
            .base = base,
            .evidence = descriptor.path.empty()
                            ? std::string{}
                            : base + '\x1f' + descriptor.path,
            .exactPath = descriptor.path};
  }
  if (!descriptor.path.empty()) {
    const std::string base = prefix + "path:" + sha256(descriptor.path);
    return {.kind = IdentityKind::Path,
            .base = base,
            .evidence = base + '\x1f' + descriptor.path,
            .exactPath = descriptor.path};
  }
  return {.kind = IdentityKind::Name,
          .base = prefix + "name:" + normalizeName(descriptor.name)};
}

std::string
InputDeviceIdentity::connect(const SdlDeviceIdentityDescriptor &descriptor) {
  pendingRemappings_.clear();
  return connectClassified(classify(descriptor));
}

std::vector<std::string> InputDeviceIdentity::connectBatch(
    std::span<const SdlDeviceIdentityDescriptor> descriptors) {
  pendingRemappings_.clear();
  std::vector<ClassifiedIdentity> identities;
  identities.reserve(descriptors.size());

  struct SerialGroup {
    std::set<std::string> exactPaths;
    std::size_t pathlessDevices = 0;
  };
  std::map<std::string, SerialGroup> serialGroups;
  for (const auto &descriptor : descriptors) {
    auto identity = classify(descriptor);
    if (identity.kind == IdentityKind::Serial) {
      auto &group = serialGroups[identity.base];
      if (identity.exactPath.empty()) {
        ++group.pathlessDevices;
      } else {
        group.exactPaths.insert(identity.exactPath);
      }
    }
    identities.push_back(std::move(identity));
  }
  for (const auto &[base, group] : serialGroups) {
    if (group.exactPaths.size() + group.pathlessDevices > 1) {
      serialLedgers_[base].collisionKnown = true;
    }
  }

  std::vector<std::string> result;
  result.reserve(identities.size());
  for (const auto &identity : identities) {
    result.push_back(connectClassified(identity));
  }
  return result;
}

std::vector<InputDeviceIdentityRemap> InputDeviceIdentity::takeRemappings() {
  return std::exchange(pendingRemappings_, {});
}

std::string
InputDeviceIdentity::connectClassified(const ClassifiedIdentity &identity) {
  if (identity.kind == IdentityKind::Name) {
    const auto ordinal = reserveOrdinal(identity.base, {}, 1);
    activate(ordinal.stableId, identity, ordinal.pool, ordinal.index);
    return ordinal.stableId;
  }

  if (identity.kind == IdentityKind::Path) {
    if (const auto active = activeIdentities_.find(identity.base);
        active != activeIdentities_.end()) {
      if (active->second.evidence == identity.evidence) {
        ++active->second.owners;
        return identity.base;
      }
      const auto ordinal = reserveOrdinal(identity.base, "ordinal", 2);
      activate(ordinal.stableId, identity, ordinal.pool, ordinal.index);
      return ordinal.stableId;
    }
    activate(identity.base, identity);
    return identity.base;
  }

  auto &ledger = serialLedgers_[identity.base];
  if (ledger.collisionKnown) {
    return connectKnownSerialCollision(identity);
  }

  ActiveIdentity *activeSerial = nullptr;
  for (auto &[stableId, active] : activeIdentities_) {
    (void)stableId;
    if (active.kind == IdentityKind::Serial && active.base == identity.base) {
      activeSerial = &active;
      break;
    }
  }
  if (activeSerial == nullptr) {
    activate(identity.base, identity);
    return identity.base;
  }
  if (!identity.evidence.empty() &&
      activeSerial->evidence == identity.evidence) {
    ++activeSerial->owners;
    return identity.base;
  }

  establishSerialCollision(identity.base);
  return connectKnownSerialCollision(identity);
}

std::string InputDeviceIdentity::connectKnownSerialCollision(
    const ClassifiedIdentity &identity) {
  if (!identity.exactPath.empty()) {
    const std::string stableId =
        identity.base + ":path:" + sha256(identity.exactPath);
    if (const auto active = activeIdentities_.find(stableId);
        active != activeIdentities_.end()) {
      if (active->second.evidence == identity.evidence) {
        ++active->second.owners;
        return stableId;
      }
      const auto ordinal = reserveOrdinal(stableId, "ordinal", 2);
      activate(ordinal.stableId, identity, ordinal.pool, ordinal.index);
      return ordinal.stableId;
    }
    activate(stableId, identity);
    return stableId;
  }

  // Pathless devices with the same serialized descriptor are physically
  // indistinguishable. Runtime ordinals prevent aliasing, but cannot promise
  // that a persisted ordinal follows the same physical unit after reconnect.
  const auto ordinal = reserveOrdinal(identity.base, "ordinal", 1);
  activate(ordinal.stableId, identity, ordinal.pool, ordinal.index);
  return ordinal.stableId;
}

void InputDeviceIdentity::establishSerialCollision(
    std::string_view serialBase) {
  serialLedgers_[std::string(serialBase)].collisionKnown = true;
  std::vector<std::string> activeStableIds;
  for (const auto &[stableId, active] : activeIdentities_) {
    if (active.kind == IdentityKind::Serial && active.base == serialBase) {
      activeStableIds.push_back(stableId);
    }
  }
  std::ranges::sort(activeStableIds);

  for (const auto &oldStableId : activeStableIds) {
    auto node = activeIdentities_.extract(oldStableId);
    ActiveIdentity &active = node.mapped();
    std::string newStableId;
    if (!active.exactPath.empty()) {
      newStableId = active.base + ":path:" + sha256(active.exactPath);
      active.ordinalPool.clear();
      active.ordinalIndex = std::numeric_limits<std::size_t>::max();
    } else {
      const auto ordinal = reserveOrdinal(active.base, "ordinal", 1);
      newStableId = ordinal.stableId;
      active.ordinalPool = ordinal.pool;
      active.ordinalIndex = ordinal.index;
    }
    node.key() = newStableId;
    activeIdentities_.insert(std::move(node));
    pendingRemappings_.push_back(
        {.fromStableId = oldStableId, .toStableId = newStableId});
  }
}

InputDeviceIdentity::ReservedOrdinal InputDeviceIdentity::reserveOrdinal(
    std::string_view base, std::string_view marker, std::size_t firstOrdinal) {
  const std::string pool = std::string(base) + '\x1f' + std::string(marker) +
                           '\x1f' + std::to_string(firstOrdinal);
  auto &slots = ordinalSlots_[pool];
  const auto available = std::ranges::find(slots, false);
  const std::size_t index = static_cast<std::size_t>(available - slots.begin());
  if (available == slots.end()) {
    slots.push_back(true);
  } else {
    *available = true;
  }

  std::string stableId(base);
  stableId.push_back(':');
  if (!marker.empty()) {
    stableId.append(marker);
    stableId.push_back(':');
  }
  stableId.append(std::to_string(firstOrdinal + index));
  return {.stableId = std::move(stableId), .pool = pool, .index = index};
}

bool InputDeviceIdentity::disconnect(std::string_view stableId) {
  const auto active = activeIdentities_.find(std::string(stableId));
  if (active == activeIdentities_.end()) {
    return false;
  }
  if (active->second.owners > 1) {
    --active->second.owners;
    return false;
  }

  if (!active->second.ordinalPool.empty()) {
    auto slots = ordinalSlots_.find(active->second.ordinalPool);
    if (slots != ordinalSlots_.end() &&
        active->second.ordinalIndex < slots->second.size()) {
      slots->second[active->second.ordinalIndex] = false;
    }
  }
  activeIdentities_.erase(active);
  return true;
}

std::size_t
InputDeviceIdentity::activeOwnerCount(std::string_view stableId) const {
  const auto active = activeIdentities_.find(std::string(stableId));
  return active == activeIdentities_.end() ? 0 : active->second.owners;
}

void InputDeviceIdentity::activate(std::string stableId,
                                   const ClassifiedIdentity &identity,
                                   std::string ordinalPool,
                                   std::size_t ordinalIndex) {
  activeIdentities_.insert_or_assign(
      std::move(stableId), ActiveIdentity{.kind = identity.kind,
                                          .base = identity.base,
                                          .evidence = identity.evidence,
                                          .exactPath = identity.exactPath,
                                          .owners = 1,
                                          .ordinalPool = std::move(ordinalPool),
                                          .ordinalIndex = ordinalIndex});
}
