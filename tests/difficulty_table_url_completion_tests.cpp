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
  const std::string submitted = "https://example.com/table.html";
  std::string url = submitted;

  expect(!settings_ui::applyDifficultyTableUrlCompletion(false, false,
                                                          submitted, url),
         "an in-progress import does not clear the URL");
  expect(url == submitted,
         "an in-progress import preserves the URL text");

  expect(!settings_ui::applyDifficultyTableUrlCompletion(true, false,
                                                          submitted, url),
         "a failed import does not clear the URL");
  expect(url == submitted, "a failed import preserves the URL text");

  url = "https://example.com/next-table.html";
  expect(!settings_ui::applyDifficultyTableUrlCompletion(true, true,
                                                          submitted, url),
         "a successful import does not clear a newer URL");
  expect(url == "https://example.com/next-table.html",
         "a newer URL survives completion of the previous import");

  url = submitted;
  expect(settings_ui::applyDifficultyTableUrlCompletion(true, true, submitted,
                                                         url),
         "a successful matching import reports that the URL was cleared");
  expect(url.empty(), "a successful matching import clears the URL text");

  return 0;
}
