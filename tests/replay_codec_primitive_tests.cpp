#include "FileChecksum.h"
#include "replay/Base64Url.h"
#include "replay/GzipCodec.h"
#include "replay/ReplayLimits.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Bytes bytes(std::string_view text) {
  Bytes result(text.size());
  std::transform(text.begin(), text.end(), result.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return result;
}

void testBase64UrlCanonicalAndBounded() {
  const Bytes binary{std::byte{0xfb}, std::byte{0xff}};
  std::string diagnostic;
  expect(replay::base64UrlEncode(binary) == "-_8=",
         "Base64URL encoder uses URL-safe padded alphabet");
  expect(replay::base64UrlDecodeBounded("-_8=", 2, diagnostic) ==
             std::optional<Bytes>(binary),
         "canonical padded Base64URL decodes");
  expect(replay::base64UrlDecodeBounded("-_8", 2, diagnostic) ==
             std::optional<Bytes>(binary),
         "canonical unpadded Base64URL decodes");
  expect(!replay::base64UrlDecodeBounded("+/8=", 2, diagnostic),
         "ordinary Base64 alphabet is rejected");
  expect(!replay::base64UrlDecodeBounded("-_9", 2, diagnostic),
         "non-canonical tail bits are rejected");
  expect(!replay::base64UrlDecodeBounded("-_8=\n", 2, diagnostic),
         "whitespace is rejected");
  expect(!replay::base64UrlDecodeBounded("-_8=", 1, diagnostic),
         "decoded-size bound is enforced before output allocation");
}

void testGzipFramingIntegrityAndBound() {
  const Bytes source = bytes("gzip and Base64URL fixture payload");
  std::string diagnostic;
  const auto compressed = replay::gzipCompress(source, diagnostic);
  expect(compressed.has_value(), "gzip compression succeeds");
  if (!compressed) {
    return;
  }
  expect(compressed->size() >= 18 && (*compressed)[0] == std::byte{0x1f} &&
             (*compressed)[1] == std::byte{0x8b},
         "gzip output has interoperable framing");
  expect(
      replay::gzipDecompressBounded(*compressed, source.size(), diagnostic) ==
          std::optional<Bytes>(source),
      "exact output bound accepts a complete stream");
  expect(!replay::gzipDecompressBounded(*compressed, source.size() - 1,
                                        diagnostic),
         "gzip expansion past configured limit is rejected");

  Bytes corrupt = *compressed;
  corrupt.back() ^= std::byte{0x01};
  expect(!replay::gzipDecompressBounded(corrupt, 1024, diagnostic),
         "trailer size corruption is rejected");
  corrupt = *compressed;
  corrupt[corrupt.size() - 8] ^= std::byte{0x01};
  expect(!replay::gzipDecompressBounded(corrupt, 1024, diagnostic),
         "CRC corruption is rejected");
  expect(!replay::gzipDecompressBounded(bytes("not gzip"), 1024, diagnostic),
         "malformed gzip framing is rejected");

  Bytes trailing = *compressed;
  trailing.insert(trailing.end() - 8, std::byte{0});
  expect(!replay::gzipDecompressBounded(trailing, 1024, diagnostic),
         "trailing deflate data is rejected");
}

void testIndependentFixtureHashes() {
  struct Fixture {
    std::string_view name;
    std::string_view sha256;
  };
  constexpr Fixture fixtures[]{
      {"beatoraja-chart.brd",
       "c4f2a22b571a9bc31f5df0290ce9ab80cd39ad42a8c27d708707f8fe7f170ba7"},
      {"beatoraja-course.brd",
       "75fce78c355a62cf9b21a5f971e019a5682a130c0f941ac2e4ced233ad2d0b08"},
      {"beatoraja-keyinput.bin",
       "ec101f21efe5c18fb8562ca495f034ccfa75dbab304ac00de647fd3691a8bc1d"},
  };
  const auto root = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests" /
                    "fixtures" / "replay";
  for (const auto &fixture : fixtures) {
    std::string diagnostic;
    const auto digest =
        file_checksum::sha256File(root / fixture.name, diagnostic,
                                  replay::kReplayLimits.maxCompressedBytes);
    expect(digest == fixture.sha256,
           "independent Beatoraja fixture has its pinned digest");
  }
  expect(
      std::filesystem::is_regular_file(root / "BeatorajaFixtureGenerator.java"),
      "fixture generator is retained beside golden bytes");
}

} // namespace

int main() {
  testBase64UrlCanonicalAndBounded();
  testGzipFramingIntegrityAndBound();
  testIndependentFixtureHashes();
  if (failures != 0) {
    std::cerr << failures << " replay primitive test(s) failed\n";
    return 1;
  }
  std::cout << "replay primitive tests passed\n";
  return 0;
}
