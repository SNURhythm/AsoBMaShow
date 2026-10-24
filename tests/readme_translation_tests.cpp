#include "library/ReadmeTranslation.h"
#include <cassert>
#include <string>
int main() {
  using namespace readme_translation;
  const std::string source = "hello\n\n한글🙂\n" + std::string(10000, 'x');
  std::string restored;
  for (const auto &part : chunks(source)) {
    assert(part.size() <= 2000);
    if (!part.empty()) assert((static_cast<unsigned char>(part[0]) & 0xc0) != 0x80);
    restored += part;
  }
  assert(restored == source);
  assert(chunks("  hello world  ") == (std::vector<std::string>{"  ", "hello world", "  "}));
  assert(chunks(std::string(100000, '\n')).size() == 50);
  const auto notes = chunks("English heading\n\n흔쾌히 사운드와 영상의 사용을 허가해 주신 여러분께 감사합니다.\nAuthor님\n\n日本語の見出し");
  const auto samples = languageSamples(notes);
  std::size_t prose = 0, author = 0;
  for (std::size_t i = 0; i < notes.size(); ++i) {
    if (notes[i].starts_with("흔쾌히")) prose = i;
    if (notes[i] == "Author님") author = i;
  }
  assert(samples[author] == prose && samples[0] == 0 && samples.back() == notes.size() - 1);
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
