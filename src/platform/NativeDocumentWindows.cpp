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
  LONG length = 0;
  bool startedTextInput = false, menuActive = false;

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

  void colorRange(CHARRANGE span, COLORREF color) {
    ITextRange *range = nullptr;
    if (FAILED(document->Range(span.cpMin, span.cpMax, &range))) return;
    ITextFont *font = nullptr;
    if (SUCCEEDED(range->GetFont(&font))) {
      font->SetBackColor(static_cast<LONG>(color));
      font->Release();
    }
    range->Release();
  }

  void paint(bool updateAppearance, bool focus) {
    if (!edit || !document) return;
    auto nextPaintedRanges = ranges; // Allocate before changing read-only/freeze state.
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
    return DefSubclassProc(window, message, wparam, lparam);
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
