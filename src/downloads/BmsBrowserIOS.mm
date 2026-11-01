#include "../targets.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadPlatform.h"
#include "DownloadService.h"
#include "../AtomicFile.h"
#include <fstream>

namespace {
NSString *text(const std::string &value) { return [NSString stringWithUTF8String:value.c_str()] ?: @""; }
bool http(NSURL *url) { return url && downloads::httpOrigin(url.absoluteString.UTF8String).has_value(); }
NSString *cookieHeader(NSArray<NSHTTPCookie *> *cookies, NSURL *url) {
  NSMutableArray *matching = [NSMutableArray new];
  for (NSHTTPCookie *cookie in cookies) {
    NSString *domain = cookie.domain.lowercaseString;
    BOOL domainCookie = [domain hasPrefix:@"."];
    if (domainCookie) domain = [domain substringFromIndex:1];
    NSString *host = url.host.lowercaseString, *path = url.path.length ? url.path : @"/";
    if (![host isEqualToString:domain] && !(domainCookie && [host hasSuffix:[@"." stringByAppendingString:domain]])) continue;
    if (![path hasPrefix:cookie.path] ||
        (![path isEqualToString:cookie.path] && ![cookie.path hasSuffix:@"/"] &&
         ![[path substringFromIndex:cookie.path.length] hasPrefix:@"/"])) continue;
    if (cookie.secure && ![url.scheme.lowercaseString isEqualToString:@"https"]) continue;
    if (cookie.expiresDate && [cookie.expiresDate timeIntervalSinceNow] <= 0) continue;
    if (cookie.portList && ![cookie.portList containsObject:url.port ?: ([url.scheme isEqualToString:@"https"] ? @443 : @80)]) continue;
    [matching addObject:cookie];
  }
  return [NSHTTPCookie requestHeaderFieldsWithCookies:matching][@"Cookie"] ?: @"";
}
}
@interface AsoBmsBrowser : UIViewController <WKNavigationDelegate, WKUIDelegate, UITextFieldDelegate>
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) UITextField *address;
@property(nonatomic, strong) UIBarButtonItem *importItem;
@property(nonatomic, copy) NSString *method;
@property(nonatomic, copy) NSString *referrer;
- (void)load:(NSString *)url;
@end
@implementation AsoBmsBrowser
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.systemBackgroundColor;
  WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
  // Retained while this browser lives; session credentials never enter backups.
  configuration.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
  configuration.applicationNameForUserAgent = @"AsoBMaShow";
  self.web = [[WKWebView alloc] initWithFrame:CGRectZero configuration:configuration];
  self.web.navigationDelegate = self; self.web.UIDelegate = self;
  self.web.allowsBackForwardNavigationGestures = YES;
  self.web.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:self.web];
  [NSLayoutConstraint activateConstraints:@[
    [self.web.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [self.web.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [self.web.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [self.web.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor]]];
  self.address = [UITextField new]; self.address.placeholder = @"Browse BMS";
  self.address.keyboardType = UIKeyboardTypeURL; self.address.returnKeyType = UIReturnKeyGo;
  self.address.autocapitalizationType = UITextAutocapitalizationTypeNone;
  self.address.autocorrectionType = UITextAutocorrectionTypeNo;
  self.address.font = [UIFont systemFontOfSize:14]; self.address.delegate = self;
  self.address.frame = CGRectMake(0, 0, 400, 36);
  self.navigationItem.titleView = self.address;
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose target:self action:@selector(close)];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Sites" style:UIBarButtonItemStylePlain target:self action:@selector(sites)];
  self.importItem = [[UIBarButtonItem alloc] initWithTitle:@"Import table" style:UIBarButtonItemStylePlain target:self action:@selector(importTable)];
  self.importItem.enabled = NO;
  self.toolbarItems = @[
    [[UIBarButtonItem alloc] initWithTitle:@"Back" style:UIBarButtonItemStylePlain target:self action:@selector(back)],
    [[UIBarButtonItem alloc] initWithTitle:@"Forward" style:UIBarButtonItemStylePlain target:self action:@selector(forward)],
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil], self.importItem,
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil],
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(reload)]];
  [self.web addObserver:self forKeyPath:@"URL" options:NSKeyValueObservingOptionNew context:nil];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background) name:UIApplicationDidEnterBackgroundNotification object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
  NSString *url = @"https://gingerrush.com/";
  try {
    const auto path = downloads::platform::privateRoot() / "browser-url";
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && std::filesystem::file_size(path, error) <= 16384) {
      std::ifstream input(path); std::string saved((std::istreambuf_iterator<char>(input)), {});
      if (downloads::httpOrigin(saved)) url = text(saved);
    }
  } catch (...) {}
  [self load:url];
}
- (void)dealloc { [self.web removeObserver:self forKeyPath:@"URL"]; [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self foreground]; }
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self background]; }
- (void)foreground { if (self.view.window) { try { downloads::setBrowserForeground(true); } catch (...) {} } }
- (void)background { try { downloads::setBrowserForeground(false); } catch (...) {} }
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
  if (object != self.web || ![keyPath isEqualToString:@"URL"]) { [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return; }
  NSURL *url = self.web.URL;
  self.address.text = url.absoluteString;
  self.importItem.enabled = url && downloads::gingerTableUrl(url.absoluteString.UTF8String).has_value();
  if (http(url)) {
    try {
      const std::string value = url.absoluteString.UTF8String;
      const auto ops = atomic_file::privateFileOperations(); std::string error;
      atomic_file::writeWithoutBackup(downloads::platform::privateRoot() / "browser-url",
          std::as_bytes(std::span(value.data(), value.size())), error, &ops);
    } catch (...) {}
  }
}
- (void)load:(NSString *)value {
  NSURL *url = [NSURL URLWithString:value];
  if (http(url)) [self.web loadRequest:[NSURLRequest requestWithURL:url]];
}
- (BOOL)textFieldShouldReturn:(UITextField *)field {
  NSString *value = field.text;
  if (![value containsString:@"://"]) value = [@"https://" stringByAppendingString:value];
  [self load:value]; [field resignFirstResponder]; return YES;
}
- (void)back { [self.web goBack]; }
- (void)forward { [self.web goForward]; }
- (void)reload { [self.web reload]; }
- (void)close { [self.web stopLoading]; [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)external:(NSURL *)url {
  if (http(url)) [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
}
- (void)sites {
  UIAlertController *menu = [UIAlertController alertControllerWithTitle:@"Browse BMS" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  [menu addAction:[UIAlertAction actionWithTitle:@"GingerRush" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://gingerrush.com/"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Manbow" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://manbow.nothing.sh/event/"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Downloads" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { downloads::requestDownloadsPresentation(); [self close]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Open externally" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self external:self.web.URL]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  menu.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
  [self presentViewController:menu animated:YES completion:nil];
}
- (void)importTable {
  NSString *url = self.web.URL.absoluteString;
  if (!url || !downloads::gingerTableUrl(url.UTF8String)) return;
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Import this table?"
      message:@"Adds or updates the selected GingerRush table. Its charts will not be downloaded." preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Import table" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
    if (downloads::requestBrowserTableImport(url.UTF8String)) [self close];
  }]];
  [self presentViewController:alert animated:YES completion:nil];
}
- (void)unsupported {
  NSURL *source = self.web.URL;
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Open this download externally"
      message:@"This request cannot be repeated safely. Open the source page in your browser and choose the download there." preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Open externally" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self external:source]; }]];
  [self presentViewController:alert animated:YES completion:nil];
}
- (void)download:(NSURL *)url filename:(NSString *)filename method:(NSString *)method {
  if (!http(url) || ![method isEqualToString:@"GET"]) { [self unsupported]; return; }
  NSString *referrer = self.referrer ?: self.web.URL.absoluteString ?: @"";
  NSString *notice = @"Keep the app open. On iOS 26 or later, the system may grant continued background processing. If runtime ends, reopen Downloads and retry.";
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Download BMS archive?"
      message:[NSString stringWithFormat:@"%@\n\n%@", filename, notice] preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Download" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
    [self.web evaluateJavaScript:@"navigator.userAgent" completionHandler:^(id agent, NSError *) {
      NSString *userAgent = [agent isKindOfClass:NSString.class] ? agent : @"AsoBMaShow";
      [self.web.configuration.websiteDataStore.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
        auto error = downloads::acceptBrowserDownload("GET", url.absoluteString.UTF8String,
            (filename ?: @"download.archive").UTF8String, userAgent.UTF8String,
            cookieHeader(cookies, url).UTF8String, referrer.UTF8String);
        UIAlertController *result = [UIAlertController alertControllerWithTitle:error.empty() ? @"Added to Downloads" : @"Download unavailable"
            message:error.empty() ? @"You can continue browsing or close this page. Manage progress and cancellation in Downloads." : text(error)
            preferredStyle:UIAlertControllerStyleAlert];
        [result addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:result animated:YES completion:nil];
      }];
    }];
  }]];
  [self presentViewController:alert animated:YES completion:nil];
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
  if (!http(action.request.URL) && ![action.request.URL.absoluteString isEqualToString:@"about:blank"]) {
    decision(WKNavigationActionPolicyCancel); if (action.targetFrame.mainFrame || !action.targetFrame) [self unsupported]; return;
  }
  if (action.targetFrame.mainFrame || !action.targetFrame) {
    self.method = action.request.HTTPMethod ?: @"GET";
    self.referrer = [action.request valueForHTTPHeaderField:@"Referer"] ?: web.URL.absoluteString;
  }
  if (action.shouldPerformDownload) {
    decision(WKNavigationActionPolicyCancel);
    [self download:action.request.URL filename:action.request.URL.lastPathComponent method:action.request.HTTPMethod]; return;
  }
  decision(WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationResponse:(WKNavigationResponse *)response decisionHandler:(void (^)(WKNavigationResponsePolicy))decision {
  auto httpResponse = [response.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response.response : nil;
  NSString *disposition = [httpResponse valueForHTTPHeaderField:@"Content-Disposition"];
  const bool attachment = [[disposition lowercaseString] hasPrefix:@"attachment"];
  const bool archive = downloads::archiveResponse((response.response.MIMEType ?: @"").UTF8String,
      (response.response.suggestedFilename ?: @"").UTF8String);
  if (response.isForMainFrame && (attachment || !response.canShowMIMEType || archive)) {
    decision(WKNavigationResponsePolicyCancel);
    [self download:response.response.URL filename:response.response.suggestedFilename method:self.method]; return;
  }
  decision(WKNavigationResponsePolicyAllow);
}
- (WKWebView *)webView:(WKWebView *)web createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
    forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
  if (![action.request.HTTPMethod isEqualToString:@"GET"]) { [self unsupported]; return nil; }
  [web loadRequest:action.request]; return nil;
}
@end
namespace downloads {
void openBrowser() {
  dispatch_async(dispatch_get_main_queue(), ^{
    static AsoBmsBrowser *browser;
    static UINavigationController *navigation;
    if (!browser) {
      browser = [AsoBmsBrowser new]; navigation = [[UINavigationController alloc] initWithRootViewController:browser];
      navigation.toolbarHidden = NO; navigation.modalPresentationStyle = UIModalPresentationFullScreen;
    }
    if (navigation.presentingViewController) return;
    UIViewController *presenting = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
      if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:UIWindowScene.class]) continue;
      for (UIWindow *window in ((UIWindowScene *)scene).windows) if (window.isKeyWindow) presenting = window.rootViewController;
    }
    while (presenting.presentedViewController) presenting = presenting.presentedViewController;
    [presenting presentViewController:navigation animated:YES completion:nil];
  });
}
}
#endif
