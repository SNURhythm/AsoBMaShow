#!/usr/bin/env python3
"""Run the production bridge on macOS with simulated UIKit window geometry."""
import subprocess
import tempfile
from pathlib import Path

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import build_foundation_fixture

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#import <Foundation/Foundation.h>
#include "platform/IPadHardwareButton.h"
#include <cassert>
#include <cstdarg>
#include <string>
#include <utility>
#include <vector>
static std::vector<std::string> cueLogReasons;
void SDL_Log(const char *format, ...) {
  va_list args;
  va_start(args, format);
  cueLogReasons.emplace_back(va_arg(args, const char *));
  va_end(args);
}
// Geometry fixture runs synchronously on its simulated UIKit owner.
namespace platform {
inline bool isMainThread() { return true; }
template <typename F> decltype(auto) onMain(F &&operation) {
  return std::forward<F>(operation)();
}
}
#include <cmath>
#include <cstdlib>
#include <sys/utsname.h>
#undef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR 1
using CGRect = NSRect;
// UIKit UIOrientation.h: interface landscape values reverse device values.
enum UIInterfaceOrientation { UIInterfaceOrientationUnknown = 0,
  UIInterfaceOrientationPortrait = 1, UIInterfaceOrientationPortraitUpsideDown = 2,
  UIInterfaceOrientationLandscapeLeft = 4, UIInterfaceOrientationLandscapeRight = 3 };
@interface UIScreen : NSObject
@property CGRect bounds;
@property(strong) id coordinateSpace;
+ (UIScreen *)mainScreen;
@end
@implementation UIScreen
+ (UIScreen *)mainScreen { static UIScreen *s = [UIScreen new]; return s; }
@end
@interface UIView : NSObject
@property CGRect bounds;
@property CGRect screenFrame;
- (CGRect)convertRect:(CGRect)rect toCoordinateSpace:(id)space;
@end
@implementation UIView
- (CGRect)convertRect:(CGRect)rect toCoordinateSpace:(id)space {
  assert(NSEqualRects(rect, self.bounds));
  assert(space == UIScreen.mainScreen.coordinateSpace);
  return self.screenFrame;
}
@end
@interface UIViewController : NSObject
@property(strong) UIView *view;
@property UIInterfaceOrientation interfaceOrientation;
@end
@implementation UIViewController
@end
@interface UIWindowSceneGeometry : NSObject
@property UIInterfaceOrientation interfaceOrientation;
@end
@implementation UIWindowSceneGeometry
@end
@interface UIWindowScene : NSObject
@property UIInterfaceOrientation interfaceOrientation;
@property(strong) UIScreen *screen;
@property(strong) UIWindowSceneGeometry *effectiveGeometry;
@end
@implementation UIWindowScene
@end
@interface UIWindow : NSObject
@property(strong) UIScreen *screen;
@property(strong) UIWindowScene *windowScene;
@property(strong) UIViewController *rootViewController;
@end
@implementation UIWindow
@end
static UIWindow *testWindow;
UIWindow *FindActiveWindow() { return testWindow; }
'''
TEST = r'''
int main() {
  @autoreleasepool {
    using namespace ipad_hardware;
    setenv("SIMULATOR_MODEL_IDENTIFIER", "iPad17,2", 1);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    assert(cueLogReasons.back() == "no-window");
    testWindow = [UIWindow new];
    testWindow.screen = UIScreen.mainScreen;
    testWindow.screen.bounds = NSMakeRect(0, 0, 1024, 768);
    testWindow.screen.coordinateSpace = [NSObject new];
    testWindow.windowScene = [UIWindowScene new];
    testWindow.windowScene.screen = testWindow.screen;
    testWindow.rootViewController = [UIViewController new];
    UIView *view = [UIView new];
    testWindow.rootViewController.view = view;
    view.bounds = testWindow.screen.bounds;
    view.screenFrame = testWindow.screen.bounds;
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeRight;
    auto cue = GetIOSHardwareButtonLocation();
    assert(cue.button == Button::Top && cue.edge == Edge::Left);
    assert(cue.x == 0 && std::abs(cue.y - 0.070693F) < 0.00001F);
    assert(cueLogReasons.back() == "visible");
    const auto logCount = cueLogReasons.size();
    GetIOSHardwareButtonLocation();
    assert(cueLogReasons.size() == logCount); // No per-frame log spam.
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeLeft;
    cue = GetIOSHardwareButtonLocation();
    assert(cue.edge == Edge::Right && cue.x == 1);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationPortrait;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Top);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationPortraitUpsideDown;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Bottom);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationUnknown;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    assert(cueLogReasons.back() == "unknown-orientation");
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeRight;

    // SDL can host a full-screen window without adopting the UIScene lifecycle.
    UIWindowScene *scene = testWindow.windowScene;
    testWindow.windowScene = nil;
    testWindow.rootViewController.interfaceOrientation = UIInterfaceOrientationLandscapeRight;
    cue = GetIOSHardwareButtonLocation();
    assert(cue.button == Button::Top && cue.edge == Edge::Left);
    assert(std::abs(cue.y - 0.070693F) < 0.00001F);
    testWindow.rootViewController.interfaceOrientation = UIInterfaceOrientationLandscapeLeft;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Right);
    testWindow.rootViewController.interfaceOrientation = UIInterfaceOrientationUnknown;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    testWindow.windowScene = scene;
    // A valid scene remains authoritative over a stale controller orientation.
    testWindow.rootViewController.interfaceOrientation = UIInterfaceOrientationLandscapeLeft;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Left);

    // The view's screen-space rectangle matters, not just its local size.
    view.screenFrame = NSMakeRect(512, 0, 512, 768);
    cue = GetIOSHardwareButtonLocation();
    assert(cue.button == Button::Top && cue.edge == Edge::Unknown);
    assert(cueLogReasons.back() == "partial-viewport");
    view.screenFrame = NSMakeRect(20, 20, 1024, 768);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    view.screenFrame = NSMakeRect(0, 0, 1024, 740);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    view.screenFrame = UIScreen.mainScreen.bounds;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Left);
    testWindow.screen = [UIScreen new]; // External screen
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    assert(cueLogReasons.back() == "screen-mismatch");
    testWindow.screen = UIScreen.mainScreen;
    testWindow.rootViewController.view = nil;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    testWindow = nil;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
  }
}
'''


def main():
    method = extract((ROOT / "src/iOSNatives.mm").read_text(),
                     "ipad_hardware::ButtonLocation GetIOSHardwareButtonLocation()")
    with tempfile.TemporaryDirectory(prefix="ipad-button-test-") as temporary:
        source = Path(temporary) / "test.mm"
        binary = Path(temporary) / "test"
        source.write_text(PREAMBLE + method + TEST)
        build_foundation_fixture(source, binary, includes=[ROOT / "src"])
        subprocess.run([str(binary)], check=True)
    print("iPad native button orientation and window geometry tests passed")


if __name__ == "__main__":
    main()
