#include "LocalNotifications.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <atomic>
#include <mutex>

namespace {
std::atomic<reminders::Permission> authorization{reminders::Permission::Unknown};
std::mutex tapMutex;
std::string pendingProfile;
std::string lastTap;
// Center calls are asynchronous. Advance one add at a time, and apply any new
// plan only after the in-flight add completes, so cancellation cannot race it.
dispatch_queue_t notificationQueue() {
  static dispatch_queue_t queue = dispatch_queue_create("com.snurhythm.kombo-notifications", DISPATCH_QUEUE_SERIAL);
  return queue;
}
NSArray<UNNotificationRequest *> *desiredPlan = @[];
NSUInteger nextRequest = 0;
bool adding = false, replacePending = false;
NSArray<NSString *> *ownedIds() {
  NSMutableArray *ids = [NSMutableArray array];
  for (int i = 0; i < 56; ++i) [ids addObject:[NSString stringWithFormat:@"kombo.%d", i]];
  return ids;
}
void clearOwned() {
  auto center = UNUserNotificationCenter.currentNotificationCenter;
  [center removePendingNotificationRequestsWithIdentifiers:ownedIds()];
  [center removeDeliveredNotificationsWithIdentifiers:ownedIds()];
}
void pumpPlan() {
  if (adding) return;
  if (replacePending) {
    clearOwned();
    nextRequest = 0;
    replacePending = false;
  }
  if (nextRequest >= desiredPlan.count) return;
  adding = true;
  [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:desiredPlan[nextRequest++]
      withCompletionHandler:^(NSError *error) {
    if (error) NSLog(@"Kombo notification scheduling failed: %@", error.localizedDescription);
    dispatch_async(notificationQueue(), ^{ adding = false; pumpPlan(); });
  }];
}
void submitPlan(NSArray<UNNotificationRequest *> *plan) {
  desiredPlan = plan;
  replacePending = true;
  pumpPlan();
}
void receiveTap(UNNotificationResponse *response) {
  if (![response.notification.request.identifier hasPrefix:@"kombo."] ||
      ![response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) return;
  id profile = response.notification.request.content.userInfo[@"profile"];
  if (![profile isKindOfClass:NSString.class]) return;
  const std::lock_guard lock(tapMutex);
  const auto identity = std::string(response.notification.request.identifier.UTF8String) + ":" +
      std::to_string(response.notification.date.timeIntervalSince1970);
  if (identity == lastTap) return; // Scene connection and UN delegate may report the same cold tap.
  lastTap = identity;
  pendingProfile = [(NSString *)profile UTF8String];
}
}

@interface KomboNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end
@implementation KomboNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
      willPresentNotification:(UNNotification *)notification
        withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completion {
  completion(UNNotificationPresentationOptionNone);
}
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
 didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completion {
  receiveTap(response);
  completion();
}
@end

namespace {
void installDelegate() {
  static KomboNotificationDelegate *delegate;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    delegate = [KomboNotificationDelegate new];
    UNUserNotificationCenter.currentNotificationCenter.delegate = delegate;
    dispatch_async(notificationQueue(), ^{ submitPlan(@[]); });
    for (NSNotificationName name in @[UIApplicationWillEnterForegroundNotification,
                                      UISceneWillEnterForegroundNotification]) {
      [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:nil
          usingBlock:^(NSNotification *notification) {
        dispatch_async(notificationQueue(), ^{ submitPlan(@[]); });
      }];
    }
  });
}
}
// Own the public SDL entry point so the notification delegate is installed
// before UIKit launches, including cold notification taps. SDL's delegate
// classes are private symbols in its framework and must not be subclassed.
extern int AsoBMaShowMain(int argc, char **argv);
int main(int argc, char **argv) {
  @autoreleasepool {
    installDelegate();
    return SDL_RunApp(argc, argv, AsoBMaShowMain, nullptr);
  }
}

namespace reminders {
void refreshPermission() {
  [UNUserNotificationCenter.currentNotificationCenter getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
    switch (settings.authorizationStatus) {
    case UNAuthorizationStatusNotDetermined: authorization = Permission::NotDetermined; break;
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
    case UNAuthorizationStatusEphemeral:
      authorization = settings.alertSetting == UNNotificationSettingDisabled ? Permission::Denied : Permission::Granted;
      break;
    default: authorization = Permission::Denied; break;
    }
  }];
}
Permission permission() { return authorization.load(); }
void requestPermission() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [UNUserNotificationCenter.currentNotificationCenter requestAuthorizationWithOptions:
        UNAuthorizationOptionAlert | UNAuthorizationOptionSound completionHandler:^(BOOL granted, NSError *error) {
      refreshPermission();
    }];
  });
}
void openSettings() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString]
        options:@{} completionHandler:nil];
  });
}
void replacePlan(const std::string &json) {
  NSData *data = [NSData dataWithBytes:json.data() length:json.size()];
  dispatch_async(notificationQueue(), ^{
    NSMutableArray<UNNotificationRequest *> *plan = [NSMutableArray array];
    id requests = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![requests isKindOfClass:NSArray.class]) { submitPlan(@[]); return; }
    NSUInteger slot = 0;
    for (NSDictionary *entry in requests) {
      if (slot >= 56 || ![entry isKindOfClass:NSDictionary.class]) break;
      NSDateComponents *date = [NSDateComponents new];
      date.calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
      date.year = [entry[@"year"] integerValue];
      date.month = [entry[@"month"] integerValue];
      date.day = [entry[@"day"] integerValue];
      date.hour = [entry[@"hour"] integerValue];
      date.minute = [entry[@"minute"] integerValue];
      UNCalendarNotificationTrigger *trigger = [UNCalendarNotificationTrigger triggerWithDateMatchingComponents:date repeats:NO];
      if (!trigger.nextTriggerDate || [trigger.nextTriggerDate timeIntervalSinceNow] <= 0) continue;
      UNMutableNotificationContent *content = [UNMutableNotificationContent new];
      content.title = entry[@"title"];
      content.body = entry[@"body"];
      content.sound = UNNotificationSound.defaultSound;
      content.userInfo = @{@"profile": entry[@"profile"]};
      NSString *identifier = [NSString stringWithFormat:@"kombo.%lu", (unsigned long)slot++];
      [plan addObject:[UNNotificationRequest requestWithIdentifier:identifier content:content trigger:trigger]];
    }
    submitPlan(plan);
  });
}
std::string takeTapProfile() {
  const std::lock_guard lock(tapMutex);
  std::string result;
  result.swap(pendingProfile);
  return result;
}
}
#endif
