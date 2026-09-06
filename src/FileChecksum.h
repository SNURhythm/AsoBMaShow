#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace file_checksum {
class Sha256 {
public:
  Sha256();

  void update(std::span<const std::byte> bytes);
  [[nodiscard]] std::array<std::byte, 32> final();
  [[nodiscard]] std::string finalHex();

private:
#if !defined(__APPLE__)
  void transform(const std::byte *block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::byte, 64> buffer_{};
  std::uint64_t totalBytes_ = 0;
  std::size_t bufferedBytes_ = 0;
#else
  CC_SHA256_CTX state_{};
#endif
  bool finalized_ = false;
  std::array<std::byte, 32> digest_{};
};

[[nodiscard]] std::string sha256(std::string_view bytes);
[[nodiscard]] std::optional<std::string> sha256File(
    const std::filesystem::path &path, std::string &errorMessage,
    std::uint64_t maximumBytes = std::numeric_limits<std::uint64_t>::max());
[[nodiscard]] std::string hexDigest(std::span<const std::byte, 32> digest);
} // namespace file_checksum
