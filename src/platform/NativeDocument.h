#pragma once

#include "../view/TextRange.h"
#include "../view/BmsSyntax.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace native_document {
// Normalized coordinates within the SDL window's client area, top-left origin.
struct Frame { float x = 0, y = 0, width = 0, height = 0; };
// Colors use 0xRRGGBBAA; font size is in native logical points (Android sp).
struct Appearance {
  std::uint32_t foreground = 0xffffffff;
  std::uint32_t background = 0x181d27ff;
  std::uint32_t highlight = 0xffc04d70;
  float fontSize = 16;
  std::array<std::uint32_t, bms_syntax::kindCount> syntax = {
      0x70d6ffff, 0xc69af5ff, 0xe6bb78ff, 0xffab82ff,
      0x7bda92ff, 0x99a7bbff, 0xb2c6e5ff};
};
enum class Scroll { LineUp, LineDown, PageUp, PageDown, Start, End };
struct State {
  std::atomic_bool dismissed = false;
  // Safe from native UI callbacks; never captures the owning scene.
  void requestDismiss();
};
class Surface {
public:
  virtual ~Surface() = default;
  virtual void setFrame(Frame frame) = 0;
  virtual void setAppearance(Appearance appearance) = 0;
  virtual void setSyntax(std::shared_ptr<const bms_syntax::Document> syntax) = 0;
  // UTF-8 byte ranges in the original decoded text. Keep native selection
  // independent of practice highlights; focus scrolls to the first highlight.
  virtual void highlight(const std::vector<text_range::Range> &ranges, bool focus) = 0;
  virtual void scroll(Scroll action) = 0;
  bool dismissRequested() const { return state_->dismissed.load(); }
protected:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
// Native controls are read-only but selectable. Destruction removes the native
// view, retires queued UI work, and restores SDL focus. All functions may be
// called on the application worker; backends dispatch to their native UI owner.
std::unique_ptr<Surface> create(const std::string &text, Frame frame, Appearance appearance);

// Convert validated UTF-8 byte ranges to UTF-16 code-unit ranges for native text
// APIs. Invalid endpoints are ignored; output is sorted and overlapping ranges
// are merged. Implemented in NativeDocument.cpp and shared by all backends.
std::vector<text_range::Range> utf16Ranges(std::string_view text,
    const std::vector<text_range::Range> &ranges);

namespace detail {
std::unique_ptr<Surface> createApple(const std::string &, Frame, Appearance);
std::unique_ptr<Surface> createAndroid(const std::string &, Frame, Appearance);
std::unique_ptr<Surface> createWindows(const std::string &, Frame, Appearance);
}
} // namespace native_document
