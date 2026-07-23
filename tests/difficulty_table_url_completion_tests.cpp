#include "scene/DifficultyTableUrlCompletion.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

} // namespace

int main() {
  std::string url = "https://example.com/table.html";

  expect(!settings_ui::applyDifficultyTableUrlCompletion(false, false, url),
         "an in-progress import does not clear the URL");
  expect(url == "https://example.com/table.html",
         "an in-progress import preserves the URL text");

  expect(!settings_ui::applyDifficultyTableUrlCompletion(true, false, url),
         "a failed import does not clear the URL");
  expect(url == "https://example.com/table.html",
         "a failed import preserves the URL text");

  expect(settings_ui::applyDifficultyTableUrlCompletion(true, true, url),
         "a successful import reports that the URL was cleared");
  expect(url.empty(), "a successful import clears the URL text");

  return 0;
}
