#include "platform/NativeDocument.h"
#include <SDL3/SDL.h>
#import <UIKit/UIKit.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

// Host only the production native document backend; no renderer, chart library,
// signing credentials or app data are needed. SDL window lookup and the worker
// dispatcher are the only substitutes. UIKit/TextKit and all painting are real.
static UIWindow *testWindow;
extern "C" SDL_Window *SDL_GetKeyboardFocus() { return reinterpret_cast<SDL_Window *>(1); }
extern "C" SDL_Window *SDL_GetMouseFocus() { return nullptr; }
extern "C" SDL_Window **SDL_GetWindows(int *count) { *count = 0; return nullptr; }
extern "C" void SDL_free(void *) {}
extern "C" SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *) { return 1; }
extern "C" void *SDL_GetPointerProperty(SDL_PropertiesID, const char *, void *) {
  return (__bridge void *)testWindow;
}
void RunIOSMainThread(std::function<void()> operation) {
  if ([NSThread isMainThread]) operation();
  else dispatch_sync(dispatch_get_main_queue(), ^{ operation(); });
}
bool IsIOSMainThread() { return [NSThread isMainThread]; }
namespace ui { void notifyCompletion() {} }

namespace {
std::unique_ptr<native_document::Surface> surface;
std::string source;
std::size_t target;
CGFloat focusedY;

void require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
UITextView *findText(UIView *root) {
  if ([root isKindOfClass:UITextView.class]) return (UITextView *)root;
  for (UIView *child in root.subviews)
    if (UITextView *text = findText(child)) return text;
  return nil;
}
void checkFocus(UITextView *view) {
  [view layoutIfNeeded];
  auto *layout = view.layoutManager;
  const auto glyph = [layout glyphIndexForCharacterAtIndex:target];
  const CGRect line = [layout lineFragmentRectForGlyphAtIndex:glyph effectiveRange:nullptr];
  require(std::abs(view.contentOffset.y - line.origin.y) < 1,
      "highlight begins at the viewport top after native layout");
  require(NSEqualRanges(view.selectedRange, NSMakeRange(7, 2)), "focus preserves selection");
  require([view.textStorage attribute:NSBackgroundColorAttributeName atIndex:target effectiveRange:nullptr] != nil,
      "practice background remains attached to the source");
}
void checkSyntax(UITextView *view) {
  __block std::size_t colored = 0, runs = 0;
  [view.textStorage enumerateAttribute:NSForegroundColorAttributeName
      inRange:NSMakeRange(0, view.textStorage.length) options:0
      usingBlock:^(id value, NSRange range, BOOL *) {
    CGFloat r = 1, g = 1, b = 1, a = 1;
    if (value) [(UIColor *)value getRed:&r green:&g blue:&b alpha:&a];
    if (r < 0.99 || g < 0.99 || b < 0.99) { colored += range.length; ++runs; }
  }];
  require(colored > 0 && colored <= bms_syntax::maxPaintCharacters && runs <= bms_syntax::maxPaintSpans,
      "UIKit syntax runs remain bounded after distant jumps");
}
void jump() { surface->highlight({{target, target + 20}}, true); }
}

@interface NativeDocumentTestScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) CADisplayLink *frames;
@property(nonatomic) int stage;
@end

@implementation NativeDocumentTestScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [UIViewController new];
  [self.window makeKeyAndVisible];
  testWindow = self.window;
  dispatch_async(dispatch_get_main_queue(), ^{
    const std::string row = "#00111:010000020000030000040000050000060000070000080000\n";
    for (int i = 0; i < 10000; ++i) source += row;
    target = row.size() * 5000;
    // Deliver the button action before any usable viewport/first layout.
    surface = native_document::detail::createApple(source, {0, 0, 0, 0}, {});
    require(surface != nullptr, "create UIKit surface");
    surface->setSyntax(bms_syntax::tokenize(source));
    jump();
    surface->setFrame({0, 0, 1, 1});
    UITextView *view = findText(testWindow);
    require(view != nil && !view.editable && view.selectable, "native view is selectable/read-only");
    view.selectedRange = NSMakeRange(7, 2);
    checkFocus(view);
    focusedY = view.contentOffset.y;
    self.frames = [CADisplayLink displayLinkWithTarget:self selector:@selector(nextFrame:)];
    [self.frames addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  });
}
- (void)nextFrame:(CADisplayLink *)sender {
  UITextView *view = findText(testWindow);
  if (self.stage < 4 && self.stage != 2) {
    checkFocus(view);
    checkSyntax(view);
    require(std::abs(view.contentOffset.y - focusedY) < 1, "line geometry stays stable across frames");
  }
  switch (self.stage++) {
  case 0:
    [view setContentOffset:CGPointMake(0, 1500) animated:NO];
    jump();
    checkFocus(view);
    break;
  case 1:
    [view setContentOffset:CGPointMake(0, 3000) animated:YES];
    break; // Let a native scroll animation begin before requesting focus.
  case 2:
    jump();
    checkFocus(view);
    break;
  case 3:
    surface->setFrame({0, 0, 0.7f, 0.8f});
    jump();
    checkFocus(view);
    break;
  case 4:
    checkFocus(view);
    checkSyntax(view);
    // A layout callback may already be queued when the overlay closes.
    [view setNeedsLayout];
    [view layoutIfNeeded];
    surface.reset();
    require(findText(testWindow) == nil, "close removes native text view");
    break;
  default:
    [sender invalidate];
    std::fprintf(stderr, "PASS native document UIKit: early focus, manual/animated scroll, stable geometry, resize, selection, bounded syntax, queued close\n");
    std::exit(0);
  }
}
@end

int main(int argc, char **argv) {
  @autoreleasepool {
    NSSetUncaughtExceptionHandler([](NSException *exception) {
      std::fprintf(stderr, "FAIL UIKit exception: %s\n%s\n", exception.description.UTF8String,
          exception.callStackSymbols.description.UTF8String);
      std::exit(2);
    });
    return UIApplicationMain(argc, argv, nil, nil);
  }
}
