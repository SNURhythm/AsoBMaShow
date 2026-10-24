#include "view/BmsSyntax.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using bms_syntax::Kind;
void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void span(const bms_syntax::Document &document, std::size_t begin, std::size_t end, Kind kind) {
  require(std::any_of(document.spans.begin(), document.spans.end(), [&](const auto &item) {
    return item.begin == begin && item.end == end && item.kind == kind;
  }), "expected syntax span and kind");
}
void lexicalCases() {
  const std::string source = "// comment\n#TITLE A title\n#WAV0a sounds/a; b//.wav\n#BPM 180\n"
      "#BPM0F 200.5\n#00111:01000002\n#00102:0.5\n#RANDOM 2\n#IF 1\n#ENDIF\n#UNKNOWN data";
  const auto document = bms_syntax::tokenize(source);
  require(document->length == source.size(), "source length is retained in UTF-16 units");
  const auto check = [&](const char *text, Kind kind) {
    const auto offset = source.find(text);
    span(*document, offset, offset + std::char_traits<char>::length(text), kind);
  };
  check("// comment", Kind::Comment);
  check("#TITLE", Kind::Directive);
  check("A title", Kind::String);
  check("#WAV", Kind::Directive);
  check("0a", Kind::Identifier);
  check("sounds/a; b//.wav", Kind::String);
  check("180", Kind::Number);
  check("0F", Kind::Identifier);
  check("200.5", Kind::Number);
  check("#001", Kind::Number);
  span(*document, source.find("#00111:") + 4, source.find("#00111:") + 6, Kind::Identifier);
  check("01000002", Kind::Data);
  span(*document, source.find("#00102:") + 7, source.find("#00102:") + 10, Kind::Number);
  check("#RANDOM", Kind::Control);
  check("#IF", Kind::Control);
  check("#ENDIF", Kind::Control);
  check("#UNKNOWN", Kind::Directive);
  check("data", Kind::String);
  std::uint32_t previous = 0;
  for (const auto &item : document->spans) {
    require(previous <= item.begin && item.begin < item.end && item.end <= document->length,
        "tokens are ordered, non-overlapping and in bounds");
    previous = item.end;
  }

  const auto unicode = bms_syntax::tokenize("#TITLE 한글🎵\r\n\t#00111:01\r\n");
  require(unicode->length == 25, "supplementary Unicode and CRLF offsets");
  span(*unicode, 7, 11, Kind::String);
  span(*unicode, 14, 18, Kind::Number);
  span(*unicode, 18, 20, Kind::Identifier);
  span(*unicode, 21, 23, Kind::Data);
  const auto lower = bms_syntax::tokenize("\xef\xbb\xbf  #wavZZ foo.wav\r\t#random 2\r#title 123");
  span(*lower, 3, 7, Kind::Directive);
  span(*lower, 7, 9, Kind::Identifier);
  span(*lower, 19, 26, Kind::Control);
  span(*lower, 36, 39, Kind::String);
  require(bms_syntax::tokenize("")->spans.empty(), "empty source has no syntax");
}
void scaleCases() {
  std::string payload = "#00111:" + std::string(1024 * 1024, '0');
  const auto longLine = bms_syntax::tokenize(payload);
  require(longLine->spans.size() == 3, "one-megabyte note data has one payload token, not per-cell tokens");
  const auto middle = bms_syntax::paintWindow(*longLine, 500000, 501000);
  require(middle.contains(500000, 501000) && middle.lastSpan - middle.firstSpan == 1,
      "viewport inside a huge payload needs only one syntax run");
  require(middle.end - middle.begin <= bms_syntax::maxPaintCharacters, "character work is bounded");

  std::string dense;
  for (int i = 0; i < 30000; ++i) dense += "#00111:01\n";
  const auto document = bms_syntax::tokenize(dense);
  require(document->spans.size() == 90000, "short-line token count scales with lines");
  auto window = bms_syntax::paintWindow(*document, dense.size() - 500, dense.size());
  require(window.contains(dense.size() - 500, dense.size()), "distant viewport reaches end of document");
  require(window.firstSpan > 80000 && window.lastSpan == document->spans.size(),
      "distant lookup starts at the viewport, not the beginning of the file");
  window = bms_syntax::paintWindow(*document, 0, dense.size());
  require(window.end - window.begin <= bms_syntax::maxPaintCharacters &&
      window.lastSpan - window.firstSpan <= bms_syntax::maxPaintSpans, "oversized view work is bounded");
  for (std::size_t i = window.firstSpan; i < window.lastSpan; ++i)
    require(document->spans[i].begin < window.end && document->spans[i].end > window.begin,
        "window indices describe intersecting spans only");
}
void emptyDataCases() {
  const std::string source = "#00111:0000010000100100\n#BPM 100\n#TITLE 00\n#00202:100\n";
  const auto document = bms_syntax::tokenize(source);
  const auto paint = [&](std::size_t begin, std::size_t end) {
    auto window = bms_syntax::paintWindow(*document, begin, end);
    window.begin = begin; window.end = end;
    std::vector<bms_syntax::Span> runs;
    std::size_t budget = bms_syntax::maxPaintSpans;
    bms_syntax::paintRuns(*document, window, [&](auto p) { return source.at(p); },
        [&](auto run) { runs.push_back(run); }, budget);
    return runs;
  };
  const auto runs = paint(7, 23);
  require(runs.size() == 5, "adjacent empty and populated cells coalesce into runs");
  require(runs[0].begin == 7 && runs[0].end == 11 && runs[0].kind == Kind::EmptyData,
      "consecutive 00 cells share one dimmed run");
  require(runs[2].begin == 13 && runs[2].end == 17 && runs[2].kind == Kind::EmptyData,
      "a second empty run is grouped");
  require(runs[3].begin == 17 && runs[3].end == 21 && runs[3].kind == Kind::Data,
      "00 across populated 10/01 cell boundaries is not dimmed");
  const auto clipped = paint(8, 10);
  require(clipped.size() == 1 && clipped[0].begin == 8 && clipped[0].end == 10 &&
      clipped[0].kind == Kind::EmptyData, "viewport clipping preserves original pair alignment");
  for (auto run : paint(24, source.size()))
    require(run.kind != Kind::EmptyData, "00 outside channel data stays unchanged");

  const std::string empty = "#00111:" + std::string(1000000, '0');
  const auto large = bms_syntax::tokenize(empty);
  auto window = bms_syntax::paintWindow(*large, 500000, 501000);
  std::size_t reads = 0, count = 0, budget = bms_syntax::maxPaintSpans;
  bms_syntax::paintRuns(*large, window, [&](auto p) { ++reads; return empty.at(p); },
      [&](auto run) { ++count; require(run.kind == Kind::EmptyData, "large empty run is dimmed"); }, budget);
  require(large->spans.size() == 3 && count == 1, "empty cells do not expand the document cache");
  require(reads <= 2 * (window.end - window.begin + 2), "only the paint window is scanned");

  std::string alternating = "#00111:";
  for (int i = 0; i < 10000; ++i) alternating += "0001";
  const auto dense = bms_syntax::tokenize(alternating);
  window = bms_syntax::paintWindow(*dense, 0, alternating.size());
  budget = bms_syntax::maxPaintSpans; count = 0;
  const auto paintedEnd = bms_syntax::paintRuns(*dense, window, [&](auto p) { return alternating.at(p); },
      [&](auto) { ++count; }, budget);
  require(count == bms_syntax::maxPaintSpans && budget == 0 && paintedEnd < window.end,
      "alternating cells honor a shared foreground-run budget");
}
void benchmark() {
  const auto run = [](const char *name, const std::string &text) {
    const auto start = std::chrono::steady_clock::now();
    const auto document = bms_syntax::tokenize(text);
    const auto end = std::chrono::steady_clock::now();
    std::size_t styled = 0;
    const auto lookupStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < 10000; ++i) {
      const auto begin = (i * 7919) % document->length;
      const auto window = bms_syntax::paintWindow(*document, begin, begin + 2000);
      styled += window.lastSpan - window.firstSpan;
    }
    const auto lookupEnd = std::chrono::steady_clock::now();
    std::printf("%s: bytes=%zu spans=%zu span_capacity_bytes=%zu tokenize_ms=%.2f lookup_10000_ms=%.2f mean_paint_spans=%.1f\n",
        name, text.size(), document->spans.size(), document->spans.capacity() * sizeof(bms_syntax::Span),
        std::chrono::duration<double, std::milli>(end - start).count(),
        std::chrono::duration<double, std::milli>(lookupEnd - lookupStart).count(), styled / 10000.0);
  };
  run("long-payload-64MiB", "#00111:" + std::string(64 * 1024 * 1024, '0'));
  std::string lines;
  const std::string line = "#TITLE 한글🎵\r\n#WAV01 sounds/01.ogg\r\n#00111:01000200030004000500060007000800\r\n";
  lines.reserve(16 * 1024 * 1024 + line.size());
  while (lines.size() < 16 * 1024 * 1024) lines += line;
  run("mixed-lines-16MiB", lines);
}
}
int main(int argc, char **argv) {
  lexicalCases();
  scaleCases();
  emptyDataCases();
  if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
}
