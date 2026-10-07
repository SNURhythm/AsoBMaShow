#include "FileChecksum.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <span>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  assert(file_checksum::sha256("") ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(file_checksum::sha256("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(file_checksum::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Padding and block boundaries, independently generated with Python hashlib.
  const std::pair<std::size_t, const char *> vectors[] = {
      {55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
      {56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
      {63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
      {64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
      {65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
      {129, "4f1757ae4bffbae86d775b831765b75af154d52f7deaa46dd378051a2d3ad57f"},
      {4096, "4e441a3533bb2c10cd5649981d395744213e09a336746b5a3458fee4057205ec"}};
  for (const auto &[length, expected] : vectors) {
    std::vector<std::byte> input(length);
    for (std::size_t i = 0; i < length; ++i) input[i] = std::byte((i * 37 + 11) % 256);
    for (std::size_t chunk : {1, 7, 31, 64, 65, 4096}) {
      file_checksum::Sha256 hash;
      for (std::size_t offset = 0; offset < length; offset += chunk) {
        hash.update(std::span(input).subspan(offset, std::min(chunk, length - offset)));
      }
      assert(hash.finalHex() == expected);
      hash.update(input); // Finalized digests are stable.
      assert(hash.finalHex() == expected);
    }
  }

  file_checksum::Sha256 prefix;
  prefix.update(std::as_bytes(std::span("ab", 2)));
  auto fork = prefix;
  file_checksum::Sha256 assigned;
  assigned = prefix;
  prefix.update(std::as_bytes(std::span("c", 1)));
  fork.update(std::as_bytes(std::span("d", 1)));
  assigned.update(std::as_bytes(std::span("e", 1)));
  assert(prefix.finalHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert(fork.finalHex() == "a52d159f262b2c6ddb724a61840befc36eb30c88877a4030b65cbe86298449c9");
  assert(assigned.finalHex() == "d81a65c1de02e17d9cfd88d68a8768fd1e3262f5e2fb859382fe33734b3f3ca8");

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

  // Optional device-only throughput probe; ordinary CTest has no timing gate.
  if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
    std::vector<std::byte> input(256 * 1024, std::byte{0x5a});
    file_checksum::Sha256 hash;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1024; ++i) hash.update(input);
    assert(hash.finalHex() == "d4e0d5a6082e9536f1ff4fbc69855d8b3e458328f27af8d72cb104d8e81b5bc2");
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "SHA-256 256 MiB: " << milliseconds << " ms\n";
    if (argc > 2 && milliseconds > std::stod(argv[2])) return 1;
  }
}
