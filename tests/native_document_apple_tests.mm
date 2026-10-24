#include "platform/NativeDocument.h"
#include <SDL3/SDL.h>
#import <AppKit/AppKit.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

namespace {
void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

// Exercise the production worker -> SDL main-thread dispatch without a window
// on the desktop, arbitrary sleeps, or touching the user's general pasteboard.
template <typename F> auto onWorker(F operation, bool nativeTracking = false) {
  auto result = std::async(std::launch::async, std::move(operation));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    require(std::chrono::steady_clock::now() < deadline, "native UI dispatch must complete");
    if (nativeTracking)
      [[NSRunLoop mainRunLoop] runMode:NSEventTrackingRunLoopMode beforeDate:[NSDate date]];
    else
      SDL_PumpEvents();
    std::this_thread::yield();
  }
  return result.get();
}

NSTextView *findTextView(NSView *root) {
  if ([root isKindOfClass:[NSTextView class]]) return (NSTextView *)root;
  for (NSView *child in root.subviews)
    if (NSTextView *text = findTextView(child)) return text;
  return nil;
}

NSEvent *key(NSWindow *window, NSString *characters, unsigned short code,
    NSEventModifierFlags modifiers = 0) {
  return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
      modifierFlags:modifiers timestamp:NSProcessInfo.processInfo.systemUptime
      windowNumber:window.windowNumber context:nil characters:characters
      charactersIgnoringModifiers:characters isARepeat:NO keyCode:code];
}

void near(CGFloat actual, CGFloat expected, const char *message) {
  require(std::abs(actual - expected) < 1, message);
}
}

int main() {
  @autoreleasepool {
    require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("Native document regression", 640, 480, SDL_WINDOW_HIDDEN);
    require(window != nullptr, SDL_GetError());
    NSWindow *nativeWindow = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    require(nativeWindow != nil, "SDL exposes its own Cocoa window");
    NSView *host = nativeWindow.contentView;
    const NSUInteger initialChildren = host.subviews.count;
    std::string source = "#TITLE π😀\r\n#00111:0100\n#00211:0200\n";
    for (int i = 0; i < 100; ++i) source += "#00311:01000200\n";
    source += "#LAST\n";
    const auto first = source.find("#001"), second = source.find("#002"), last = source.find("#LAST");
    native_document::Appearance appearance;
    auto surface = onWorker([&] {
      return native_document::detail::createApple(source, {0.1f, 0.2f, 0.8f, 0.6f}, appearance);
    });
    require(surface != nullptr, "create a native surface in the sole hidden SDL window");
    NSTextView *text = findTextView(host);
    require(text != nil && !text.editable && text.selectable, "native text is read-only and selectable");
    require(nativeWindow.firstResponder == text, "native text owns keyboard focus");
    require([text.string isEqualToString:[NSString stringWithUTF8String:source.c_str()]],
        "Unicode, CRLF and source bytes are displayed without normalization");
    NSScrollView *scroll = text.enclosingScrollView;
    NSView *container = scroll.superview;
    require(container.isFlipped, "document geometry has a top-left origin");
    near(scroll.frame.origin.x, 64, "normalized left position");
    near(scroll.frame.origin.y, 96, "normalized top position");
    near(scroll.frame.size.width, 512, "normalized width");
    near(scroll.frame.size.height, 288, "normalized height");
    require([container hitTest:NSMakePoint(1, 1)] == nil, "toolbar hits pass through to SDL");
    require([container hitTest:[container convertPoint:NSMakePoint(80, 110) toView:host]] != nil,
        "document hits reach native controls");

    [text setSelectedRange:NSMakeRange(7, 3)]; // π + surrogate pair for 😀.
    onWorker([&] { surface->highlight({{first, first + 4}, {second, second + 4}}, false); });
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "practice highlight preserves selection");
    for (NSString *match in @[@"#001", @"#002"]) {
      const NSRange range = [text.string rangeOfString:match];
      require([text.textStorage attribute:NSBackgroundColorAttributeName atIndex:range.location
          effectiveRange:nullptr] != nil, "all UTF-16 practice ranges have background attributes");
    }
    require([text.textStorage attribute:NSBackgroundColorAttributeName atIndex:8 effectiveRange:nullptr] == nil,
        "native selection does not become a practice highlight");
    NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
    require([text writeSelectionToPasteboard:pasteboard types:text.writablePasteboardTypes],
        "native selection can be copied");
    require([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"π😀"],
        "native copy preserves non-ASCII and supplementary Unicode characters");
    [pasteboard releaseGlobally];

    appearance.fontSize = 19;
    appearance.highlight = 0x12ab3480;
    onWorker([&] { surface->setAppearance(appearance); });
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "theme updates preserve native selection");
    NSColor *color = [[text.textStorage attribute:NSBackgroundColorAttributeName
        atIndex:[text.string rangeOfString:@"#001"].location effectiveRange:nullptr]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    require(std::abs(color.redComponent - 0x12 / 255.0) < 0.01 &&
        std::abs(color.alphaComponent - 0x80 / 255.0) < 0.01, "appearance uses RRGGBBAA");

    onWorker([&] { surface->scroll(native_document::Scroll::End); });
    require(scroll.contentView.bounds.origin.y > 0, "scroll-to-end moves the native viewport");
    onWorker([&] { surface->scroll(native_document::Scroll::Start); });
    near(scroll.contentView.bounds.origin.y, 0, "scroll-to-start returns to the beginning");
    onWorker([&] { surface->highlight({{last, last + 5}}, true); });
    require(scroll.contentView.bounds.origin.y > 0, "focus scrolls to the first practice highlight");
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "focus scroll does not change selection");
    require([text.textStorage attribute:NSBackgroundColorAttributeName
        atIndex:[text.string rangeOfString:@"#001"].location effectiveRange:nullptr] == nil,
        "updating highlights removes old practice attributes");
    onWorker([&] { surface->highlight({}, false); });
    require([text.textStorage attribute:NSBackgroundColorAttributeName
        atIndex:[text.string rangeOfString:@"#LAST"].location effectiveRange:nullptr] == nil,
        "empty highlights clear practice attributes");

    [nativeWindow setContentSize:NSMakeSize(800, 600)];
    near(scroll.frame.size.width, 640, "native resize reapplies normalized width without worker update");
    near(scroll.frame.origin.y, 120, "native resize preserves top-left position");
    onWorker([&] { surface->setFrame({0.25f, 0.1f, 0.5f, 0.8f}); }, true);
    near(scroll.frame.origin.x, 200, "worker frame updates use resized client bounds");
    near(scroll.frame.size.height, 480, "worker frame updates resize native viewport");
    require([text performKeyEquivalent:key(nativeWindow, @"a", 0, NSEventModifierFlagCommand)],
        "Command-A works without an application Edit menu");
    require(text.selectedRange.length == text.string.length, "Command-A selects source text");
    [nativeWindow sendEvent:key(nativeWindow, @"x", 7)];
    require([text.string isEqualToString:[NSString stringWithUTF8String:source.c_str()]],
        "typing in the native first responder cannot edit source");
    [nativeWindow sendEvent:key(nativeWindow, @"\x1b", 53)];
    require(surface->dismissRequested(), "native Escape requests dismissal through shared State");

    __weak NSTextView *retiredText = text;
    text = nil;
    scroll = nil;
    container = nil;
    onWorker([&] { surface.reset(); }, true);
    require(host.subviews.count == initialChildren, "teardown removes the entire native surface");
    require(nativeWindow.firstResponder == host, "teardown restores the SDL content view responder");
    // AppKit may retain a recently used responder until its autorelease pool
    // drains. It must already be detached, with its dismissal callback retired.
    require(retiredText == nil || retiredText.superview == nil || retiredText.window == nil,
        "retained native selection objects cannot remain attached to the SDL window");
    require(!nativeWindow.visible, "the test never presents a desktop window");
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  std::puts("native_document_apple_tests passed");
}
