#include "InputDeviceIdentity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <ranges>
#include <sstream>

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

std::string normalizeName(std::string_view value) {
  std::string result;
  bool pendingSeparator = false;
  for (const unsigned char c : trim(value)) {
    if (std::isalnum(c)) {
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

std::string
InputDeviceIdentity::connect(const SdlDeviceIdentityDescriptor &descriptor) {
  const std::string prefix = "sdl:" + normalizeGuid(descriptor.guid) + ':';
  const std::string serial = trim(descriptor.serial);
  if (!serial.empty()) {
    return prefix + "serial:" + serial;
  }

  const std::string path = trim(descriptor.path);
  if (!path.empty()) {
    return prefix + "path:" + sha256(path);
  }

  const std::string base = prefix + "name:" + normalizeName(descriptor.name);
  auto &slots = nameSlots_[base];
  const auto available = std::ranges::find(slots, false);
  const std::size_t index = static_cast<std::size_t>(available - slots.begin());
  if (available == slots.end()) {
    slots.push_back(true);
  } else {
    *available = true;
  }

  const std::string stableId = base + ':' + std::to_string(index + 1);
  assignedNameSlots_[stableId] = {.base = base, .index = index};
  return stableId;
}

void InputDeviceIdentity::disconnect(std::string_view stableId) {
  const auto assigned = assignedNameSlots_.find(std::string(stableId));
  if (assigned == assignedNameSlots_.end()) {
    return;
  }
  auto slots = nameSlots_.find(assigned->second.base);
  if (slots != nameSlots_.end() &&
      assigned->second.index < slots->second.size()) {
    slots->second[assigned->second.index] = false;
  }
  assignedNameSlots_.erase(assigned);
}
