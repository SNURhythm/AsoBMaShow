#include "platform/NativeDocument.h"
#include <cassert>

int main() {
  const std::string text = "#TITLE 한글🎵\n#00011:0102\n";
  const auto data = text.find("0102");
  const auto emoji = text.find("🎵");
  auto ranges = native_document::utf16Ranges(text,
      {{data + 2, data + 4}, {data, data + 3}, {emoji, emoji + 4}});
  assert(ranges.size() == 2);
  assert(ranges[0].begin == 9 && ranges[0].end == 11);
  assert(ranges[1].begin == 19 && ranges[1].end == 23);
  ranges = native_document::utf16Ranges(text,
      {{emoji + 1, emoji + 4}, {0, 0}, {20, 2}, {data, text.size() + 1}});
  assert(ranges.empty());
  ranges = native_document::utf16Ranges("A\r\nB", {{3, 4}});
  assert(ranges.size() == 1 && ranges[0].begin == 3 && ranges[0].end == 4);
  ranges = native_document::utf16Ranges("🎵가Z", {{7, 8}});
  assert(ranges.size() == 1 && ranges[0].begin == 3 && ranges[0].end == 4);
  assert(native_document::utf16Ranges("", {}).empty());
  native_document::State state;
  assert(!state.dismissed.load());
  state.requestDismiss();
  assert(state.dismissed.load());
}
