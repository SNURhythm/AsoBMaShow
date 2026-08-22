#include "skin/beatoraja/LuaSkinJavaPattern.h"

#include <iomanip>
#include <iostream>
#include <string>

namespace {

void printHex(std::string_view value) {
  std::cout << "MATCH:";
  for (const unsigned char byte : value) {
    std::cout << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<unsigned int>(byte);
  }
  std::cout << '\n';
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: lua_skin_java_pattern_tests PATTERN SUBJECT\n";
    return 2;
  }
  const auto compiled = skin::LuaSkinJavaPattern::compile(argv[1]);
  if (!compiled) {
    std::cout << "INVALID\n";
    return 0;
  }
  const auto match = compiled->find(argv[2]);
  if (!match) {
    std::cout << "NO_MATCH\n";
    return 0;
  }
  printHex(*match);
  return 0;
}
