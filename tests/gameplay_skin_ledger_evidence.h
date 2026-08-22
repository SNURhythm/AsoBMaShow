#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

namespace gameplay_skin_ledger_evidence {

inline std::string_view expectedProof(std::string_view runner) {
  if (runner == "json_gameplay_skin_decoder_tests") {
    return "all-json-fields-decode";
  }
  if (runner == "beatoraja_skin_model_tests") {
    return "all-lua-model-fields";
  }
  if (runner == "lr2_gameplay_skin_decoder_tests") {
    return "all-lr2-gameplay-commands";
  }
  if (runner == "lr2_skin_csv_parser_tests") {
    return "all-lr2-csv-commands";
  }
  if (runner == "lua_skin_host_modules_tests") {
    return "closed-lua-host-surface";
  }
  if (runner == "lua_skin_file_system_tests") {
    return "selected-file-behavior";
  }
  if (runner == "lua_skin_text_graph_live_integration_tests") {
    return "live-text-graph-behavior";
  }
  return {};
}

inline std::vector<std::string> assertionIds(std::string_view runner) {
  std::ifstream input(
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/ledger/native_assertion_coverage_v1.tsv");
  std::vector<std::string> result;
  std::string line;
  const auto proof = expectedProof(runner);
  while (std::getline(input, line)) {
    const auto first = line.find('\t');
    const auto second = line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos) continue;
    if (std::string_view(line).substr(0, first) != runner) continue;
    if (std::string_view(line).substr(second + 1) != proof) {
      std::cerr << "ledger evidence names an unexpected native proof\n";
      return {};
    }
    result.push_back(line.substr(first + 1, second - first - 1));
  }
  if (!input.eof()) return {};
  std::ranges::sort(result);
  if (result.empty() || std::adjacent_find(result.begin(), result.end()) !=
                            result.end()) {
    return {};
  }
  return result;
}

inline int finish(int argc, char **argv, std::string_view runner, int failures,
                  std::string_view failureLabel,
                  std::string_view successMessage) {
  if (failures != 0) {
    std::cerr << failures << ' ' << failureLabel << '\n';
    return 1;
  }
  if (argc == 1) {
    std::cout << successMessage << '\n';
    return 0;
  }
  const auto ids = assertionIds(runner);
  if (ids.empty()) {
    std::cerr << "native ledger assertion coverage is missing\n";
    return 1;
  }
  const std::string_view command(argv[1]);
  if (command == "--assert-ledger-id" && argc == 3) {
    if (!std::ranges::binary_search(ids, std::string(argv[2]))) return 1;
    std::cout << "{\"runner\":\"" << runner
              << "\",\"assertionIds\":[\"" << argv[2] << "\"]}\n";
    return 0;
  }
  if (command != "--list-ledger-assertions" || argc != 2) return 2;
  std::cout << "{\"runner\":\"" << runner << "\",\"assertionIds\":[";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << '\"' << ids[index] << '\"';
  }
  std::cout << "]}\n";
  return 0;
}

} // namespace gameplay_skin_ledger_evidence
