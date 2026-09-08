#pragma once

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace music_select_skin_ledger_evidence {

inline int finish(int argc, char **argv, std::string_view runner, int failures,
                  std::vector<std::string> identifiers,
                  std::string_view failureLabel,
                  std::string_view successMessage) {
  if (failures != 0) {
    std::cerr << failures << ' ' << failureLabel << '\n';
    return 1;
  }
  std::ranges::sort(identifiers);
  if (identifiers.empty() ||
      std::adjacent_find(identifiers.begin(), identifiers.end()) !=
          identifiers.end()) {
    std::cerr << "music-select ledger evidence is missing or duplicated\n";
    return 1;
  }
  if (argc == 1) {
    std::cout << successMessage << '\n';
    return 0;
  }
  const std::string_view command(argv[1]);
  if (command == "--assert-ledger-id" && argc == 3) {
    if (!std::ranges::binary_search(identifiers, std::string(argv[2]))) {
      return 1;
    }
    std::cout << "{\"runner\":\"" << runner
              << "\",\"assertionIds\":[\"" << argv[2] << "\"]}\n";
    return 0;
  }
  if (command != "--list-ledger-assertions" || argc != 2) {
    return 2;
  }
  std::cout << "{\"runner\":\"" << runner << "\",\"assertionIds\":[";
  for (std::size_t index = 0; index < identifiers.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << '\"' << identifiers[index] << '\"';
  }
  std::cout << "]}\n";
  return 0;
}

} // namespace music_select_skin_ledger_evidence
