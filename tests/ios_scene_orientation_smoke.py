from pathlib import Path
import argparse, subprocess, shutil, plistlib
parser = argparse.ArgumentParser(description="Build the native SDL orientation regression harness; does not install or upload.")
parser.add_argument("--sdl-framework", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
app = out / "Smoke.app"
app.mkdir(exist_ok=True)
s=root.joinpath('src/iOSNatives.mm').read_text()
apply=s[s.index('// iOS scene orientation transition.'):s.index('// list files recursively',s.index('// iOS scene orientation transition.'))]
source=r'''
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "platform/ScreenOrientation.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
static UIWindow *appWindow;
static UIWindow *FindActiveWindow() { return appWindow; }
static void require(bool value,const char *message) { if(!value){NSLog(@"FAIL %s",message);abort();} }
static void pumpUntil(bool (^condition)(void)) {
 double end=CACurrentMediaTime()+5;
 while(!condition() && CACurrentMediaTime()<end) { @autoreleasepool { SDL_PumpEvents();CFRunLoopRunInMode(kCFRunLoopDefaultMode,.004,true); } }
 require(condition(),"orientation transition timed out");
}
'''+apply+r'''

static unsigned requests = 0;
static bool denyRequest = false;
static void (^savedError)(NSError *);
static void (*originalRequest)(id, SEL, UIWindowSceneGeometryPreferences *, void (^)(NSError *));
static void checkRequest(id scene, SEL selector, UIWindowSceneGeometryPreferences *preferences, void (^error)(NSError *)) {
 require(!appWindow.rootViewController.prefersInterfaceOrientationLocked, "geometry requested without a lock preference");
 require(!appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked, "geometry requested only after effective unlock");
 ++requests;
 if (denyRequest) { savedError = [error copy]; return; }
 originalRequest(scene, selector, preferences, error);
}
static void ignoreLegacyRotation(id, SEL) {}
static int audit(int,char**) {
 @autoreleasepool {
  require(SDL_Init(SDL_INIT_VIDEO),"SDL init");
  SDL_Window *window=SDL_CreateWindow("Orientation smoke",320,640,SDL_WINDOW_FULLSCREEN|SDL_WINDOW_METAL|SDL_WINDOW_HIGH_PIXEL_DENSITY|SDL_WINDOW_RESIZABLE);
  require(window!=nullptr,"SDL window");
  appWindow=(__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,nullptr);
  require(appWindow!=nil,"actual SDL UIKit window");

  Method requestMethod=class_getInstanceMethod(UIWindowScene.class,@selector(requestGeometryUpdateWithPreferences:errorHandler:));
  originalRequest=(decltype(originalRequest))method_setImplementation(requestMethod,(IMP)checkRequest);
  auto mode=screen_orientation::Mode::Auto;
  screen_orientation::apply(mode,false);
  require(!appWindow.rootViewController.prefersInterfaceOrientationLocked,"Auto does not lock (including iPhone without upside-down)");
  screen_orientation::apply(screen_orientation::Mode::Landscape,false);
  require(!appWindow.rootViewController.prefersInterfaceOrientationLocked,"must not lock portrait while Landscape is pending");
  pumpUntil(^bool { return UIInterfaceOrientationIsLandscape(appWindow.windowScene.effectiveGeometry.interfaceOrientation) && appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });
  screen_orientation::apply(screen_orientation::Mode::Portrait,false);
  pumpUntil(^bool { return appWindow.windowScene.effectiveGeometry.interfaceOrientation==UIInterfaceOrientationPortrait && appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });
  screen_orientation::apply(mode,false);
  pumpUntil(^bool { return !appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });
  screen_orientation::apply(mode,true);
  pumpUntil(^bool { return appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });
  require(appWindow.rootViewController.supportedInterfaceOrientations==UIInterfaceOrientationMaskPortrait,"gameplay captures current orientation");
  screen_orientation::apply(mode,false);
  pumpUntil(^bool { return !appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });

  // Expired unlock waits terminate without issuing a locked geometry request.
  screen_orientation::apply(screen_orientation::Mode::Portrait,false);
  pumpUntil(^bool { return appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });
  const unsigned beforeTimeout=requests;
  RequestIOSOrientationAfterUnlock(appWindow,UIInterfaceOrientationMaskLandscape,
      sOrientationRequestGeneration,CACurrentMediaTime()-1);
  require(requests==beforeTimeout,"bounded unlock timeout");

  // Auto supersedes a request that is still waiting for the old lock to release.
  screen_orientation::apply(screen_orientation::Mode::Landscape,false);
  const NSUInteger staleGeneration=sOrientationRequestGeneration;
  screen_orientation::apply(mode,false);
  RequestIOSOrientationAfterUnlock(appWindow,UIInterfaceOrientationMaskLandscape,staleGeneration,CACurrentMediaTime()+1);
  require(requests==beforeTimeout,"Auto cancels queued geometry request");
  pumpUntil(^bool { return !appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked; });

  // Suppress iPhone's legacy mask-driven autorotation to model a resizable
  // environment; the real geometry entry point is intercepted only for denial.
  Class controllerClass=appWindow.rootViewController.class;
  SEL invalidate=@selector(setNeedsUpdateOfSupportedInterfaceOrientations);
  IMP originalInvalidate=class_getMethodImplementation(controllerClass,invalidate);
  class_replaceMethod(controllerClass,invalidate,(IMP)ignoreLegacyRotation,"v@:");
  denyRequest=true;
  screen_orientation::apply(screen_orientation::Mode::Landscape,false);
  pumpUntil(^bool { return savedError!=nil; });
  NSError *denied=[NSError errorWithDomain:UISceneErrorDomain code:UISceneErrorCodeGeometryRequestDenied userInfo:@{NSLocalizedDescriptionKey:@"Synthetic geometry denial"}];
  savedError(denied);
  __block bool denialDrained=false;
  dispatch_async(dispatch_get_main_queue(),^{ denialDrained=true; });
  pumpUntil(^bool { return denialDrained; });
  require(!appWindow.rootViewController.prefersInterfaceOrientationLocked &&
          !appWindow.windowScene.effectiveGeometry.isInterfaceOrientationLocked &&
          appWindow.windowScene.effectiveGeometry.interfaceOrientation==UIInterfaceOrientationPortrait,
          "denied Landscape request leaves portrait unlocked");
  screen_orientation::apply(mode,false);
  savedError(denied); // Late completion from the cancelled request must be ignored.
  __block bool drained=false;
  dispatch_async(dispatch_get_main_queue(),^{ drained=true; });
  pumpUntil(^bool { return drained; });
  require(!appWindow.rootViewController.prefersInterfaceOrientationLocked,"late denial cannot relock Auto");
  savedError=nil;
  class_replaceMethod(controllerClass,invalidate,originalInvalidate,"v@:");
  method_setImplementation(requestMethod,(IMP)originalRequest);
  NSLog(@"PASS actual SDL + production apply: unlocked requests, confirmed relock, Auto/gameplay, timeout, cancellation, denial, stale completion");
  SDL_DestroyWindow(window);SDL_Quit();
 }
 return 0;
}
int main(int argc,char **argv){return SDL_RunApp(argc,argv,audit,nullptr);}
'''
(out/'smoke.mm').write_text(source)
plist = {
    "CFBundlePackageType": "APPL", "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
    "MinimumOSVersion": "26.0", "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
    "UIApplicationSupportsIndirectInputEvents": True,
    "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown", "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
    "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False,
        "UISceneConfigurations": {"UIWindowSceneSessionRoleApplication": [
            {"UISceneConfigurationName": "Smoke", "UISceneDelegateClassName": "SDLUIKitSceneDelegate"}]}}
}
plist.update(CFBundleExecutable='Smoke',CFBundleIdentifier='com.snurhythm.sdl-orientation-smoke',CFBundleName='OrientationSmoke',UIRequiresFullScreen=True,UIRequiresFullScreenIgnoredStartingWithVersion='27')
(app/'Info.plist').write_bytes(plistlib.dumps(plist))
framework = args.sdl_framework.resolve()
shutil.copytree(framework,app/'Frameworks/SDL3.framework',dirs_exist_ok=True)
sdk=subprocess.check_output(['xcrun','--sdk','iphonesimulator','--show-sdk-path'],text=True).strip()
cmd=['xcrun','--sdk','iphonesimulator','clang++','-std=c++23','-fobjc-arc','-target','arm64-apple-ios26.0-simulator','-isysroot',sdk,'-I'+str(root/'src'),str(out/'smoke.mm'),'-F'+str(app/'Frameworks'),'-framework','SDL3','-framework','UIKit','-framework','QuartzCore','-framework','Foundation','-Wl,-rpath,@executable_path/Frameworks','-o',str(app/'Smoke')]
subprocess.run(cmd,check=True)
