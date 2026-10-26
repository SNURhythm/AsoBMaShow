// Manual simulator integration fixture for the production iOS adapter.
// The SDL launch shim below starts a minimal scene-based UIKit host; the real
// notification delegate, queue, replacement, and foreground cancellation run
// unchanged. Provisional authorization avoids a test-only permission dialog.
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include "notifications/LocalNotifications.h"
static UILabel *statusLabel;
static bool requested = false, scheduled = false;
static void record(NSString *value) {
  NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/status.txt"];
  [value writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
  statusLabel.text = value;
}
@interface SmokeScene : NSObject <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation SmokeScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  UIViewController *view = [UIViewController new];
  view.view.backgroundColor = UIColor.systemBackgroundColor;
  statusLabel = [[UILabel alloc] initWithFrame:CGRectMake(20, 180, 340, 220)];
  statusLabel.numberOfLines = 0;
  statusLabel.textColor = UIColor.labelColor;
  [view.view addSubview:statusLabel];
  self.window.rootViewController = view;
  [self.window makeKeyAndVisible];
  record([NSString stringWithFormat:@"Started. Delegate=%@. Scene cold response=%@",
      NSStringFromClass(UNUserNotificationCenter.currentNotificationCenter.delegate.class), options.notificationResponse ? @"yes" : @"no"]);
  [UNUserNotificationCenter.currentNotificationCenter requestAuthorizationWithOptions:UNAuthorizationOptionProvisional
      completionHandler:^(BOOL granted, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
          requested = granted;
          if (!granted) record(@"FAIL: provisional authorization denied");
        });
      }];
  [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *timer) {
    reminders::refreshPermission();
    const auto tap = reminders::takeTapProfile();
    if (!tap.empty()) record([@"TAP RECEIVED: " stringByAppendingString:[NSString stringWithUTF8String:tap.c_str()]]);
    if (requested && !scheduled && reminders::permission() == reminders::Permission::Granted) {
      scheduled = true;
      NSCalendar *calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
      NSDateComponents *date = [calendar components:NSCalendarUnitYear|NSCalendarUnitMonth|NSCalendarUnitDay|NSCalendarUnitHour|NSCalendarUnitMinute
          fromDate:[NSDate dateWithTimeIntervalSinceNow:90]];
      NSDictionary *entry = @{@"year":@(date.year), @"month":@(date.month), @"day":@(date.day), @"hour":@(date.hour), @"minute":@(date.minute),
          @"title":@"Combo smoke", @"body":@"Tap to test a cold notification launch.", @"profile":@"smoke-profile"};
      NSData *data = [NSJSONSerialization dataWithJSONObject:@[entry] options:0 error:nil];
      reminders::replacePlan(std::string((const char *)data.bytes, data.length));
      reminders::replacePlan("[]");
      reminders::replacePlan(std::string((const char *)data.bytes, data.length));
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [UNUserNotificationCenter.currentNotificationCenter getPendingNotificationRequestsWithCompletionHandler:^(NSArray<UNNotificationRequest *> *requests) {
          dispatch_async(dispatch_get_main_queue(), ^{
            if (requests.count != 1) { record(@"FAIL: replace/cancel/replace count"); return; }
            [NSNotificationCenter.defaultCenter postNotificationName:UISceneWillEnterForegroundNotification object:nil];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
              [UNUserNotificationCenter.currentNotificationCenter getPendingNotificationRequestsWithCompletionHandler:^(NSArray *remaining) {
                dispatch_async(dispatch_get_main_queue(), ^{
                  record(remaining.count == 0 ? @"PASS: replacement leaves one request; foreground return cancels it" : @"FAIL: cancellation left pending requests");
                });
              }];
            });
          });
        }];
      });
    }
  }];
}
@end
@interface SmokeDelegate : NSObject <UIApplicationDelegate>
@end
@implementation SmokeDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Smoke" sessionRole:session.role];
  config.delegateClass = SmokeScene.class;
  return config;
}
@end
int AsoBMaShowMain(int, char **) { return 0; }
extern "C" int SDL_RunApp(int argc, char **argv, SDL_main_func, void *) {
  return UIApplicationMain(argc, argv, nil, @"SmokeDelegate");
}
