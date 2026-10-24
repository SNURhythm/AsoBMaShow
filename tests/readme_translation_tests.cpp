#include "library/ReadmeTranslation.h"
#include <cassert>
#include <string>
int main() {
  using namespace readme_translation;
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
