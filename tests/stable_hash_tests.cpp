#include "StableHash.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testExistingIdentityValues() {
  struct Case {
    std::string bytes;
    std::uint64_t expected;
  };
  // Recorded from the original archive, Android, audio, MIDI, and queue helpers.
  const Case cases[] = {
      {"", 0xcbf29ce484222325ULL},
      {"a", 0xaf63dc4c8601ec8cULL},
      {"foobar", 0x85944171f73967e8ULL},
      {std::string("a\0b", 3), 0xe5d29919042666b2ULL},
      {std::string("\x80\xff\0\x7f", 4), 0x7d83f4abf79470edULL},
      {"content://com.example/tree/primary%3AMusic", 0x77bb0f3cd034901bULL},
      {"track-id\n12", 0x15448953f0efbd4fULL},
  };
  for (const auto &test : cases) {
    require(stable_hash::fnv1a64(test.bytes) == test.expected,
            "persisted identity hash changed");
  }

  std::string allBytes;
  for (unsigned int byte = 0; byte < 256; ++byte) {
    allBytes.push_back(static_cast<char>(byte));
  }
  require(stable_hash::fnv1a64(allBytes) == 0x4242dc5249c33625ULL,
          "all byte values must retain unsigned hashing semantics");
  const std::string surrounding = "xfoobarx";
  require(stable_hash::fnv1a64(std::string_view(surrounding).substr(1, 6)) ==
              0x85944171f73967e8ULL,
          "hashing must respect the view length without a terminator");
}

void testExistingHexadecimalNames() {
  require(stable_hash::hex64(0) == "0000000000000000",
          "zero must retain all sixteen digits");
  require(stable_hash::hex64(1) == "0000000000000001",
          "small values must retain leading zeros");
  require(stable_hash::hex64(0x0123456789abcdefULL) == "0123456789abcdef",
          "digits must remain lowercase and most-significant first");
  require(stable_hash::hex64(0xffffffffffffffffULL) == "ffffffffffffffff",
          "full-width values must retain every bit");
}

}

int main() {
  testExistingIdentityValues();
  testExistingHexadecimalNames();
  return 0;
}
