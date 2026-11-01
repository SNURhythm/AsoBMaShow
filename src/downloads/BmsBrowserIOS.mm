#include "../targets.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadPlatform.h"
#include "DownloadService.h"
#include "../AtomicFile.h"
#include <fstream>

namespace {
char downloadReferrerKey;
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
@interface AsoBmsBrowser : UIViewController <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate, UITextFieldDelegate>
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) UITextField *address;
@property(nonatomic, strong) UIBarButtonItem *importItem;
@property(nonatomic, copy) NSString *tableUrl;
@property(nonatomic, copy) NSString *tablePageUrl;
@property(nonatomic) NSUInteger tableProbe;
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
  self.address = [UITextField new]; self.address.placeholder = @"Browse Sites";
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
- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  // Block SDL input throughout presentation, before the browser is interactive.
  try { downloads::setBrowserForeground(true); } catch (...) {}
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self foreground]; }
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self background]; }
- (void)foreground { if (self.view.window) { try { downloads::setBrowserForeground(true); } catch (...) {} } }
- (void)background { try { downloads::setBrowserForeground(false); } catch (...) {} }
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
  if (object != self.web || ![keyPath isEqualToString:@"URL"]) { [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return; }
  NSURL *url = self.web.URL;
  self.address.text = url.absoluteString;
  [self refreshTable:!self.web.loading];
  if (http(url)) {
    try {
      const std::string value = url.absoluteString.UTF8String;
      const auto ops = atomic_file::privateFileOperations(); std::string error;
      atomic_file::writeWithoutBackup(downloads::platform::privateRoot() / "browser-url",
          std::as_bytes(std::span(value.data(), value.size())), error, &ops);
    } catch (...) {}
  }
}
- (void)clearTable {
  ++self.tableProbe;
  self.tableUrl = nil; self.tablePageUrl = nil; self.importItem.enabled = NO;
}
- (void)refreshTable:(BOOL)inspectDocument {
  [self clearTable];
  NSString *pageUrl = self.web.URL.absoluteString;
  if (!http(self.web.URL)) return;
  self.tablePageUrl = pageUrl;
  if (const auto known = downloads::browserTableUrl(pageUrl.UTF8String)) {
    self.tableUrl = text(*known); self.importItem.enabled = YES;
  }
  if (!inspectDocument) return;
  const NSUInteger probe = self.tableProbe;
  __weak AsoBmsBrowser *weakSelf = self;
  [self.web evaluateJavaScript:text(downloads::browserTableDetectionScript()) completionHandler:^(id result, NSError *error) {
    AsoBmsBrowser *browser = weakSelf;
    if (!browser || error || probe != browser.tableProbe ||
        ![pageUrl isEqualToString:browser.web.URL.absoluteString] || ![result isKindOfClass:NSDictionary.class]) return;
    if (![result[@"pageUrl"] isKindOfClass:NSString.class] || ![result[@"pageUrl"] isEqualToString:pageUrl] ||
        ![result[@"tableUrl"] isKindOfClass:NSString.class]) return;
    const auto table = downloads::browserTableUrl(pageUrl.UTF8String, [result[@"tableUrl"] UTF8String]);
    browser.tableUrl = table ? text(*table) : nil;
    browser.importItem.enabled = table.has_value();
  }];
}
- (void)webView:(WKWebView *)web didStartProvisionalNavigation:(WKNavigation *)navigation { [self clearTable]; }
- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)navigation { [self refreshTable:YES]; }
- (void)load:(NSString *)value {
  NSURL *url = [NSURL URLWithString:value];
  if (http(url)) { [self clearTable]; [self.web loadRequest:[NSURLRequest requestWithURL:url]]; }
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
  UIAlertController *menu = [UIAlertController alertControllerWithTitle:@"Browse Sites" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  [menu addAction:[UIAlertAction actionWithTitle:@"GingerRush" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://gingerrush.com/"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Manbow" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://manbow.nothing.sh/event/event.cgi"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Downloads" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { downloads::requestDownloadsPresentation(); [self close]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Open externally" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self external:self.web.URL]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  menu.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
  [self presentViewController:menu animated:YES completion:nil];
}
- (void)importTable {
  NSString *url = self.tableUrl;
  if (!url || ![self.tablePageUrl isEqualToString:self.web.URL.absoluteString]) return;
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Import this table?"
      message:@"Adds or updates the selected difficulty table. Its charts will not be downloaded." preferredStyle:UIAlertControllerStyleAlert];
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
- (void)download:(NSURL *)url filename:(NSString *)filename method:(NSString *)method referrer:(NSString *)referrer {
  if (!http(url) || ![method isEqualToString:@"GET"]) { [self unsupported]; return; }
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
  if (action.shouldPerformDownload) {
    decision(WKNavigationActionPolicyDownload); return;
  }
  if (action.navigationType == WKNavigationTypeLinkActivated &&
      (action.targetFrame.mainFrame || !action.targetFrame) &&
      http(action.request.URL) && [action.request.HTTPMethod isEqualToString:@"GET"]) {
    // A user-activated universal link may launch Drive (or another app).
    // Programmatic loads keep this HTTP request in WebKit, including redirects.
    decision(WKNavigationActionPolicyCancel);
    [web loadRequest:action.request]; return;
  }
  decision(WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationResponse:(WKNavigationResponse *)response decisionHandler:(void (^)(WKNavigationResponsePolicy))decision {
  auto httpResponse = [response.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response.response : nil;
  NSString *disposition = [httpResponse valueForHTTPHeaderField:@"Content-Disposition"];
  const bool attachment = [[disposition lowercaseString] hasPrefix:@"attachment"];
  const bool archive = downloads::archiveResponse((response.response.MIMEType ?: @"").UTF8String,
      (response.response.suggestedFilename ?: @"").UTF8String);
  // Sites such as OneDrive start attachment downloads in hidden iframes.
  // WebKit supplies the actual download request for every frame; the main
  // page's last method is not evidence that an iframe request is replayable.
  if (attachment || !response.canShowMIMEType || archive) {
    decision(WKNavigationResponsePolicyDownload); return;
  }
  decision(WKNavigationResponsePolicyAllow);
}
- (void)prepareDownload:(WKDownload *)download fromWebView:(WKWebView *)web {
  NSString *source = [download.originalRequest valueForHTTPHeaderField:@"Referer"] ?: web.URL.absoluteString ?: @"";
  objc_setAssociatedObject(download, &downloadReferrerKey, source, OBJC_ASSOCIATION_COPY_NONATOMIC);
  download.delegate = self;
}
- (void)webView:(WKWebView *)web navigationAction:(WKNavigationAction *)action didBecomeDownload:(WKDownload *)download {
  [self prepareDownload:download fromWebView:web];
}
- (void)webView:(WKWebView *)web navigationResponse:(WKNavigationResponse *)response didBecomeDownload:(WKDownload *)download {
  [self prepareDownload:download fromWebView:web];
}
- (void)download:(WKDownload *)download decideDestinationUsingResponse:(NSURLResponse *)response
    suggestedFilename:(NSString *)filename completionHandler:(void (^)(NSURL *))completion {
  NSURLRequest *request = download.originalRequest;
  NSString *source = objc_getAssociatedObject(download, &downloadReferrerKey) ?: @"";
  // Stop the browser probe before admitting the durable, cancellable transfer.
  // Never replay a POST as GET or borrow method evidence from another frame.
  completion(nil);
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!http(request.URL) || !http(response.URL) ||
        !downloads::allowedRedirect(request.URL.absoluteString.UTF8String, response.URL.absoluteString.UTF8String)) {
      [self unsupported]; return;
    }
    [self download:response.URL filename:filename method:request.HTTPMethod referrer:source];
  });
}
- (void)download:(WKDownload *)download willPerformHTTPRedirection:(NSHTTPURLResponse *)response
    newRequest:(NSURLRequest *)request decisionHandler:(void (^)(WKDownloadRedirectPolicy))decision {
  decision(http(response.URL) && http(request.URL) &&
      downloads::allowedRedirect(response.URL.absoluteString.UTF8String, request.URL.absoluteString.UTF8String)
      ? WKDownloadRedirectPolicyAllow : WKDownloadRedirectPolicyCancel);
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
