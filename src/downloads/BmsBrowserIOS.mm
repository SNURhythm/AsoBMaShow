#include "../targets.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadService.h"
#include "DownloadWebKitIOS.h"

namespace {
// Resume within this process only; a new app launch starts at Manbow.
NSString *sessionUrl = @"https://manbow.nothing.sh/event/event.cgi";
NSString *text(const std::string &value) { return [NSString stringWithUTF8String:value.c_str()] ?: @""; }
bool http(NSURL *url) { return url && downloads::httpOrigin(url.absoluteString.UTF8String).has_value(); }

}
@interface AsoBmsBrowser : UIViewController <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate, UITextFieldDelegate>
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) UISearchTextField *address;
@property(nonatomic, strong) UIBarButtonItem *backItem;
@property(nonatomic, strong) UIBarButtonItem *forwardItem;
@property(nonatomic, strong) UIBarButtonItem *reloadItem;
@property(nonatomic, strong) UIProgressView *progress;
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
  // Safari compatibility tokens keep Google from serving its legacy UI.
  // Leave out an app suffix for compatibility with ordinary browsing sites.
  NSOperatingSystemVersion osVersion = NSProcessInfo.processInfo.operatingSystemVersion;
  configuration.applicationNameForUserAgent = [NSString stringWithFormat:
      @"Version/%ld.%ld Safari/605.1.15", (long)osVersion.majorVersion, (long)osVersion.minorVersion];
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
  self.progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
  self.progress.translatesAutoresizingMaskIntoConstraints = NO; self.progress.hidden = YES;
  [self.view addSubview:self.progress];
  [NSLayoutConstraint activateConstraints:@[
    [self.progress.leadingAnchor constraintEqualToAnchor:self.web.leadingAnchor],
    [self.progress.trailingAnchor constraintEqualToAnchor:self.web.trailingAnchor],
    [self.progress.topAnchor constraintEqualToAnchor:self.web.topAnchor]]];
  // Put the native field directly in the title slot: UISearchBar adds its own
  // vertical padding, which shifts its visible field below the bar buttons.
  self.address = [UISearchTextField new]; self.address.placeholder = @"Search or enter URL";
  self.address.keyboardType = UIKeyboardTypeWebSearch; self.address.returnKeyType = UIReturnKeyGo;
  self.address.autocapitalizationType = UITextAutocapitalizationTypeNone;
  self.address.autocorrectionType = UITextAutocorrectionTypeNo;
  self.address.accessibilityLabel = @"Search or website address";
  self.address.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  self.address.adjustsFontForContentSizeCategory = YES;
  self.address.clearButtonMode = UITextFieldViewModeWhileEditing;
  self.address.contentVerticalAlignment = UIControlContentVerticalAlignmentCenter;
  self.address.delegate = self;
  self.address.frame = CGRectMake(0, 0, 400, 36);
  self.navigationItem.titleView = self.address;
  self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose target:self action:@selector(close)];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Sites" style:UIBarButtonItemStylePlain target:self action:@selector(sites)];
  self.importItem = [[UIBarButtonItem alloc] initWithTitle:@"Import table" style:UIBarButtonItemStylePlain target:self action:@selector(importTable)];
  self.importItem.enabled = NO;
  self.backItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"chevron.backward"]
      style:UIBarButtonItemStylePlain target:self action:@selector(back)];
  self.backItem.accessibilityLabel = @"Back";
  self.forwardItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"chevron.forward"]
      style:UIBarButtonItemStylePlain target:self action:@selector(forward)];
  self.forwardItem.accessibilityLabel = @"Forward";
  self.reloadItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.clockwise"]
      style:UIBarButtonItemStylePlain target:self action:@selector(reload)];
  self.toolbarItems = @[
    self.backItem,
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil], self.forwardItem,
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil], self.importItem,
    [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace target:nil action:nil], self.reloadItem];
  for (NSString *key in @[@"URL", @"canGoBack", @"canGoForward", @"loading", @"estimatedProgress"])
    [self.web addObserver:self forKeyPath:key options:NSKeyValueObservingOptionNew context:nil];
  [self updateControls];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background) name:UIApplicationDidEnterBackgroundNotification object:nil];
  [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
  [self load:sessionUrl];
}
- (void)dealloc {
  for (NSString *key in @[@"URL", @"canGoBack", @"canGoForward", @"loading", @"estimatedProgress"])
    [self.web removeObserver:self forKeyPath:key];
  [NSNotificationCenter.defaultCenter removeObserver:self];
}
- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  // Block SDL input throughout presentation, before the browser is interactive.
  try { downloads::setBrowserForeground(true); } catch (...) {}
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self foreground]; }
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self background]; }
- (void)foreground { if (self.view.window) { try { downloads::setBrowserForeground(true); } catch (...) {} } }
- (void)background { try { downloads::setBrowserForeground(false); } catch (...) {} }
- (void)updateControls {
  self.backItem.enabled = self.web.canGoBack; self.forwardItem.enabled = self.web.canGoForward;
  self.reloadItem.image = [UIImage systemImageNamed:self.web.loading ? @"xmark" : @"arrow.clockwise"];
  self.reloadItem.accessibilityLabel = self.web.loading ? @"Stop loading" : @"Reload";
  self.progress.hidden = !self.web.loading;
  [self.progress setProgress:self.web.estimatedProgress animated:YES];
  // Redirects and background navigation must not replace a URL being edited.
  if (!self.address.isFirstResponder) self.address.text = self.web.URL.absoluteString;
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
  if (object != self.web || ![@[@"URL", @"canGoBack", @"canGoForward", @"loading", @"estimatedProgress"] containsObject:keyPath]) {
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context]; return;
  }
  [self updateControls];
  if ([keyPath isEqualToString:@"URL"]) {
    [self refreshTable:!self.web.loading];
    if (http(self.web.URL)) sessionUrl = self.web.URL.absoluteString;
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
  if (http(url)) {
    [self.address resignFirstResponder]; self.address.text = value;
    [self clearTable]; [self.web loadRequest:[NSURLRequest requestWithURL:url]];
  }
}
- (void)textFieldDidBeginEditing:(UITextField *)field {
  dispatch_async(dispatch_get_main_queue(), ^{ if (field.isFirstResponder) [field selectAll:nil]; });
}
- (void)textFieldDidEndEditing:(UITextField *)field { [self updateControls]; }
- (BOOL)textFieldShouldReturn:(UITextField *)field {
  if (const auto target = downloads::browserAddressTarget((field.text ?: @"").UTF8String)) {
    [field resignFirstResponder]; [self load:text(*target)];
  }
  return YES;
}
- (void)back { [self.web goBack]; }
- (void)forward { [self.web goForward]; }
- (void)reload { if (self.web.loading) [self.web stopLoading]; else [self.web reload]; }
- (void)close { [self.web stopLoading]; [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)external:(NSURL *)url {
  if (http(url)) [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
}
- (void)sites {
  UIAlertController *menu = [UIAlertController alertControllerWithTitle:@"Browse Sites" message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  [menu addAction:[UIAlertAction actionWithTitle:@"Digital Emergency Exit" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://manbow.nothing.sh/event/event.cgi"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"GingerRush" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://gingerrush.com/"]; }]];
  [menu addAction:[UIAlertAction actionWithTitle:@"BMS Search" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) { [self load:@"https://bmssearch.net/"]; }]];
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
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))decision {
  // Browser checks such as Turnstile create inline documents in child frames.
  const bool inlineFrame = action.targetFrame && !action.targetFrame.mainFrame &&
      [action.request.URL.absoluteString isEqualToString:@"about:srcdoc"];
  if (!http(action.request.URL) && ![action.request.URL.absoluteString isEqualToString:@"about:blank"] && !inlineFrame) {
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
- (void)webView:(WKWebView *)web navigationAction:(WKNavigationAction *)action didBecomeDownload:(WKDownload *)download {
  download.delegate = self;
}
- (void)webView:(WKWebView *)web navigationResponse:(WKNavigationResponse *)response didBecomeDownload:(WKDownload *)download {
  download.delegate = self;
}
- (void)download:(WKDownload *)download decideDestinationUsingResponse:(NSURLResponse *)response
    suggestedFilename:(NSString *)filename completionHandler:(void (^)(NSURL *))completion {
  NSURLRequest *request = download.originalRequest;
  if (!http(request.URL) || !http(response.URL) || ![request.HTTPMethod isEqualToString:@"GET"] ||
      !downloads::allowedRedirect(request.URL.absoluteString.UTF8String, response.URL.absoluteString.UTF8String)) {
    completion(nil); [self unsupported]; return;
  }
  // Keep the actual browser response. A service worker can supply an archive
  // here even when replaying the same URL outside WebKit returns an HTML page.
  UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Download BMS archive?"
      message:[NSString stringWithFormat:@"%@\n\nKeep the app open. iOS 26 or later may grant continued background processing. Interrupted browser sessions may require choosing the download again.", filename]
      preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *) { completion(nil); }]];
  [alert addAction:[UIAlertAction actionWithTitle:@"Download" style:UIAlertActionStyleDefault handler:^(UIAlertAction *) {
    const auto error = downloads::platform::admitWebKitDownload(download, self.web, response, filename, completion);
    UIAlertController *result = [UIAlertController alertControllerWithTitle:error.empty() ? @"Added to Downloads" : @"Download unavailable"
        message:error.empty() ? @"You can continue browsing or close this page. Manage progress and cancellation in Downloads." : text(error)
        preferredStyle:UIAlertControllerStyleAlert];
    [result addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:result animated:YES completion:nil];
  }]];
  [self presentViewController:alert animated:YES completion:nil];
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
