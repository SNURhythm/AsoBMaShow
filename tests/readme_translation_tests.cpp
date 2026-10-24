#include "library/ReadmeTranslation.h"
#include <cassert>
#include <string>
int main() {
  using namespace readme_translation;
  const std::string linked = "Read this: https://example.com/a?q=1&x=2\n다음 www.example.org/path.\n";
  const auto parts = standardParts(linked);
  std::string restored;
  int urls = 0;
  for (const auto &part : parts) {
    restored += part.text;
    if (part.text.find("https://") != std::string::npos || part.text.find("www.") != std::string::npos) {
      assert(!part.translate);
      ++urls;
    }
    if (part.translate) assert(!part.text.empty() && part.text.front() != ' ' && part.text.back() != ' ');
  }
  assert(restored == linked && urls == 2);
  const auto whole = standardParts("Whole document\nwith no URL.\n");
  assert(whole.size() == 1 && whole[0].translate && whole[0].text == "Whole document\nwith no URL.\n");
  const auto onlyUrl = standardParts("https://example.com/한글?q=1#part");
  assert(onlyUrl.size() == 1 && !onlyUrl[0].translate && onlyUrl[0].text == "https://example.com/한글?q=1#part");
  Request first;
  const auto old = first.begin();
  first.cancel();
  deliver(old, State::Complete, "stale");
  assert(!first.take());
  const auto current = first.begin();
  deliver(old, State::Complete, "still stale");
  assert(!first.take());
  deliver(current, State::Working, "");
  assert(first.take()->state == State::Working);
  deliver(current, State::Complete, "translated");
  assert(first.take()->text == "translated");
  deliver(current, State::Failed, "late duplicate");
  assert(!first.take());
  const auto oversized = first.begin();
  deliver(oversized, State::Complete, std::string(maximumOutputBytes + 1, 'x'));
  auto rejected = first.take();
  assert(rejected && rejected->state == State::Failed && rejected->text.empty());
}
