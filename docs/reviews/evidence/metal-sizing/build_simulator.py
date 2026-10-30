from pathlib import Path
import argparse,subprocess,plistlib,shutil
parser=argparse.ArgumentParser(description='Build the native SDL drawable sizing regression harness; never installs or distributes.')
parser.add_argument('--sdl-framework',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[4]
out=args.output.resolve()
app=out/'Sizing.app'
app.mkdir(parents=True,exist_ok=True)
s=(root/'src/iOSNatives.mm').read_text()
helper=s[s.index('bool SetIOSMetalLayerDrawableSize('):s.index('void WaitIOSMainRunLoopForMicros(')]
source=r'''#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
namespace platform {
inline bool isMainThread() { return [NSThread isMainThread]; }
template<typename F> auto onMain(F &&f) { return f(); }
}
static void require(bool ok,const char *msg) {if(!ok){NSLog(@"FAIL %s",msg);abort();}}
'''+helper+r'''
static unsigned resizeEvents = 0;
static bool SDLCALL watch(void *, SDL_Event *event) {
 if(event->type==SDL_EVENT_WINDOW_METAL_VIEW_RESIZED) ++resizeEvents;
 return true;
}
static int audit(int,char**) {
 @autoreleasepool {
  require(SDL_Init(SDL_INIT_VIDEO),"SDL init");
  SDL_Window *window=SDL_CreateWindow("Metal sizing",320,640,SDL_WINDOW_FULLSCREEN|SDL_WINDOW_METAL|SDL_WINDOW_HIGH_PIXEL_DENSITY|SDL_WINDOW_RESIZABLE);
  require(window!=nullptr,"window");
  SDL_MetalView metal=SDL_Metal_CreateView(window);
  require(metal!=nullptr,"metal view");
  require(SDL_AddEventWatch(watch,nullptr),"resize watcher");
  UIView *view=(__bridge UIView*)metal;
  CAMetalLayer *layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(metal);
  // Recorded iPad geometry on the real SDL view; no physical-device timing claim.
  const CGFloat nativeScale=1668.0/970.0;
  view.bounds=CGRectMake(0,0,970,1408);
  layer.contentsScale=nativeScale;
  [view layoutSubviews];
  require(SetIOSMetalLayerDrawableSize((__bridge void*)layer,1668,2420),"set native target");
  NSLog(@"before layout size=%@ scale=%.12f",NSStringFromCGSize(layer.drawableSize),layer.contentsScale);
  const auto unchangedEvents=resizeEvents;
  for(int i=0;i<4;++i) [view layoutSubviews];
  NSLog(@"after layout size=%@ scale=%.12f",NSStringFromCGSize(layer.drawableSize),layer.contentsScale);
  require(CGSizeEqualToSize(layer.drawableSize,CGSizeMake(1668,2420)),"unchanged foreground layout must retain exact native drawable size");
  require(resizeEvents==unchangedEvents,"unchanged layout must not emit another Metal resize");
  require(layer.contentsScale==nativeScale,"setting drawable pixels must preserve UIKit native scale");
  view.bounds=CGRectMake(0,0,500,800);
  [view layoutSubviews];
  // Metal canonicalizes fractional pixel sizes. Check the new pixel bounds,
  // not an exact floating-point product that the native layer cannot retain.
  require(layer.drawableSize.width>=859 && layer.drawableSize.width<=860 &&
          layer.drawableSize.height>=1375 && layer.drawableSize.height<=1376,
          "real bounds change still updates drawable sizing");
  layer.contentsScale=2;
  [view layoutSubviews];
  require(CGSizeEqualToSize(layer.drawableSize,CGSizeMake(1000,1600)),"real display-scale change still updates drawable sizing");
  require(resizeEvents==unchangedEvents+2,"actual bounds and scale changes each emit a resize");
  require(SetIOSMetalLayerDrawableSize((__bridge void*)layer,999,1599),"custom render size");
  [view layoutSubviews];
  require(CGSizeEqualToSize(layer.drawableSize,CGSizeMake(999,1599)),"unchanged geometry preserves an explicit renderer size");
  NSLog(@"PASS actual SDL + production sizing: repeated layouts stable; native scale retained; bounds and scale changes still resize");
  SDL_RemoveEventWatch(watch,nullptr);
  SDL_Metal_DestroyView(metal); SDL_DestroyWindow(window); SDL_Quit();
 }
 return 0;
}
int main(int argc,char **argv) {return SDL_RunApp(argc,argv,audit,nullptr);}
'''
(out/'sizing.mm').write_text(source)
info={'CFBundleExecutable':'Sizing','CFBundleIdentifier':'com.snurhythm.metal-sizing-audit','CFBundleName':'MetalSizingAudit','CFBundlePackageType':'APPL','CFBundleVersion':'1','CFBundleShortVersionString':'1.0','MinimumOSVersion':'17.0','UIDeviceFamily':[1,2],'UILaunchScreen':{},'UIApplicationSupportsIndirectInputEvents':True,'UISupportedInterfaceOrientations':['UIInterfaceOrientationPortrait','UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight'],'UIApplicationSceneManifest':{'UIApplicationSupportsMultipleScenes':False,'UISceneConfigurations':{'UIWindowSceneSessionRoleApplication':[{'UISceneConfigurationName':'Audit','UISceneDelegateClassName':'SDLUIKitSceneDelegate'}]}}}
(app/'Info.plist').write_bytes(plistlib.dumps(info))
framework=args.sdl_framework.resolve()
shutil.copytree(framework,app/'Frameworks/SDL3.framework',dirs_exist_ok=True)
sdk=subprocess.check_output(['xcrun','--sdk','iphonesimulator','--show-sdk-path'],text=True).strip()
subprocess.run(['xcrun','--sdk','iphonesimulator','clang++','-std=c++23','-fobjc-arc','-target','arm64-apple-ios17.0-simulator','-isysroot',sdk,'-I'+str(root/'SDL/include'),str(out/'sizing.mm'),'-F'+str(app/'Frameworks'),'-framework','SDL3','-Wl,-rpath,@executable_path/Frameworks','-framework','Foundation','-framework','UIKit','-framework','QuartzCore','-framework','Metal','-o',str(app/'Sizing')],check=True)
print(app)
