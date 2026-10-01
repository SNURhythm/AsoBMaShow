#!/usr/bin/env python3
"""Run the production bridge on macOS with simulated UIKit window geometry."""
import subprocess
import tempfile
from pathlib import Path

from gameplay_terminal_scene_extract import extract

ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#import <Foundation/Foundation.h>
#include "platform/IPadHardwareButton.h"
#include <cassert>
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
@end
@implementation UIViewController
@end
@interface UIWindowScene : NSObject
@property UIInterfaceOrientation interfaceOrientation;
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
    setenv("SIMULATOR_MODEL_IDENTIFIER", "iPad12,1", 1);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    testWindow = [UIWindow new];
    testWindow.screen = UIScreen.mainScreen;
    testWindow.screen.bounds = NSMakeRect(0, 0, 1024, 768);
    testWindow.screen.coordinateSpace = [NSObject new];
    testWindow.windowScene = [UIWindowScene new];
    testWindow.rootViewController = [UIViewController new];
    UIView *view = [UIView new];
    testWindow.rootViewController.view = view;
    view.bounds = testWindow.screen.bounds;
    view.screenFrame = testWindow.screen.bounds;
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeRight;
    auto cue = GetIOSHardwareButtonLocation();
    assert(cue.button == Button::Home && cue.edge == Edge::Right);
    assert(cue.x == 1 && cue.y == 0.5F);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeLeft;
    cue = GetIOSHardwareButtonLocation();
    assert(cue.edge == Edge::Left && cue.x == 0);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationPortrait;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Bottom);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationPortraitUpsideDown;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Top);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationUnknown;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    testWindow.windowScene.interfaceOrientation = UIInterfaceOrientationLandscapeRight;

    // The view's screen-space rectangle matters, not just its local size.
    view.screenFrame = NSMakeRect(512, 0, 512, 768);
    cue = GetIOSHardwareButtonLocation();
    assert(cue.button == Button::Home && cue.edge == Edge::Unknown);
    view.screenFrame = NSMakeRect(20, 20, 1024, 768);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    view.screenFrame = NSMakeRect(0, 0, 1024, 740);
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
    view.screenFrame = UIScreen.mainScreen.bounds;
    assert(GetIOSHardwareButtonLocation().edge == Edge::Right);
    testWindow.screen = [UIScreen new]; // External screen
    assert(GetIOSHardwareButtonLocation().edge == Edge::Unknown);
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
        subprocess.run(["xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
                        "-fobjc-arc", "-framework", "Foundation", "-I", str(ROOT / "src"),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("iPad native button orientation and window geometry tests passed")


if __name__ == "__main__":
    main()
