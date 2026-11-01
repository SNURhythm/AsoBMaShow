#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadService.h"
#include "../targets.h"
#include <deque>
#include <algorithm>

namespace downloads {
namespace {
std::mutex browserMutex;
std::optional<Request> browserDefaults;
std::deque<std::string> tables;
}
void configureBrowser(Request defaults) {
  std::lock_guard lock(browserMutex);
  browserDefaults = std::move(defaults);
}
std::string acceptBrowserDownload(const std::string &method, const std::string &url,
    const std::string &filename, const std::string &userAgent, const std::string &cookie,
    const std::string &referrer) {
  std::optional<Request> request;
  {
    std::lock_guard lock(browserMutex);
    if (!browserDefaults) return "Return to the app and reopen Browse BMS before downloading.";
    request = makeBrowserRequest(*browserDefaults, method, url, filename, userAgent, cookie, referrer);
  }
  if (!request) return "This request cannot be repeated safely. Open the page in your external browser.";
  try {
    const auto job = service()->submit(std::move(*request));
    requestDownloadsPresentation(job.id);
    return {};
  } catch (...) { return "Could not save this download. Check free storage and try again."; }
}
bool requestBrowserTableImport(const std::string &pageUrl) {
  const auto url = browserTableUrl(pageUrl, pageUrl);
  if (!url) return false;
  std::lock_guard lock(browserMutex);
  if (tables.size() >= 32) return false;
  if (std::find(tables.begin(), tables.end(), *url) == tables.end()) tables.push_back(*url);
  return true;
}
std::optional<std::string> takeBrowserTableImport() {
  std::lock_guard lock(browserMutex);
  if (tables.empty()) return {};
  auto url = std::move(tables.front()); tables.pop_front(); return url;
}
#if !TARGET_OS_IPHONE && !TARGET_OS_ANDROID
void openBrowser() {}
#endif
}
