#include "FileChecksum.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>

namespace file_checksum {
#if !defined(__APPLE__)
namespace {
constexpr std::array<std::uint32_t, 64> kRoundConstants = {
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

std::uint32_t loadBigEndian(const std::byte *bytes) {
  return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
         std::to_integer<std::uint32_t>(bytes[3]);
}
} // namespace
#endif

#if defined(__APPLE__)
Sha256::Sha256() { CC_SHA256_Init(&state_); }
#else
Sha256::Sha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

#if defined(__clang__) && !defined(NDEBUG)
#pragma clang optimize on
#elif defined(__GNUC__) && !defined(__clang__) && !defined(NDEBUG)
__attribute__((optimize("O3")))
#elif defined(_MSC_VER) && !defined(NDEBUG)
#pragma optimize("gt", on)
#endif
void Sha256::transform(const std::byte *block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = loadBigEndian(block + index * 4);
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const std::uint32_t s0 = std::rotr(words[index - 15], 7) ^
                             std::rotr(words[index - 15], 18) ^
                             (words[index - 15] >> 3U);
    const std::uint32_t s1 = std::rotr(words[index - 2], 17) ^
                             std::rotr(words[index - 2], 19) ^
                             (words[index - 2] >> 10U);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t sum1 =
        std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRoundConstants[index] + words[index];
    const std::uint32_t sum0 =
        std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}
#if defined(__clang__) && !defined(NDEBUG)
#pragma clang optimize off
#elif defined(_MSC_VER) && !defined(NDEBUG)
#pragma optimize("", off)
#endif
#endif

void Sha256::update(std::span<const std::byte> bytes) {
  if (finalized_ || bytes.empty()) {
    return;
  }
#if defined(__APPLE__)
  while (!bytes.empty()) {
    const auto count = static_cast<CC_LONG>(std::min<std::size_t>(
        bytes.size(), std::numeric_limits<CC_LONG>::max()));
    CC_SHA256_Update(&state_, bytes.data(), count);
    bytes = bytes.subspan(count);
  }
#else
  totalBytes_ += bytes.size();
  if (bufferedBytes_ != 0) {
    const std::size_t count =
        std::min(buffer_.size() - bufferedBytes_, bytes.size());
    std::copy_n(bytes.begin(), count, buffer_.begin() + bufferedBytes_);
    bufferedBytes_ += count;
    bytes = bytes.subspan(count);
    if (bufferedBytes_ == buffer_.size()) {
      transform(buffer_.data());
      bufferedBytes_ = 0;
    }
  }
  while (bytes.size() >= buffer_.size()) {
    transform(bytes.data());
    bytes = bytes.subspan(buffer_.size());
  }
  if (!bytes.empty()) {
    std::copy(bytes.begin(), bytes.end(), buffer_.begin());
    bufferedBytes_ = bytes.size();
  }
#endif
}

std::array<std::byte, 32> Sha256::final() {
  if (finalized_) {
    return digest_;
  }
#if defined(__APPLE__)
  CC_SHA256_Final(reinterpret_cast<unsigned char *>(digest_.data()), &state_);
#else
  const std::uint64_t bitCount = totalBytes_ * 8U;
  buffer_[bufferedBytes_++] = std::byte{0x80};
  if (bufferedBytes_ > 56) {
    std::fill(buffer_.begin() + bufferedBytes_, buffer_.end(), std::byte{0});
    transform(buffer_.data());
    bufferedBytes_ = 0;
  }
  std::fill(buffer_.begin() + bufferedBytes_, buffer_.begin() + 56,
            std::byte{0});
  for (std::size_t index = 0; index < 8; ++index) {
    buffer_[63 - index] =
        static_cast<std::byte>((bitCount >> (index * 8U)) & 0xffU);
  }
  transform(buffer_.data());
  for (std::size_t word = 0; word < state_.size(); ++word) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      digest_[word * 4 + byte] =
          static_cast<std::byte>((state_[word] >> ((3U - byte) * 8U)) & 0xffU);
    }
  }
#endif
  finalized_ = true;
  return digest_;
}

std::string Sha256::finalHex() { return hexDigest(final()); }

std::string hexDigest(std::span<const std::byte, 32> digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string encoded(64, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(digest[index]);
    encoded[index * 2] = kHex[value >> 4U];
    encoded[index * 2 + 1] = kHex[value & 0x0fU];
  }
  return encoded;
}

std::string sha256(std::string_view bytes) {
  Sha256 hash;
  hash.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
  return hash.finalHex();
}

std::optional<std::string> sha256File(const std::filesystem::path &path,
                                      std::string &errorMessage,
                                      std::uint64_t maximumBytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "unable to open file for checksum: " + path.string();
    return std::nullopt;
  }
  Sha256 hash;
  std::uint64_t totalBytes = 0;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      const auto unsignedCount = static_cast<std::uint64_t>(count);
      if (unsignedCount > maximumBytes - totalBytes) {
        errorMessage = "file exceeds checksum size limit: " + path.string();
        return std::nullopt;
      }
      hash.update(std::as_bytes(
          std::span(buffer.data(), static_cast<std::size_t>(count))));
      totalBytes += unsignedCount;
    }
  }
  if (!input.eof()) {
    errorMessage = "unable to read file for checksum: " + path.string();
    return std::nullopt;
  }
  return hash.finalHex();
}
} // namespace file_checksum
