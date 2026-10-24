#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "NativeDocument.h"
#include "SDLMainThread.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <ole2.h>
#include <richole.h>
#include <tom.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace native_document {
namespace {

COLORREF rgb(std::uint32_t color) {
  return RGB((color >> 24) & 255, (color >> 16) & 255, (color >> 8) & 255);
}
COLORREF highlightColor(Appearance appearance) {
  const unsigned alpha = appearance.highlight & 255;
  const auto channel = [&](unsigned shift) {
    return (((appearance.highlight >> shift) & 255) * alpha +
            ((appearance.background >> shift) & 255) * (255 - alpha) + 127) / 255;
  };
  return RGB(channel(24), channel(16), channel(8));
}

struct NativeText {
  std::wstring text;
  std::vector<std::size_t> collapsedLF;

  explicit NativeText(const std::string &source) {
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
      throw std::runtime_error("Native document exceeds Rich Edit's character limit");
    if (source.empty()) return;
    const auto bytes = static_cast<int>(source.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        source.data(), bytes, nullptr, 0);
    if (!length) throw std::runtime_error("Invalid native document UTF-8");
    text.resize(length);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), bytes, text.data(), length);
    std::size_t out = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const wchar_t character = text[i];
      if (character == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
        text[out++] = L'\r';
        collapsedLF.push_back(++i);
      } else {
        // Rich Edit stores one CR per paragraph. LF-only decoded chart source
        // therefore keeps its UTF-16 positions; tolerate CRLF callers as well.
        // A visible replacement for NUL avoids silently truncating the source.
        text[out++] = character == L'\n' ? L'\r' : character == 0 ? L'\ufffd' : character;
      }
    }
    text.resize(out);
  }
  LONG position(std::size_t original) const {
    const auto removed = std::lower_bound(collapsedLF.begin(), collapsedLF.end(), original) -
                         collapsedLF.begin();
    return static_cast<LONG>(std::min(original - removed, text.size()));
  }
  std::shared_ptr<const bms_syntax::Document> syntaxPositions(
      std::shared_ptr<const bms_syntax::Document> syntax) const {
    if (!syntax || syntax->length != text.size() + collapsedLF.size()) return nullptr;
    if (collapsedLF.empty()) return syntax;
    auto mapped = std::make_shared<bms_syntax::Document>();
    mapped->length = static_cast<std::uint32_t>(text.size());
    mapped->spans.reserve(syntax->spans.size());
    std::size_t removed = 0;
    const auto position = [&](std::uint32_t original) {
      while (removed < collapsedLF.size() && collapsedLF[removed] < original) ++removed;
      return static_cast<std::uint32_t>(original - removed);
    };
    // Sorted, non-overlapping endpoints and collapsed LFs merge in one pass on
    // the caller's worker. UI scrolling only searches the cached native spans.
    for (const auto &span : syntax->spans) {
      const auto begin = position(span.begin), end = position(span.end);
      if (begin < end) mapped->spans.push_back({begin, end, span.kind});
    }
    return mapped;
  }
};

struct TextStream {
  const char *data;
  std::size_t remaining;
  static DWORD CALLBACK read(DWORD_PTR cookie, LPBYTE buffer, LONG capacity, LONG *copied) {
    auto &stream = *reinterpret_cast<TextStream *>(cookie);
    *copied = static_cast<LONG>(std::min(stream.remaining, static_cast<std::size_t>(capacity)));
    std::memcpy(buffer, stream.data, *copied);
    stream.data += *copied;
    stream.remaining -= *copied;
    return 0;
  }
};

// Each segment is a horizontal slice of one visible display line. Never join
// across paragraph boundaries: a hidden tail can be millions of characters.
// Charge both limits across the entire viewport, not once per row.
std::vector<bms_syntax::PaintWindow> syntaxWindows(const bms_syntax::Document &syntax,
    const std::vector<CHARRANGE> &visible) {
  std::vector<bms_syntax::PaintWindow> windows;
  windows.reserve(visible.size());
  std::uint32_t characters = bms_syntax::maxPaintCharacters;
  std::size_t spans = bms_syntax::maxPaintSpans;
  for (const auto &segment : visible) {
    if (!characters || !spans) break;
    const auto begin = std::min(static_cast<std::uint32_t>(segment.cpMin), syntax.length);
    auto end = std::min(static_cast<std::uint32_t>(segment.cpMax), syntax.length);
    end = begin + std::min(end - begin, characters);
    if (begin == end) continue;
    const auto first = std::lower_bound(syntax.spans.begin(), syntax.spans.end(), begin,
        [](const bms_syntax::Span &span, std::uint32_t position) { return span.end <= position; });
    auto last = std::lower_bound(first, syntax.spans.end(), end,
        [](const bms_syntax::Span &span, std::uint32_t position) { return span.begin < position; });
    if (static_cast<std::size_t>(last - first) > spans) {
      last = first + spans;
      end = std::min(end, (last - 1)->end);
    }
    windows.push_back({begin, end, static_cast<std::size_t>(first - syntax.spans.begin()),
        static_cast<std::size_t>(last - syntax.spans.begin())});
    characters -= end - begin;
    spans -= static_cast<std::size_t>(last - first);
  }
  return windows;
}

// Mutable native state is accessed only by the SDL/Win32 window owner; the
// worker may retire the control and post to its atomic cancellation target.
// The subclass holds shared ownership, including across TrackPopupMenu's nested
// message loop, so destroying the Surface cannot invalidate an active callback.
struct Control : std::enable_shared_from_this<Control> {
  std::shared_ptr<State> state;
  std::atomic_bool retired = false;
  std::atomic<HWND> cancelTarget = nullptr;
  UINT cancelMessage = 0;
  HWND parent = nullptr, edit = nullptr;
  SDL_WindowID windowID = 0;
  ITextDocument *document = nullptr;
  Frame frame;
  Appearance appearance;
  std::vector<CHARRANGE> ranges, paintedRanges;
  std::shared_ptr<const bms_syntax::Document> syntax;
  std::vector<bms_syntax::PaintWindow> syntaxWindowsPainted;
  std::vector<CHARRANGE> syntaxViewport;
  LONG length = 0;
  bool startedTextInput = false, menuActive = false;
  bool formatting = false, syntaxPainted = false;

  explicit Control(std::shared_ptr<State> shared) : state(std::move(shared)) {}

  void nativeFocus(bool focused) {
    auto *window = SDL_GetWindowFromID(windowID);
    if (!window) { startedTextInput = false; return; }
    if (focused && !SDL_TextInputActive(window)) {
      // SDL's raw keyboard pump otherwise delivers arrows a second time to
      // the overlay. In text-input mode only the HWND receiving WM_KEYDOWN
      // handles those keys. Do not replace the application's global filter.
      startedTextInput = SDL_StartTextInput(window);
      SDL_ResetKeyboard();
    } else if (!focused && startedTextInput) {
      SDL_StopTextInput(window);
      startedTextInput = false;
    }
  }

  void layout() {
    if (!edit) return;
    RECT client{};
    if (!GetClientRect(parent, &client)) return;
    const auto coordinate = [](float value, LONG extent) {
      return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * extent));
    };
    const int x = coordinate(frame.x, client.right), y = coordinate(frame.y, client.bottom);
    const int right = coordinate(frame.x + frame.width, client.right);
    const int bottom = coordinate(frame.y + frame.height, client.bottom);
    SetWindowPos(edit, HWND_TOP, x, y, std::max(0, right - x), std::max(0, bottom - y),
        SWP_NOACTIVATE);
  }

  void colorRange(CHARRANGE span, COLORREF color, bool foreground = false) {
    ITextRange *range = nullptr;
    if (FAILED(document->Range(span.cpMin, span.cpMax, &range))) return;
    ITextFont *font = nullptr;
    if (SUCCEEDED(range->GetFont(&font))) {
      if (foreground) font->SetForeColor(static_cast<LONG>(color));
      else font->SetBackColor(static_cast<LONG>(color));
      font->Release();
    }
    range->Release();
  }

  std::vector<CHARRANGE> visibleSegments() const {
    std::vector<CHARRANGE> segments;
    RECT client{}, format{}, visible{};
    GetClientRect(edit, &client);
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    if (!IntersectRect(&visible, &client, &format) || !length) return segments;
    const auto characterAt = [&](LONG x, LONG y) {
      POINTL point{x, y};
      return static_cast<LONG>(std::clamp<LRESULT>(SendMessageW(edit, EM_CHARFROMPOS,
          0, reinterpret_cast<LPARAM>(&point)), 0, length));
    };
    const auto firstLine = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    const auto lastLine = SendMessageW(edit, EM_EXLINEFROMCHAR, 0,
        characterAt(visible.left, visible.bottom - 1));
    // Even pathological geometry has fixed query/allocation bounds. Empty rows
    // count too; do not walk a document looking for the next nonempty line.
    const auto lineCount = std::min<LRESULT>(std::max<LRESULT>(0, lastLine - firstLine + 1),
        bms_syntax::maxPaintSpans);
    segments.reserve(static_cast<std::size_t>(lineCount));
    for (LRESULT row = 0; row < lineCount; ++row) {
      const auto start = SendMessageW(edit, EM_LINEINDEX, firstLine + row, 0);
      if (start < 0 || start >= length) break;
      const LONG lineBegin = static_cast<LONG>(start);
      const LONG lineEnd = lineBegin + static_cast<LONG>(std::clamp<LRESULT>(
          SendMessageW(edit, EM_LINELENGTH, lineBegin, 0), 0, length - lineBegin));
      if (lineBegin == lineEnd) continue;
      POINTL origin{};
      SendMessageW(edit, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&origin), lineBegin);
      if (origin.y >= visible.bottom) break;
      // Clamp the first partially visible row into the formatting rectangle.
      // Hit-test each row at its own y; bottom-right alone includes hidden tails
      // of all preceding rows. Clip EOL hits to this row, excluding its CR.
      const LONG y = std::max(origin.y, visible.top);
      const LONG begin = std::clamp(characterAt(visible.left, y), lineBegin, lineEnd);
      const LONG right = std::clamp(characterAt(visible.right - 1, y), begin, lineEnd);
      const LONG end = right < lineEnd ? right + 1 : lineEnd;
      if (begin < end) segments.push_back({begin, end});
    }
    return segments;
  }

  void paintSyntax(bool force = false) {
    if (retired || !edit || !document || formatting || (!syntax && !syntaxPainted)) return;
    // Guard before querying/formatting: Rich Edit can synchronously send size,
    // scroll and paint messages during TOM transactions and Unfreeze.
    formatting = true;
    auto visible = visibleSegments();
    if (!force && syntaxPainted && visible.size() == syntaxViewport.size() &&
        std::equal(visible.begin(), visible.end(), syntaxViewport.begin(),
            [](CHARRANGE a, CHARRANGE b) { return a.cpMin == b.cpMin && a.cpMax == b.cpMax; })) {
      // Cache the actual row slices, including when their total exceeds a cap.
      // Equal outer endpoints alone do not describe a nonwrapping viewport.
      formatting = false;
      return;
    }
    auto next = syntax ? syntaxWindows(*syntax, visible) : std::vector<bms_syntax::PaintWindow>{};
    LONG count = 0;
    const bool frozen = SUCCEEDED(document->Freeze(&count));
    SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    // Reset all previous bounded row segments, including any overlapping
    // section. Retired foreground runs coalesce back to the default; visiting
    // more source never accumulates a document-wide syntax attribute table.
    for (const auto &window : syntaxWindowsPainted)
      colorRange({static_cast<LONG>(window.begin), static_cast<LONG>(window.end)},
          rgb(appearance.foreground), true);
    std::size_t budget = bms_syntax::maxPaintSpans, painted = 0;
    std::array<wchar_t, bms_syntax::maxPaintCharacters + 3> source;
    for (auto &window : next) {
      if (!budget) break;
      // Read only this visible row slice, plus one character at either edge
      // so clipped two-character cells keep their original alignment.
      const auto readBegin = window.begin ? window.begin - 1 : 0;
      const auto readEnd = window.end + (window.end < syntax->length ? 1 : 0);
      TEXTRANGEW sample{{static_cast<LONG>(readBegin), static_cast<LONG>(readEnd)}, source.data()};
      const auto copied = SendMessageW(edit, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&sample));
      window.end = bms_syntax::paintRuns(*syntax, window, [&](auto position) {
        const auto offset = position - readBegin;
        return offset < static_cast<std::size_t>(std::max<LRESULT>(0, copied)) ? source[offset] : L'?';
      }, [&](auto span) {
        const auto kind = static_cast<std::size_t>(span.kind);
        if (kind < appearance.syntax.size())
          colorRange({static_cast<LONG>(span.begin), static_cast<LONG>(span.end)},
              rgb(appearance.syntax[kind]), true);
      }, budget);
      ++painted;
    }
    next.resize(painted);
    syntaxWindowsPainted.swap(next);
    syntaxViewport.swap(visible);
    syntaxPainted = syntax != nullptr;
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    if (frozen) document->Unfreeze(&count);
    // Foreground-only TOM ranges do not scroll or change layout/selection.
    // Do not round-trip EM_GETSCROLLPOS here: its documented 16-bit coordinates
    // can truncate a distant viewport in a large document or long channel line.
    formatting = false;
  }

  void paint(bool updateAppearance, bool focus) {
    if (!edit || !document || formatting) return;
    auto nextPaintedRanges = ranges; // Allocate before changing read-only/freeze state.
    formatting = true;
    POINT scroll{};
    SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    LONG count = 0;
    const bool frozen = SUCCEEDED(document->Freeze(&count));
    // TOM refuses formatting a read-only document. No message pump runs during
    // this synchronous formatting transaction; restore read-only before return.
    SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    if (updateAppearance) {
      SendMessageW(edit, EM_SETBKGNDCOLOR, 0, rgb(appearance.background));
      CHARFORMAT2W format{};
      format.cbSize = sizeof(format);
      format.dwMask = CFM_COLOR | CFM_SIZE | CFM_FACE | CFM_BACKCOLOR;
      format.crTextColor = rgb(appearance.foreground);
      format.crBackColor = rgb(appearance.background);
      format.yHeight = static_cast<LONG>(std::lround(
          std::clamp(appearance.fontSize, 1.0f, 256.0f) * 20));
      const wchar_t face[] = L"Consolas";
      std::copy(std::begin(face), std::end(face), format.szFaceName);
      SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
      SendMessageW(edit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
    } else {
      for (const auto &range : paintedRanges) colorRange(range, rgb(appearance.background));
    }
    for (const auto &range : ranges) colorRange(range, highlightColor(appearance));
    paintedRanges.swap(nextPaintedRanges);
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    if (frozen) document->Unfreeze(&count);
    // TOM ranges never become the native selection (including its active end).
    SendMessageW(edit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    if (focus && !ranges.empty()) {
      ITextRange *range = nullptr;
      if (SUCCEEDED(document->Range(ranges.front().cpMin, ranges.front().cpMin, &range))) {
        range->ScrollIntoView(tomStart);
        range->Release();
      }
    }
    formatting = false;
    // SCF_ALL resets foreground too; a theme change must bypass window caching.
    paintSyntax(updateAppearance);
  }

  void contextMenu(LPARAM position) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    CHARRANGE selection{};
    SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    AppendMenuW(menu, MF_STRING | (selection.cpMin == selection.cpMax ? MF_GRAYED : 0),
        1, L"&Copy\tCtrl+C");
    AppendMenuW(menu, MF_STRING, 2, L"Select &All\tCtrl+A");
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (point.x == -1 && point.y == -1) {
      if (!GetCaretPos(&point)) point = {8, 8};
      ClientToScreen(edit, &point);
    }
    menuActive = true;
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, 0, edit, nullptr);
    menuActive = false;
    DestroyMenu(menu);
    if (retired || !edit) return;
    if (command == 1) SendMessageW(edit, WM_COPY, 0, 0);
    else if (command == 2) SendMessageW(edit, EM_SETSEL, 0, -1);
  }

  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
      UINT_PTR id, DWORD_PTR reference) {
    auto *owner = reinterpret_cast<std::shared_ptr<Control> *>(reference);
    const auto self = *owner;
    if (message == self->cancelMessage && lparam == reinterpret_cast<LPARAM>(self.get())) {
      if (self->retired && self->menuActive) EndMenu();
      if (self->retired && wparam) self->close();
      return 0;
    }
    if (message == WM_NCDESTROY) {
      if (self->menuActive) EndMenu();
      self->nativeFocus(false);
      if (self->document) { self->document->Release(); self->document = nullptr; }
      self->edit = nullptr;
      self->cancelTarget = nullptr;
      RemoveWindowSubclass(window, procedure, id);
      delete owner;
      return DefSubclassProc(window, message, wparam, lparam);
    }
    if (!self->retired) {
      // Paint also covers native TOM/accessibility navigation and selection
      // auto-scroll that need not arrive as an explicit WM_VSCROLL message.
      if (message == WM_PAINT) self->paintSyntax();
      if (message == WM_SETFOCUS) self->nativeFocus(true);
      else if (message == WM_KILLFOCUS) self->nativeFocus(false);
      else if (message == WM_KEYDOWN) {
        if (wparam == VK_ESCAPE) { self->state->requestDismiss(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && wparam == 'A') {
          SendMessageW(window, EM_SETSEL, 0, -1);
          return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && wparam == 'C') {
          SendMessageW(window, WM_COPY, 0, 0);
          return 0;
        }
      } else if (message == WM_CHAR && (wparam == 1 || wparam == 3 || wparam == VK_ESCAPE)) {
        return 0;
      } else if (message == WM_CONTEXTMENU) {
        self->contextMenu(lparam);
        return 0;
      }
    }
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    switch (message) {
    case WM_VSCROLL: case WM_HSCROLL: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_SIZE: case WM_KEYDOWN: case WM_LBUTTONUP: case WM_TIMER:
    case EM_SETSCROLLPOS: case EM_SCROLL: case EM_LINESCROLL: case EM_SCROLLCARET:
      self->paintSyntax();
      break;
    }
    return result;
  }

  bool create(const NativeText &text) {
    auto *window = SDL_GetKeyboardFocus();
    if (!window) {
      int count = 0;
      SDL_Window **windows = SDL_GetWindows(&count);
      if (windows && count == 1) window = windows[0];
      SDL_free(windows);
    }
    if (!window) return false;
    windowID = SDL_GetWindowID(window);
    parent = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!parent || GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId()) return false;
    // Keep the module loaded: its registered class and active/reentrant window
    // procedures must remain valid until Windows finishes destroying controls.
    static const HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit) return false;
    cancelMessage = RegisterWindowMessageW(L"AsoBMaShow.NativeDocument.Retire");
    if (!cancelMessage) return false;
    auto owner = std::make_unique<std::shared_ptr<Control>>(shared_from_this());
    edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_AUTOHSCROLL | ES_NOHIDESEL | ES_READONLY,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!edit) return false;
    if (!SetWindowSubclass(edit, procedure, 1, reinterpret_cast<DWORD_PTR>(owner.get()))) {
      DestroyWindow(edit); edit = nullptr; return false;
    }
    owner.release(); // WM_NCDESTROY releases the subclass's ownership.
    cancelTarget = edit;
    SendMessageW(edit, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(edit, EM_EXLIMITTEXT, 0, std::numeric_limits<LONG>::max());
    TextStream source{reinterpret_cast<const char *>(text.text.data()), text.text.size() * sizeof(wchar_t)};
    EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&source), 0, TextStream::read};
    // Explicit SF_TEXT prevents source beginning with "{\\rtf" being interpreted
    // as formatting; WM_SETTEXT/EM_SETTEXTEX auto-detect RTF.
    SendMessageW(edit, EM_STREAMIN, SF_TEXT | SF_UNICODE, reinterpret_cast<LPARAM>(&stream));
    if (stream.dwError) return false;
    length = static_cast<LONG>(text.text.size());
    GETTEXTLENGTHEX textLength{GTL_NUMCHARS | GTL_PRECISE, 1200};
    if (SendMessageW(edit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&textLength), 0) != length)
      return false; // Never paint incorrect offsets if the control normalized more text.
    IRichEditOle *richOle = nullptr;
    if (!SendMessageW(edit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&richOle))) return false;
    const HRESULT result = richOle->QueryInterface(IID_ITextDocument, reinterpret_cast<void **>(&document));
    richOle->Release();
    if (FAILED(result)) return false;
    paint(true, false);
    layout();
    ShowWindow(edit, SW_SHOWNOACTIVATE);
    SetFocus(edit);
    return true;
  }

  void close() {
    const HWND target = cancelTarget.load();
    if (!target) return;
    // SDL may execute callbacks immediately after event-system shutdown. The
    // surviving native subclass remains the cleanup owner in that case.
    if (GetWindowThreadProcessId(target, nullptr) != GetCurrentThreadId()) {
      PostMessageW(target, cancelMessage, 1, reinterpret_cast<LPARAM>(this));
      return;
    }
    retired = true;
    const bool restoreFocus = edit && GetFocus() == edit;
    if (menuActive) EndMenu();
    if (edit) DestroyWindow(edit);
    if (restoreFocus && IsWindow(parent)) SetFocus(parent);
  }
};

class WindowsSurface final : public Surface {
  std::string text_;
  NativeText nativeText_;
  std::shared_ptr<Control> control_;
public:
  WindowsSurface(const std::string &text, Frame frame, Appearance appearance)
      : text_(text), nativeText_(text), control_(std::make_shared<Control>(state_)) {
    control_->frame = frame;
    control_->appearance = appearance;
  }
  bool initialize() {
    return platform::onMain([&] { return control_->create(nativeText_); });
  }
  ~WindowsSurface() override {
    control_->retired = true;
    // TrackPopupMenu pumps Win32 messages, not SDL main-thread callbacks. End
    // that loop before waiting on SDL dispatch; the identity check also makes a
    // late message harmless if Windows has already reused the HWND.
    if (const HWND target = control_->cancelTarget.load())
      PostMessageW(target, control_->cancelMessage, 0, reinterpret_cast<LPARAM>(control_.get()));
    try {
      platform::onMain([control = control_] { control->close(); });
    } catch (...) {
      // Never destroy an HWND from the application worker. If synchronous SDL
      // dispatch has stopped, keep cleanup owned until a queued UI call or the
      // parent's WM_NCDESTROY can retire it.
      auto *pending = new (std::nothrow) std::shared_ptr<Control>(control_);
      if (!pending) return;
      if (!SDL_RunOnMainThread([](void *data) {
            std::unique_ptr<std::shared_ptr<Control>> owner(
                static_cast<std::shared_ptr<Control> *>(data));
            (*owner)->close();
          }, pending, false)) delete pending;
    }
  }
  void setFrame(Frame frame) override {
    platform::onMain([control = control_, frame] {
      if (!control->retired) { control->frame = frame; control->layout(); }
    });
  }
  void setAppearance(Appearance appearance) override {
    platform::onMain([control = control_, appearance] {
      if (!control->retired) { control->appearance = appearance; control->paint(true, false); }
    });
  }
  void setSyntax(std::shared_ptr<const bms_syntax::Document> syntax) override {
    auto mapped = nativeText_.syntaxPositions(std::move(syntax));
    platform::onMain([control = control_, syntax = std::move(mapped)] {
      if (control->retired) return;
      control->syntax = syntax;
      control->paintSyntax(true);
    });
  }
  void highlight(const std::vector<text_range::Range> &ranges, bool focus) override {
    std::vector<CHARRANGE> nativeRanges;
    for (const auto &range : utf16Ranges(text_, ranges)) {
      const CHARRANGE span{nativeText_.position(range.begin), nativeText_.position(range.end)};
      if (span.cpMin < span.cpMax) nativeRanges.push_back(span);
    }
    platform::onMain([control = control_, ranges = std::move(nativeRanges), focus]() mutable {
      if (control->retired) return;
      control->ranges = std::move(ranges);
      control->paint(false, focus);
    });
  }
  void scroll(Scroll action) override {
    platform::onMain([control = control_, action] {
      if (control->retired || !control->edit) return;
      WPARAM command = SB_LINEUP;
      switch (action) {
      case Scroll::LineUp: command = SB_LINEUP; break;
      case Scroll::LineDown: command = SB_LINEDOWN; break;
      case Scroll::PageUp: command = SB_PAGEUP; break;
      case Scroll::PageDown: command = SB_PAGEDOWN; break;
      case Scroll::Start: command = SB_TOP; break;
      case Scroll::End: command = SB_BOTTOM; break;
      }
      SendMessageW(control->edit, WM_VSCROLL, command, 0);
    });
  }
};
}

std::unique_ptr<Surface> detail::createWindows(const std::string &text, Frame frame,
    Appearance appearance) {
  try {
    auto surface = std::make_unique<WindowsSurface>(text, frame, appearance);
    if (surface->initialize()) return surface;
  } catch (const std::exception &error) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Native Windows document: %s", error.what());
  }
  return nullptr;
}
}
#endif
