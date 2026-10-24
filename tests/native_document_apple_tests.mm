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
NSColor *syntaxColor(NSTextView *text, NSUInteger character) {
  return [text.layoutManager temporaryAttribute:NSForegroundColorAttributeName
      atCharacterIndex:character effectiveRange:nullptr];
}
std::pair<std::size_t, std::size_t> boundedSyntax(NSTextView *text) {
  std::size_t characters = 0, spans = 0;
  for (NSUInteger i = 0; i < text.string.length;) {
    NSRange effective{};
    id color = [text.layoutManager temporaryAttribute:NSForegroundColorAttributeName
        atCharacterIndex:i effectiveRange:&effective];
    require(effective.length != 0, "temporary attribute enumeration makes progress");
    if (color) { characters += effective.length; ++spans; }
    i = NSMaxRange(effective);
  }
  require(characters <= bms_syntax::maxPaintCharacters && spans <= bms_syntax::maxPaintSpans,
      "native syntax attributes stay bounded after scrolling");
  return {characters, spans};
}

void benchmarkSyntax(NSView *host) {
  std::string source;
  const std::string line = "#TITLE source\n#WAV01 sounds/01.ogg\n#00111:01000200030004000500060007000800\n";
  source.reserve(4 * 1024 * 1024 + line.size());
  while (source.size() < 4 * 1024 * 1024) source += line;
  const auto syntax = bms_syntax::tokenize(source);
  const auto createStart = std::chrono::steady_clock::now();
  auto surface = onWorker([&] { return native_document::detail::createApple(source, {0, 0, 1, 1}, {}); });
  const auto createEnd = std::chrono::steady_clock::now();
  NSTextView *text = findTextView(host);
  require(surface && text, "create large native syntax benchmark surface");
  const auto paintStart = std::chrono::steady_clock::now();
  onWorker([&] { surface->setSyntax(syntax); });
  const auto paintEnd = std::chrono::steady_clock::now();
  const auto retained = boundedSyntax(text);
  require(syntaxColor(text, 0) != nil, "large native document colors its visible text");
  std::printf("AppKit syntax: bytes=%zu cached_spans=%zu active_characters=%zu active_runs=%zu create_ms=%.2f syntax_attach_ms=%.2f\n",
      source.size(), syntax->spans.size(), retained.first, retained.second,
      std::chrono::duration<double, std::milli>(createEnd - createStart).count(),
      std::chrono::duration<double, std::milli>(paintEnd - paintStart).count());
  onWorker([&] { surface.reset(); });
}
}

int main(int argc, char **argv) {
  @autoreleasepool {
    require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("Native document regression", 640, 480, SDL_WINDOW_HIDDEN);
    require(window != nullptr, SDL_GetError());
    NSWindow *nativeWindow = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    require(nativeWindow != nil, "SDL exposes its own Cocoa window");
    NSView *host = nativeWindow.contentView;
    const NSUInteger initialChildren = host.subviews.count;
    std::string source = "#TITLE π😀\r\n#00111:0100000001\n#00211:0200\n";
    for (int i = 0; i < 100; ++i) source += "#00311:01000200\n";
    const auto wrapped = source.size();
    source += "#00411:" + std::string(2000, '1') + "\n";
    for (int i = 0; i < 2000; ++i) source += "#00511:01000200\n";
    source += "#LAST\n";
    const auto first = source.find("#001"), second = source.find("#002"), last = source.find("#LAST");
    native_document::Appearance appearance;
    auto surface = onWorker([&] {
      return native_document::detail::createApple(source, {0.1f, 0.2f, 0.8f, 0.6f}, appearance);
    });
    require(surface != nullptr, "create a native surface in the sole hidden SDL window");
    NSTextView *text = findTextView(host);
    require(text != nil && !text.editable && text.selectable, "native text is read-only and selectable");
    require(text.font != nil && std::abs(text.font.pointSize - appearance.fontSize) < 0.01,
        "initial source uses its final font before text layout");
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
    onWorker([&] { surface->setSyntax(bms_syntax::tokenize(source)); });
    require(syntaxColor(text, 0) != nil && syntaxColor(text, 7) != nil,
        "native directive and Unicode string have syntax colors");
    require(![syntaxColor(text, 0) isEqual:syntaxColor(text, 7)], "token categories have distinct colors");
    const NSUInteger data = [text.string rangeOfString:@"0100000001"].location;
    NSRange emptyRun{};
    NSColor *emptyColor = [[text.layoutManager temporaryAttribute:NSForegroundColorAttributeName
        atCharacterIndex:data + 2 effectiveRange:&emptyRun] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    require(emptyRun.location == data + 2 && emptyRun.length == 6,
        "consecutive 00 cells share one native foreground run");
    require(std::abs(emptyColor.redComponent - 0x37 / 255.0) < 0.01 &&
        ![syntaxColor(text, data) isEqual:syntaxColor(text, data + 2)], "empty cells use the muted palette");
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "syntax setup preserves native selection");
    boundedSyntax(text);
    onWorker([&] { surface->highlight({{first, first + 4}, {second, second + 4}}, false); });
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "practice highlight preserves selection");
    for (NSString *match in @[@"#001", @"#002"]) {
      const NSRange range = [text.string rangeOfString:match];
      require([text.textStorage attribute:NSBackgroundColorAttributeName atIndex:range.location
          effectiveRange:nullptr] != nil, "all UTF-16 practice ranges have background attributes");
    }
    require([text.textStorage attribute:NSBackgroundColorAttributeName atIndex:8 effectiveRange:nullptr] == nil,
        "native selection does not become a practice highlight");
    require(syntaxColor(text, 0) != nil, "practice background updates retain syntax foreground");
    NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
    require([text writeSelectionToPasteboard:pasteboard types:text.writablePasteboardTypes],
        "native selection can be copied");
    require([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"π😀"],
        "native copy preserves non-ASCII and supplementary Unicode characters");
    [pasteboard releaseGlobally];

    appearance.fontSize = 19;
    appearance.highlight = 0x12ab3480;
    appearance.syntax[static_cast<std::size_t>(bms_syntax::Kind::Directive)] = 0x16aa22ff;
    onWorker([&] { surface->setAppearance(appearance); });
    require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "theme updates preserve native selection");
    NSColor *color = [[text.textStorage attribute:NSBackgroundColorAttributeName
        atIndex:[text.string rangeOfString:@"#001"].location effectiveRange:nullptr]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    require(std::abs(color.redComponent - 0x12 / 255.0) < 0.01 &&
        std::abs(color.alphaComponent - 0x80 / 255.0) < 0.01, "appearance uses RRGGBBAA");
    NSColor *directive = [syntaxColor(text, 0) colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    require(directive != nil && std::abs(directive.redComponent - 0x16 / 255.0) < 0.01,
        "theme changes repaint cached syntax with the new palette");

    onWorker([&] { surface->scroll(native_document::Scroll::End); });
    require(scroll.contentView.bounds.origin.y > 0, "scroll-to-end moves the native viewport");
    require(syntaxColor(text, [text.string rangeOfString:@"#LAST"].location) != nil,
        "a distant scroll paints syntax around the new viewport");
    require(syntaxColor(text, 0) == nil, "scrolling retires offscreen syntax attributes");
    boundedSyntax(text);
    onWorker([&] { surface->scroll(native_document::Scroll::Start); });
    near(text.visibleRect.origin.y, 0, "scroll-to-start returns to the beginning");
    require(syntaxColor(text, 0) != nil, "scrolling back restores initial syntax colors");
    boundedSyntax(text);
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

    const auto checkJump = [&] {
      onWorker([&] { surface->highlight({{wrapped + 7, source.size()}}, true); });
      const NSUInteger character = [text.string rangeOfString:@"#00411:"].location + 7;
      const NSUInteger glyph = [text.layoutManager glyphIndexForCharacterAtIndex:character];
      const NSRect line = [text.layoutManager lineFragmentRectForGlyphAtIndex:glyph effectiveRange:nullptr];
      const CGFloat visibleY = line.origin.y + text.textContainerOrigin.y - text.visibleRect.origin.y;
      require(visibleY >= 0 && visibleY <= text.textContainerInset.height + 1,
          "highlight jumps immediately to the first wrapped match at the viewport top");
      require(NSEqualRanges(text.selectedRange, NSMakeRange(7, 3)), "direct jump preserves manual selection");
      require(syntaxColor(text, character) != nil, "direct jump colors the wrapped match");
      boundedSyntax(text);
    };
    checkJump();

    [scroll.contentView scrollToPoint:NSMakePoint(0, 150)];
    [scroll reflectScrolledClipView:scroll.contentView];
    checkJump(); // Repeat after native/manual scrolling, not only our scroll API.

    [nativeWindow setContentSize:NSMakeSize(800, 600)];
    near(scroll.frame.size.width, 640, "native resize reapplies normalized width without worker update");
    near(scroll.frame.origin.y, 120, "native resize preserves top-left position");
    onWorker([&] { surface->setFrame({0.25f, 0.1f, 0.5f, 0.8f}); }, true);
    near(scroll.frame.origin.x, 200, "worker frame updates use resized client bounds");
    near(scroll.frame.size.height, 480, "worker frame updates resize native viewport");
    checkJump();
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
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmarkSyntax(host);
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  std::puts("native_document_apple_tests passed");
}
