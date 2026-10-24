#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/NativeDocument.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <ole2.h>
#include <richole.h>
#include <tom.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

// Windows-only integration test. Link NativeDocument.cpp,
// NativeDocumentWindows.cpp, BmsSyntax.cpp, SDL3, user32, comctl32 and uuid.
namespace {
void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
template <typename F> auto onWorker(F operation) {
  auto result = std::async(std::launch::async, std::move(operation));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    require(std::chrono::steady_clock::now() < deadline, "native UI dispatch must complete");
    SDL_PumpEvents();
    std::this_thread::yield();
  }
  return result.get();
}
std::wstring contents(HWND edit) {
  GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
  const auto size = SendMessageW(edit, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
  std::wstring text(static_cast<std::size_t>(size) + 1, L'\0');
  GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
  const auto copied = SendMessageW(edit, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get),
      reinterpret_cast<LPARAM>(text.data()));
  text.resize(copied);
  return text;
}
LONG colorAt(ITextDocument *document, LONG position, bool foreground) {
  ITextRange *range = nullptr;
  require(SUCCEEDED(document->Range(position, position + 1, &range)), "obtain test range");
  ITextFont *font = nullptr;
  require(SUCCEEDED(range->GetFont(&font)), "obtain test font");
  LONG color = 0;
  require(SUCCEEDED(foreground ? font->GetForeColor(&color) : font->GetBackColor(&color)),
      "read native text color");
  font->Release();
  range->Release();
  return color;
}
LONG background(ITextDocument *document, LONG position) { return colorAt(document, position, false); }
LONG foreground(ITextDocument *document, LONG position) { return colorAt(document, position, true); }
COLORREF rgb(std::uint32_t color) {
  return RGB((color >> 24) & 255, (color >> 16) & 255, (color >> 8) & 255);
}
LRESULT CALLBACK countFormatting(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR reference) {
  if (message == EM_SETREADONLY && !wparam) ++*reinterpret_cast<unsigned *>(reference);
  return DefSubclassProc(window, message, wparam, lparam);
}
// Inspect actual TOM format runs, including retired regions. This stays cheap
// when the backend clears old attributes, rather than reading every character.
void requireBoundedColors(ITextDocument *document, LONG length, COLORREF plain) {
  ITextRange *range = nullptr;
  require(SUCCEEDED(document->Range(0, 0, &range)), "obtain format-run inspection range");
  std::size_t runs = 0, coloredRuns = 0, coloredCharacters = 0;
  for (LONG position = 0; position < length;) {
    require(SUCCEEDED(range->SetRange(position, position + 1)), "select format-run character");
    require(SUCCEEDED(range->EndOf(tomCharFormat, tomExtend, nullptr)), "find format-run end");
    LONG end = 0;
    require(SUCCEEDED(range->GetEnd(&end)) && end > position, "format-run inspection advances");
    end = std::min(end, length);
    if (foreground(document, position) != static_cast<LONG>(plain)) {
      ++coloredRuns;
      coloredCharacters += static_cast<std::size_t>(end - position);
    }
    ++runs;
    position = end;
  }
  range->Release();
  require(coloredCharacters > 0 && coloredCharacters <= bms_syntax::maxPaintCharacters,
      "aggregate retained colored characters stay within the paint cap");
  require(coloredRuns <= bms_syntax::maxPaintSpans,
      "retained colored runs stay within the syntax span cap");
  require(runs <= bms_syntax::maxPaintSpans * 2 + 1,
      "retired foreground attributes coalesce instead of accumulating");
}

void checkLargeSyntax(HWND parent, bool longLine) {
  std::string source;
  auto syntax = std::make_shared<bms_syntax::Document>();
  if (longLine) {
    source.assign(bms_syntax::maxPaintCharacters * 4, 'A');
    syntax->spans.push_back({0, static_cast<std::uint32_t>(source.size()), bms_syntax::Kind::Data});
  } else {
    // Many CRLF collapses and adjacent spans exercise the linear endpoint merge.
    // The whole fixture has far more spans than may be installed on the HWND.
    for (std::uint32_t i = 0; i < 12000; ++i) {
      const auto offset = static_cast<std::uint32_t>(source.size());
      source += "#X\r\n";
      syntax->spans.push_back({offset, offset + 1, bms_syntax::Kind::Directive});
      syntax->spans.push_back({offset + 1, offset + 2, bms_syntax::Kind::Identifier});
    }
  }
  syntax->length = static_cast<std::uint32_t>(source.size());
  native_document::Appearance appearance;
  auto surface = onWorker([&] {
    auto result = native_document::detail::createWindows(source, {0, 0, 1, 1}, appearance);
    if (result) result->setSyntax(syntax);
    return result;
  });
  require(surface != nullptr, "create bounded syntax fixture");
  const HWND edit = FindWindowExW(parent, nullptr, MSFTEDIT_CLASS, nullptr);
  require(edit != nullptr, "find bounded syntax fixture control");
  const auto original = contents(edit);
  const LONG length = static_cast<LONG>(original.size());
  IRichEditOle *richOle = nullptr;
  require(SendMessageW(edit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&richOle)) != 0,
      "obtain bounded fixture object model");
  ITextDocument *document = nullptr;
  require(SUCCEEDED(richOle->QueryInterface(IID_ITextDocument, reinterpret_cast<void **>(&document))),
      "obtain bounded fixture document");
  richOle->Release();
  requireBoundedColors(document, length, rgb(appearance.foreground));
  CHARRANGE selected{1, 2};
  SendMessageW(edit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
  if (longLine) {
    require(foreground(document, 0) == static_cast<LONG>(rgb(appearance.syntax[
        static_cast<std::size_t>(bms_syntax::Kind::Data)])), "long data token starts colored");
    require(foreground(document, length - 1) == static_cast<LONG>(rgb(appearance.foreground)),
        "long data token is clipped instead of formatted in full");
    SendMessageW(edit, WM_HSCROLL, SB_RIGHT, 0);
    require(foreground(document, length - 1) == static_cast<LONG>(rgb(appearance.syntax[
        static_cast<std::size_t>(bms_syntax::Kind::Data)])), "horizontal scroll paints the data tail");
    require(foreground(document, 0) == static_cast<LONG>(rgb(appearance.foreground)),
        "horizontal scroll clears retired data foreground");
    requireBoundedColors(document, length, rgb(appearance.foreground));
  } else {
    unsigned transactions = 0;
    require(SetWindowSubclass(edit, countFormatting, 2, reinterpret_cast<DWORD_PTR>(&transactions)),
        "observe formatting transactions");
    SendMessageW(edit, WM_PAINT, 0, 0);
    require(transactions == 0, "unchanged visible row segments do no native formatting");
    SendMessageW(edit, WM_VSCROLL, SB_LINEDOWN, 0);
    require(transactions > 0, "scrolling paints newly visible row segments");
    RemoveWindowSubclass(edit, countFormatting, 2);
    for (const LONG line : {0, 3000, 6000, 9000, 11000, 6000, 0}) {
      const auto current = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
      SendMessageW(edit, EM_LINESCROLL, 0, line - current);
      const auto visible = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
      require(visible == line, "native line scrolling reaches the requested viewport");
      require(foreground(document, line * 3) == static_cast<LONG>(rgb(appearance.syntax[
          static_cast<std::size_t>(bms_syntax::Kind::Directive)])),
          "cached syntax positions stay correct near the end of a CRLF document");
      if (line > 0)
        require(foreground(document, 0) == static_cast<LONG>(rgb(appearance.foreground)),
            "retired foreground returns to the appearance default");
      requireBoundedColors(document, length, rgb(appearance.foreground));
    }
  }
  CHARRANGE actual{};
  SendMessageW(edit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&actual));
  require(actual.cpMin == selected.cpMin && actual.cpMax == selected.cpMax,
      "repeated syntax window replacement does not move the native selection");
  require(contents(edit) == original, "syntax window updates preserve all source text");
  require((GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) != 0,
      "syntax window updates restore read-only state");
  // Native keyboard navigation and a resize must also leave bounded attributes.
  SendMessageW(edit, WM_KEYDOWN, VK_NEXT, 0);
  onWorker([&] { surface->setFrame({0, 0, 0.5f, 0.5f}); });
  requireBoundedColors(document, length, rgb(appearance.foreground));
  onWorker([&] { surface->setSyntax(nullptr); });
  ITextRange *whole = nullptr;
  require(SUCCEEDED(document->Range(0, length, &whole)), "inspect cleared syntax document");
  ITextFont *font = nullptr;
  require(SUCCEEDED(whole->GetFont(&font)), "inspect cleared syntax font");
  LONG color = 0;
  require(SUCCEEDED(font->GetForeColor(&color)) && color == static_cast<LONG>(rgb(appearance.foreground)),
      "removing syntax clears every retained foreground run");
  font->Release();
  whole->Release();
  document->Release();
  onWorker([&] { surface.reset(); });
}

void checkMixedSyntaxRows(HWND parent) {
  std::string source = "#00111:" + std::string(65536, 'A') + "\r\n#TITLE short\r\n\r\n#BPM 120\r\n";
  for (int i = 0; i < 80; ++i) source += "#BPM 120\r\n";
  source += "#TITLE EOF";
  native_document::Appearance appearance;
  appearance.highlight = 0xff0000ff;
  auto surface = onWorker([&] {
    auto result = native_document::detail::createWindows(source, {0, 0, 1, 1}, appearance);
    if (result) result->setSyntax(bms_syntax::tokenize(source));
    return result;
  });
  require(surface != nullptr, "create mixed long/short row fixture");
  const HWND edit = FindWindowExW(parent, nullptr, MSFTEDIT_CLASS, nullptr);
  require(edit != nullptr, "find mixed row fixture control");
  const auto original = contents(edit);
  const LONG length = static_cast<LONG>(original.size());
  const LONG title = static_cast<LONG>(original.find(L"#TITLE short"));
  const LONG bpm = static_cast<LONG>(original.find(L"#BPM"));
  const LONG eof = static_cast<LONG>(original.find(L"#TITLE EOF"));
  IRichEditOle *richOle = nullptr;
  require(SendMessageW(edit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&richOle)) != 0,
      "obtain mixed row object model");
  ITextDocument *document = nullptr;
  require(SUCCEEDED(richOle->QueryInterface(IID_ITextDocument, reinterpret_cast<void **>(&document))),
      "obtain mixed row document");
  richOle->Release();
  const auto requireDirective = [&](LONG position) {
    require(foreground(document, position) == static_cast<LONG>(rgb(appearance.syntax[
        static_cast<std::size_t>(bms_syntax::Kind::Directive)])),
        "short visible rows after huge nonwrapping data must receive syntax color");
  };
  requireDirective(title);
  requireDirective(bpm);
  requireBoundedColors(document, length, rgb(appearance.foreground));
  require(SendMessageW(edit, EM_GETLINECOUNT, 0, 0) == 85,
      "huge first row stays unwrapped");
  ITextSelection *selection = nullptr;
  require(SUCCEEDED(document->GetSelection(&selection)), "obtain mixed row selection");
  selection->SetRange(title + 1, title + 4);
  selection->SetFlags(tomSelStartActive);
  LONG savedFlags = 0;
  selection->GetFlags(&savedFlags);
  const auto sourceTitle = source.find("#TITLE short");
  onWorker([&] { surface->highlight({{sourceTitle, sourceTitle + 6}}, false); });
  const auto requirePreserved = [&] {
    LONG start = 0, end = 0, flags = 0;
    selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
    require(start == title + 1 && end == title + 4 && flags == savedFlags,
        "row segment replacement preserves selection and active end");
    require(background(document, title) == RGB(255, 0, 0),
        "row foreground replacement preserves practice background");
    requireBoundedColors(document, length, rgb(appearance.foreground));
  };
  for (int i = 0; i < 3; ++i) {
    SendMessageW(edit, WM_HSCROLL, SB_RIGHT, 0);
    require(foreground(document, 65542) == static_cast<LONG>(rgb(appearance.syntax[
        static_cast<std::size_t>(bms_syntax::Kind::Data)])), "mixed viewport paints long row tail");
    require(foreground(document, title) == static_cast<LONG>(rgb(appearance.foreground)),
        "short rows entirely left of the viewport retire their foreground");
    require(foreground(document, 32768) == static_cast<LONG>(rgb(appearance.foreground)),
        "hidden long row middle never consumes retained foreground budget");
    requirePreserved();
    SendMessageW(edit, WM_HSCROLL, SB_LEFT, 0);
    requireDirective(title);
    requireDirective(bpm);
    requirePreserved();
  }
  // A pixel scroll clips both the top and bottom rows; use native coordinates
  // only in this small vertical range (not the long row's horizontal offset).
  POINT scroll{0, 5};
  SendMessageW(edit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  requireDirective(title);
  requireDirective(bpm);
  requirePreserved();
  unsigned transactions = 0;
  require(SetWindowSubclass(edit, countFormatting, 2, reinterpret_cast<DWORD_PTR>(&transactions)),
      "observe unchanged mixed viewport formatting");
  SendMessageW(edit, WM_TIMER, 0, 0); // Selection drag timers also enter the refresh path.
  SendMessageW(edit, WM_PAINT, 0, 0);
  require(transactions == 0, "identical row segments reuse the native foreground cache");
  RemoveWindowSubclass(edit, countFormatting, 2);
  onWorker([&] { surface->setFrame({0, 0, 0.5f, 0.5f}); });
  appearance.foreground = 0x151515ff;
  appearance.syntax[static_cast<std::size_t>(bms_syntax::Kind::Directive)] = 0x174cc9ff;
  onWorker([&] { surface->setAppearance(appearance); });
  requireDirective(title);
  requireDirective(bpm);
  requirePreserved();
  onWorker([&] { surface->scroll(native_document::Scroll::End); });
  requireDirective(eof);
  requirePreserved();
  ITextRange *target = nullptr;
  require(SUCCEEDED(document->Range(title, title, &target)), "obtain native navigation target");
  target->ScrollIntoView(tomStart);
  target->Release();
  SendMessageW(edit, WM_TIMER, 0, 0);
  requireDirective(title);
  requireDirective(bpm);
  requirePreserved();
  require(contents(edit) == original, "mixed row paint never changes source text");
  require((GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) != 0,
      "mixed row paint restores read-only state");
  onWorker([&] { surface->setSyntax(nullptr); });
  ITextRange *whole = nullptr;
  require(SUCCEEDED(document->Range(0, length, &whole)), "inspect all cleared row segments");
  ITextFont *font = nullptr;
  require(SUCCEEDED(whole->GetFont(&font)), "obtain cleared mixed row font");
  LONG color = 0;
  require(SUCCEEDED(font->GetForeColor(&color)) && color == static_cast<LONG>(rgb(appearance.foreground)),
      "removing syntax clears all disjoint retained row segments");
  require(background(document, title) == RGB(255, 0, 0),
      "removing row syntax preserves practice background");
  font->Release();
  whole->Release();
  selection->Release();
  document->Release();
  onWorker([&] { surface.reset(); });
}
}

int main() {
  require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
  SDL_Window *window = SDL_CreateWindow("Native Windows document regression", 640, 480, SDL_WINDOW_HIDDEN);
  require(window != nullptr, SDL_GetError());
  const HWND parent = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  require(parent != nullptr, "SDL exposes its HWND");
  const bool originalTextInput = SDL_TextInputActive(window);
  // RTF-looking text must remain literal; CRLF after a surrogate pair must not
  // shift practice ranges. No system clipboard reads/writes are needed.
  std::string source = "{\\rtf1 literal}\n\xED\x95\x9C\xEA\xB8\x80\xF0\x9F\x98\x80\r\n#FIRST\n#SECOND\n";
  for (int i = 0; i < 100; ++i) source += "#00311:01000200\n";
  source += "#LAST\n";
  const auto first = source.find("#FIRST"), second = source.find("#SECOND"), last = source.find("#LAST");
  native_document::Appearance appearance;
  appearance.highlight = 0xff0000ff;
  auto surface = onWorker([&] {
    return native_document::detail::createWindows(source, {0.1f, 0.2f, 0.8f, 0.6f}, appearance);
  });
  require(surface != nullptr, "create Rich Edit under the sole SDL window");
  const HWND edit = FindWindowExW(parent, nullptr, MSFTEDIT_CLASS, nullptr);
  require(edit != nullptr, "native document is an embedded modern Rich Edit");
  require((GetWindowLongPtrW(edit, GWL_STYLE) & ES_READONLY) != 0, "document is read-only");
  const auto text = contents(edit);
  require(text.find(L"{\\rtf1 literal}\r\uD55C\uAE00\U0001F600\r#FIRST") == 0,
      "Unicode and literal markup survive paragraph normalization");
  RECT client{}, rectangle{};
  GetClientRect(parent, &client);
  GetWindowRect(edit, &rectangle);
  MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&rectangle), 2);
  require(std::abs(rectangle.left - client.right / 10) <= 1, "normalized left position");
  require(std::abs(rectangle.top - client.bottom / 5) <= 1, "normalized top position");

  IRichEditOle *richOle = nullptr;
  require(SendMessageW(edit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&richOle)) != 0,
      "obtain native text object model");
  ITextDocument *document = nullptr;
  require(SUCCEEDED(richOle->QueryInterface(IID_ITextDocument, reinterpret_cast<void **>(&document))),
      "obtain native document");
  richOle->Release();
  ITextSelection *selection = nullptr;
  require(SUCCEEDED(document->GetSelection(&selection)), "obtain native selection");
  selection->SetRange(2, 8);
  selection->SetFlags(tomSelStartActive);
  LONG originalFlags = 0;
  selection->GetFlags(&originalFlags);
  auto syntax = std::make_shared<bms_syntax::Document>();
  syntax->length = static_cast<std::uint32_t>(
      native_document::utf16Ranges(source, {{0, source.size()}}).front().end);
  const auto addSpan = [&](std::size_t begin, std::size_t end, bms_syntax::Kind kind) {
    const auto range = native_document::utf16Ranges(source, {{begin, end}}).front();
    syntax->spans.push_back({static_cast<std::uint32_t>(range.begin),
        static_cast<std::uint32_t>(range.end), kind});
  };
  addSpan(0, source.find('\n'), bms_syntax::Kind::Comment);
  addSpan(source.find('\n') + 1, first - 2, bms_syntax::Kind::String);
  addSpan(first, first + 6, bms_syntax::Kind::Directive);
  addSpan(second, second + 7, bms_syntax::Kind::Control);
  const auto channel = source.find("#00311");
  addSpan(channel + 1, channel + 4, bms_syntax::Kind::Number);
  addSpan(channel + 4, channel + 6, bms_syntax::Kind::Identifier);
  addSpan(channel + 7, channel + 15, bms_syntax::Kind::Data);
  addSpan(last, last + 5, bms_syntax::Kind::Directive);
  onWorker([&] { surface->setSyntax(syntax); });
  const LONG nativeFirst = static_cast<LONG>(text.find(L"#FIRST"));
  const LONG nativeSecond = static_cast<LONG>(text.find(L"#SECOND"));
  const LONG nativeChannel = static_cast<LONG>(text.find(L"#00311"));
  const auto requireColor = [&](LONG position, bms_syntax::Kind kind) {
    require(foreground(document, position) == static_cast<LONG>(rgb(appearance.syntax[
        static_cast<std::size_t>(kind)])), "syntax kind uses the current palette at its native offset");
  };
  requireColor(0, bms_syntax::Kind::Comment);
  requireColor(nativeFirst - 3, bms_syntax::Kind::String); // Both emoji surrogate code units.
  requireColor(nativeFirst - 2, bms_syntax::Kind::String);
  requireColor(nativeFirst, bms_syntax::Kind::Directive);
  requireColor(nativeSecond, bms_syntax::Kind::Control);
  requireColor(nativeChannel + 1, bms_syntax::Kind::Number);
  requireColor(nativeChannel + 4, bms_syntax::Kind::Identifier);
  requireColor(nativeChannel + 7, bms_syntax::Kind::Data);
  requireColor(nativeChannel + 9, bms_syntax::Kind::EmptyData);
  requireColor(nativeChannel + 10, bms_syntax::Kind::EmptyData);
  requireColor(nativeChannel + 11, bms_syntax::Kind::Data);
  require(foreground(document, nativeFirst - 1) == static_cast<LONG>(rgb(appearance.foreground)),
      "collapsed CRLF separator retains the plain foreground");
  onWorker([&] { surface->highlight({{first, first + 6}, {second, second + 7}}, false); });
  requireColor(nativeFirst, bms_syntax::Kind::Directive);
  require(background(document, static_cast<LONG>(text.find(L"#FIRST"))) == RGB(255, 0, 0),
      "highlight after CRLF and supplementary character uses the right offset");
  require(background(document, static_cast<LONG>(text.find(L"#SECOND"))) == RGB(255, 0, 0),
      "all independent ranges are painted");
  LONG start = 0, end = 0, flags = 0;
  selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
  require(start == 2 && end == 8 && flags == originalFlags,
      "syntax and practice paint preserve native selection and its active end");
  onWorker([&] { surface->highlight({{last, last + 5}}, true); });
  POINT scroll{};
  SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  require(scroll.y > 0, "focus scrolls to the first practice match");
  SendMessageW(edit, WM_VSCROLL, SB_TOP, 0);
  onWorker([&] { surface->highlight({{last, last + 5}}, true); });
  SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  require(scroll.y > 0, "highlight refocuses after native/manual scrolling");
  requireColor(static_cast<LONG>(text.find(L"#LAST")), bms_syntax::Kind::Directive);
  require(background(document, static_cast<LONG>(text.find(L"#FIRST"))) != RGB(255, 0, 0),
      "old practice ranges are cleared");
  selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
  require(start == 2 && end == 8 && flags == originalFlags,
      "focus scroll does not replace native selection");
  appearance.background = 0xeeeeeeff;
  appearance.foreground = 0x151515ff;
  appearance.syntax[static_cast<std::size_t>(bms_syntax::Kind::Directive)] = 0x174cc9ff;
  onWorker([&] { surface->setAppearance(appearance); });
  requireColor(static_cast<LONG>(text.find(L"#LAST")), bms_syntax::Kind::Directive);
  onWorker([&] { surface->scroll(native_document::Scroll::Start); });
  requireColor(nativeFirst, bms_syntax::Kind::Directive);
  require(background(document, static_cast<LONG>(text.find(L"#LAST"))) == RGB(255, 0, 0),
      "appearance changes retain practice highlighting");
  onWorker([&] { surface->setFrame({0, 0, 0.5f, 0.4f}); });
  requireColor(nativeFirst, bms_syntax::Kind::Directive);
  selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
  require(start == 2 && end == 8 && flags == originalFlags,
      "theme, viewport and resize painting preserve selection direction");
  SendMessageW(edit, WM_CHAR, 'X', 0);
  require(contents(edit) == text, "typing cannot modify the document");
  SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
  require(surface->dismissRequested(), "Escape requests dismissal through owned state");
  selection->Release();
  document->Release();
  onWorker([&] { surface.reset(); });
  require(!IsWindow(edit), "worker destruction removes the native control");
  require(SDL_TextInputActive(window) == originalTextInput, "SDL text-input state is restored");

  checkLargeSyntax(parent, false);
  checkLargeSyntax(parent, true);
  checkMixedSyntaxRows(parent);

  // Destruction of the native parent first must retire subclass state safely.
  surface = onWorker([&] { return native_document::detail::createWindows("", {0, 0, 1, 1}, {}); });
  require(surface != nullptr, "empty native documents are valid");
  SDL_DestroyWindow(window);
  onWorker([&] { surface.reset(); });
  SDL_Quit();
}
