#include "../targets.h"
#if TARGET_OS_IPHONE || TARGET_OS_OSX

#include "NativeDocument.h"
#include "IOSApplicationRuntime.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <limits>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

namespace {
using native_document::Frame;
using native_document::State;

template <typename F> auto onDocumentMain(F &&operation) -> std::invoke_result_t<F> {
#if TARGET_OS_IPHONE
  return platform::onMain(std::forward<F>(operation));
#else
  if (platform::isMainThread()) return operation();
  // NSTextView selection and NSMenu enter AppKit tracking loops. SDL's queued
  // main-thread callbacks are only serviced by SDL_PumpEvents, so waiting on
  // that queue here would stall the application worker until tracking ends.
  // Common run-loop modes also service these synchronous operations inside a
  // tracking loop, allowing destruction to cancel an open selection menu.
  using Result = std::invoke_result_t<F>;
  using StoredResult = std::conditional_t<std::is_void_v<Result>, bool, Result>;
  std::optional<StoredResult> result;
  __block std::exception_ptr failure;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  auto invoke = [&] {
    if constexpr (std::is_void_v<Result>) operation();
    else result.emplace(operation());
  };
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    @autoreleasepool {
      try { invoke(); } catch (...) { failure = std::current_exception(); }
    }
    dispatch_semaphore_signal(done);
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  if (failure) std::rethrow_exception(failure);
  if constexpr (!std::is_void_v<Result>) return std::move(*result);
#endif
}

// The contract has no window argument. Prefer SDL's focused window, and only
// fall back when there is exactly one SDL window (also useful for hidden tests).
SDL_Window *documentWindow() {
  if (auto *window = SDL_GetKeyboardFocus()) return window;
  if (auto *window = SDL_GetMouseFocus()) return window;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *window = count == 1 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

CGRect documentRect(Frame frame, CGRect bounds) {
  const auto unit = [](float value) -> CGFloat {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0;
  };
  const CGFloat x = unit(frame.x), y = unit(frame.y);
  return CGRectMake(bounds.origin.x + x * bounds.size.width,
      bounds.origin.y + y * bounds.size.height,
      std::min(unit(frame.width), 1 - x) * bounds.size.width,
      std::min(unit(frame.height), 1 - y) * bounds.size.height);
}

#if TARGET_OS_IPHONE
UIColor *nativeColor(std::uint32_t value) {
  return [UIColor colorWithRed:((value >> 24) & 255) / 255.0
                        green:((value >> 16) & 255) / 255.0
                         blue:((value >> 8) & 255) / 255.0 alpha:(value & 255) / 255.0];
}
UIResponder *firstResponderInView(UIView *view) {
  if (view.isFirstResponder) return view;
  for (UIView *child in view.subviews)
    if (UIResponder *responder = firstResponderInView(child)) return responder;
  return nil;
}
#else
NSColor *nativeColor(std::uint32_t value) {
  return [NSColor colorWithSRGBRed:((value >> 24) & 255) / 255.0
                           green:((value >> 16) & 255) / 255.0
                            blue:((value >> 8) & 255) / 255.0 alpha:(value & 255) / 255.0];
}
#endif
}

#if TARGET_OS_IPHONE
@interface AsoDocumentTextView : UITextView {
@public
  std::weak_ptr<State> dismissState;
}
@end

@implementation AsoDocumentTextView
- (NSArray<UIKeyCommand *> *)keyCommands {
  NSMutableArray<UIKeyCommand *> *commands = [[super keyCommands] mutableCopy];
  if (!commands) commands = [NSMutableArray array];
  UIKeyCommand *escape = [UIKeyCommand keyCommandWithInput:UIKeyInputEscape
      modifierFlags:0 action:@selector(dismissDocument:)];
  if (@available(iOS 15.0, *)) escape.wantsPriorityOverSystemBehavior = YES;
  [commands addObject:escape];
  return commands;
}
- (void)dismissDocument:(id)sender {
  if (const auto state = dismissState.lock()) state->requestDismiss();
}
@end

@interface AsoDocumentContainer : UIView {
@public
  Frame documentFrame;
}
@property(nonatomic, strong) AsoDocumentTextView *textView;
@end

@implementation AsoDocumentContainer
- (void)layoutSubviews {
  [super layoutSubviews];
  self.textView.frame = documentRect(documentFrame, self.bounds);
}
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
  UIView *hit = [super hitTest:point withEvent:event];
  return hit == self ? nil : hit;
}
@end
#else
@interface AsoDocumentTextView : NSTextView {
@public
  std::weak_ptr<State> dismissState;
}
@property(nonatomic, strong) NSMenu *selectionMenu;
@end

@implementation AsoDocumentTextView
- (NSMenu *)menuForEvent:(NSEvent *)event {
  self.selectionMenu = [super menuForEvent:event];
  return self.selectionMenu;
}
- (void)cancelOperation:(id)sender {
  if (const auto state = dismissState.lock()) state->requestDismiss();
}
- (BOOL)performKeyEquivalent:(NSEvent *)event {
  // SDL applications need not have an Edit menu. Keep these native text actions
  // available even when the main menu has no corresponding key equivalents.
  const auto modifiers = event.modifierFlags & (NSEventModifierFlagCommand |
      NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagShift);
  if (self.window.firstResponder == self && modifiers == NSEventModifierFlagCommand) {
    if ([event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"c"]) {
      [self copy:nil];
      return YES;
    }
    if ([event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"a"]) {
      [self selectAll:nil];
      return YES;
    }
  }
  return [super performKeyEquivalent:event];
}
@end

@interface AsoDocumentContainer : NSView {
@public
  Frame documentFrame;
}
@property(nonatomic, strong) AsoDocumentTextView *textView;
@property(nonatomic, strong) NSScrollView *scrollView;
@end

@implementation AsoDocumentContainer
- (BOOL)isFlipped { return YES; }
- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
  [super resizeSubviewsWithOldSize:oldSize];
  self.scrollView.frame = documentRect(documentFrame, self.bounds);
}
- (NSView *)hitTest:(NSPoint)point {
  NSView *hit = [super hitTest:point];
  return hit == self ? nil : hit;
}
@end
#endif

namespace native_document {
namespace {
class AppleSurface final : public Surface {
public:
  explicit AppleSurface(const std::string &text) : text_(text) {}
  ~AppleSurface() override {
    // All operations are synchronous worker -> UI calls. There are no pending
    // blocks retaining this Surface; native callbacks hold only a weak State.
    onDocumentMain([&] {
      @autoreleasepool {
        if (!container_) return;
        container_.textView->dismissState.reset();
#if TARGET_OS_IPHONE
        [container_.textView resignFirstResponder];
        container_.textView.selectedTextRange = nil;
        if (@available(iOS 16.0, *)) {
          for (id<UIInteraction> interaction in container_.textView.interactions) {
            if ([interaction isKindOfClass:[UIEditMenuInteraction class]])
              [(UIEditMenuInteraction *)interaction dismissMenu];
          }
        } else {
          [[UIMenuController sharedMenuController] hideMenu];
        }
        [container_ removeFromSuperview];
        // SDL's rendering UIView need not accept first responder. Restore its
        // actual prior responder (if any), otherwise leave UIKit's SDL window
        // responder chain active after resigning the document.
        [previousResponder_ becomeFirstResponder];
        previousResponder_ = nil;
#else
        [container_.textView.selectionMenu cancelTrackingWithoutAnimation];
        container_.textView.selectionMenu = nil;
        NSWindow *window = container_.window;
        NSView *host = container_.superview;
        [container_ removeFromSuperview];
        [window makeFirstResponder:host];
#endif
        container_ = nil; // Release AppKit/UIKit objects on their owning thread.
      }
    });
  }

  bool attach(Frame frame, Appearance appearance) {
    return onDocumentMain([&] {
      @autoreleasepool {
        SDL_Window *window = documentWindow();
        if (!window) return false;
        const auto properties = SDL_GetWindowProperties(window);
        NSString *text = [[NSString alloc] initWithBytes:text_.data() length:text_.size()
            encoding:NSUTF8StringEncoding];
        if (!text) return false;
#if TARGET_OS_IPHONE
        UIWindow *nativeWindow = (__bridge UIWindow *)SDL_GetPointerProperty(properties,
            SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
        UIView *host = nativeWindow.rootViewController.view;
        if (!host || host.window != nativeWindow) return false;
        previousResponder_ = firstResponderInView(nativeWindow);
        container_ = [[AsoDocumentContainer alloc] initWithFrame:host.bounds];
        container_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        container_.backgroundColor = UIColor.clearColor;
        AsoDocumentTextView *view = [[AsoDocumentTextView alloc] initWithFrame:CGRectZero];
        view.editable = NO; // Selectable read-only UITextView never opens a software keyboard.
        view.selectable = YES;
        view.scrollEnabled = YES;
        view.alwaysBounceVertical = YES;
        view.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
        view.textContainerInset = UIEdgeInsetsMake(8, 8, 8, 8);
        view.dataDetectorTypes = UIDataDetectorTypeNone;
        view.text = text;
        container_.textView = view;
        [container_ addSubview:view];
#else
        NSWindow *nativeWindow = (__bridge NSWindow *)SDL_GetPointerProperty(properties,
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        NSView *host = nativeWindow.contentView;
        if (!host) return false;
        container_ = [[AsoDocumentContainer alloc] initWithFrame:host.bounds];
        container_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        scroll.borderType = NSNoBorder;
        scroll.hasVerticalScroller = YES;
        scroll.hasHorizontalScroller = NO;
        scroll.autohidesScrollers = YES;
        AsoDocumentTextView *view = [[AsoDocumentTextView alloc] initWithFrame:NSZeroRect];
        view.editable = NO;
        view.selectable = YES;
        view.richText = NO;
        view.importsGraphics = NO;
        view.allowsUndo = NO;
        view.verticallyResizable = YES;
        view.horizontallyResizable = NO;
        view.minSize = NSMakeSize(0, 0);
        view.maxSize = NSMakeSize(std::numeric_limits<CGFloat>::max(), std::numeric_limits<CGFloat>::max());
        view.autoresizingMask = NSViewWidthSizable;
        view.textContainer.containerSize = NSMakeSize(0, std::numeric_limits<CGFloat>::max());
        view.textContainer.widthTracksTextView = YES;
        view.textContainerInset = NSMakeSize(8, 8);
        view.string = text;
        scroll.documentView = view;
        container_.textView = view;
        container_.scrollView = scroll;
        [container_ addSubview:scroll];
#endif
        view->dismissState = state_;
        [host addSubview:container_];
        setFrame(frame);
        setAppearance(appearance);
#if TARGET_OS_IPHONE
        [view becomeFirstResponder];
#else
        [nativeWindow makeFirstResponder:view];
#endif
        return true;
      }
    });
  }

  void setFrame(Frame frame) override {
    onDocumentMain([&] {
      container_->documentFrame = frame;
#if TARGET_OS_IPHONE
      [container_ setNeedsLayout];
      [container_ layoutIfNeeded];
#else
      [container_ resizeSubviewsWithOldSize:container_.bounds.size];
#endif
    });
  }

  void setAppearance(Appearance appearance) override {
    onDocumentMain([&] {
      appearance_ = appearance;
      const CGFloat fontSize = std::isfinite(appearance.fontSize)
          ? std::clamp(appearance.fontSize, 1.0f, 256.0f) : 16;
      auto *view = container_.textView;
#if TARGET_OS_IPHONE
      view.font = [UIFont monospacedSystemFontOfSize:fontSize weight:UIFontWeightRegular];
#else
      view.font = [NSFont monospacedSystemFontOfSize:fontSize weight:NSFontWeightRegular];
      container_.scrollView.backgroundColor = nativeColor(appearance.background);
#endif
      view.textColor = nativeColor(appearance.foreground);
      view.backgroundColor = nativeColor(appearance.background);
      paintHighlights();
    });
  }

  void highlight(const std::vector<text_range::Range> &ranges, bool focus) override {
    const auto converted = utf16Ranges(text_, ranges);
    onDocumentMain([&] {
      highlights_ = converted;
      paintHighlights();
      if (focus && !highlights_.empty()) focusCharacter(highlights_.front().begin);
    });
  }

  void scroll(Scroll action) override {
    onDocumentMain([&] {
      auto *view = container_.textView;
#if TARGET_OS_IPHONE
      [view layoutIfNeeded];
      CGPoint offset = view.contentOffset;
      const CGFloat page = view.bounds.size.height;
      const CGFloat end = std::max<CGFloat>(0, view.contentSize.height - page);
      const CGFloat line = view.font.lineHeight;
#else
      [view.layoutManager ensureLayoutForTextContainer:view.textContainer];
      NSClipView *clip = container_.scrollView.contentView;
      NSPoint offset = clip.bounds.origin;
      const CGFloat page = clip.bounds.size.height;
      const CGFloat end = std::max<CGFloat>(0, view.frame.size.height - page);
      const CGFloat line = [view.layoutManager defaultLineHeightForFont:view.font];
#endif
      switch (action) {
      case Scroll::LineUp: offset.y -= line; break;
      case Scroll::LineDown: offset.y += line; break;
      case Scroll::PageUp: offset.y -= page; break;
      case Scroll::PageDown: offset.y += page; break;
      case Scroll::Start: offset.y = 0; break;
      case Scroll::End: offset.y = end; break;
      }
      offset.y = std::clamp(offset.y, CGFloat(0), end);
#if TARGET_OS_IPHONE
      [view setContentOffset:offset animated:NO];
#else
      [clip scrollToPoint:offset];
      [container_.scrollView reflectScrolledClipView:clip];
#endif
    });
  }

private:
  void focusCharacter(std::size_t character) {
    auto *view = container_.textView;
    // Revealing an entire wrapped range can favor its end and UIKit may animate
    // that reveal. Anchor its first line explicitly after layout instead.
#if TARGET_OS_IPHONE
    [view layoutIfNeeded];
#endif
    auto *layout = view.layoutManager;
    [layout ensureLayoutForTextContainer:view.textContainer];
    const NSUInteger glyph = [layout glyphIndexForCharacterAtIndex:character];
    const CGRect line = [layout lineFragmentRectForGlyphAtIndex:glyph effectiveRange:nullptr];
#if TARGET_OS_IPHONE
    [view layoutIfNeeded];
    const CGFloat end = std::max<CGFloat>(0, view.contentSize.height - view.bounds.size.height);
    [view setContentOffset:CGPointMake(0, std::clamp(line.origin.y, CGFloat(0), end)) animated:NO];
#else
    NSClipView *clip = container_.scrollView.contentView;
    const CGFloat end = std::max<CGFloat>(0, view.frame.size.height - clip.bounds.size.height);
    const CGFloat top = line.origin.y + view.textContainerOrigin.y - view.textContainerInset.height;
    [clip scrollToPoint:NSMakePoint(0, std::clamp(top, CGFloat(0), end))];
    [container_.scrollView reflectScrolledClipView:clip];
#endif
  }

  void paintHighlights() {
    NSTextStorage *storage = container_.textView.textStorage;
    [storage beginEditing];
    [storage removeAttribute:NSBackgroundColorAttributeName range:NSMakeRange(0, storage.length)];
    for (const auto range : highlights_) {
      if (range.begin < range.end && range.end <= storage.length)
        [storage addAttribute:NSBackgroundColorAttributeName value:nativeColor(appearance_.highlight)
            range:NSMakeRange(range.begin, range.end - range.begin)];
    }
    [storage endEditing];
  }

  const std::string text_;
  // These fields are exclusively accessed on the native UI thread.
  AsoDocumentContainer *__strong container_ = nil;
#if TARGET_OS_IPHONE
  UIResponder *__weak previousResponder_ = nil;
#endif
  Appearance appearance_;
  std::vector<text_range::Range> highlights_;
};
}

std::unique_ptr<Surface> detail::createApple(const std::string &text, Frame frame, Appearance appearance) {
  auto surface = std::make_unique<AppleSurface>(text);
  if (!surface->attach(frame, appearance)) return nullptr;
  return surface;
}
}
#endif
