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

void printResult(std::string_view pattern, std::string_view subject) {
  const auto compiled = skin::LuaSkinJavaPattern::compile(pattern);
  if (!compiled) {
    std::cout << "INVALID\n";
    return;
  }
  const auto match = compiled->find(subject);
  if (!match) {
    std::cout << "NO_MATCH\n";
    return;
  }
  printHex(*match);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc % 2 == 0) {
    std::cerr << "usage: lua_skin_java_pattern_tests PATTERN SUBJECT "
                 "[PATTERN SUBJECT ...]\n";
    return 2;
  }
  for (int index = 1; index < argc; index += 2) {
    printResult(argv[index], argv[index + 1]);
  }
  return 0;
}
