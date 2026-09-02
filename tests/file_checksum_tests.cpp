#include "FileChecksum.h"

#include <algorithm>
#include <cassert>
#include <span>
#include <string>

int main() {
  assert(file_checksum::sha256("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const std::string millionAs(1'000'000, 'a');
  const std::string expected =
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  assert(file_checksum::sha256(millionAs) == expected);

  file_checksum::Sha256 chunked;
  const auto bytes = std::as_bytes(
      std::span(millionAs.data(), millionAs.size()));
  for (std::size_t offset = 0; offset < bytes.size(); offset += 31) {
    chunked.update(bytes.subspan(offset, std::min<std::size_t>(31,
                                                               bytes.size() - offset)));
  }
  assert(chunked.finalHex() == expected);
}
