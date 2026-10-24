#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace bms_syntax {
// Keep ordinals stable: Android receives these values in a compact int array.
enum class Kind : std::uint8_t { Directive, Control, Identifier, Number, String, Comment, Data, Count };
inline constexpr std::size_t kindCount = static_cast<std::size_t>(Kind::Count);
// UTF-16 code-unit offsets in the original decoded source (before Rich Edit's
// CRLF normalization). Channel data is one span, never one span per note cell.
struct Span { std::uint32_t begin, end; Kind kind; };
struct Document {
  std::vector<Span> spans;
  std::uint32_t length = 0;
};
// Separate opt-in source viewer work; never called by chart Parse/Scan. Inputs
// beyond the native controls' 32-bit text range remain plain text.
std::shared_ptr<const Document> tokenize(std::string_view utf8);

inline constexpr std::uint32_t maxPaintCharacters = 16384;
inline constexpr std::size_t maxPaintSpans = 4096;
inline constexpr std::uint32_t paintOverscan = 1024;
struct PaintWindow {
  std::uint32_t begin = 0, end = 0;
  std::size_t firstSpan = 0, lastSpan = 0; // Half-open indices into Document::spans.
  bool contains(std::size_t first, std::size_t last) const {
    return begin <= first && last <= end;
  }
};
// O(log(span count) + maxPaintSpans), no text access or allocation. Clamp each
// intersecting span to [begin,end) when applying attributes. Backends may reuse
// a painted window while it contains their visible range; force on theme change.
PaintWindow paintWindow(const Document &document, std::size_t visibleBegin, std::size_t visibleEnd);
} // namespace bms_syntax
