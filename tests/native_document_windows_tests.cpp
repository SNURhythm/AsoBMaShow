#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/NativeDocument.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <richedit.h>
#include <ole2.h>
#include <richole.h>
#include <tom.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

// Windows-only integration test. Link NativeDocument.cpp,
// NativeDocumentWindows.cpp, SDL3, user32, comctl32 and uuid.
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
LONG background(ITextDocument *document, LONG position) {
  ITextRange *range = nullptr;
  require(SUCCEEDED(document->Range(position, position + 1, &range)), "obtain test range");
  ITextFont *font = nullptr;
  require(SUCCEEDED(range->GetFont(&font)), "obtain test font");
  LONG color = 0;
  require(SUCCEEDED(font->GetBackColor(&color)), "read practice background");
  font->Release();
  range->Release();
  return color;
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
  onWorker([&] { surface->highlight({{first, first + 6}, {second, second + 7}}, false); });
  require(background(document, static_cast<LONG>(text.find(L"#FIRST"))) == RGB(255, 0, 0),
      "highlight after CRLF and supplementary character uses the right offset");
  require(background(document, static_cast<LONG>(text.find(L"#SECOND"))) == RGB(255, 0, 0),
      "all independent ranges are painted");
  LONG start = 0, end = 0, flags = 0;
  selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
  require(start == 2 && end == 8 && flags == originalFlags,
      "practice paint preserves native selection and its active end");
  onWorker([&] { surface->highlight({{last, last + 5}}, true); });
  POINT scroll{};
  SendMessageW(edit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  require(scroll.y > 0, "focus scrolls to the first practice match");
  require(background(document, static_cast<LONG>(text.find(L"#FIRST"))) != RGB(255, 0, 0),
      "old practice ranges are cleared");
  selection->GetStart(&start); selection->GetEnd(&end); selection->GetFlags(&flags);
  require(start == 2 && end == 8 && flags == originalFlags,
      "focus scroll does not replace native selection");
  appearance.background = 0xeeeeeeff;
  onWorker([&] { surface->setAppearance(appearance); surface->scroll(native_document::Scroll::Start); });
  require(background(document, static_cast<LONG>(text.find(L"#LAST"))) == RGB(255, 0, 0),
      "appearance changes retain practice highlighting");
  SendMessageW(edit, WM_CHAR, 'X', 0);
  require(contents(edit) == text, "typing cannot modify the document");
  SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
  require(surface->dismissRequested(), "Escape requests dismissal through owned state");
  selection->Release();
  document->Release();
  onWorker([&] { surface.reset(); });
  require(!IsWindow(edit), "worker destruction removes the native control");
  require(SDL_TextInputActive(window) == originalTextInput, "SDL text-input state is restored");

  // Destruction of the native parent first must retire subclass state safely.
  surface = onWorker([&] { return native_document::detail::createWindows("", {0, 0, 1, 1}, {}); });
  require(surface != nullptr, "empty native documents are valid");
  SDL_DestroyWindow(window);
  onWorker([&] { surface.reset(); });
  SDL_Quit();
}
