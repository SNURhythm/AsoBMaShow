#include "BmsBrowser.h"
#include "BrowserPolicy.h"

namespace downloads {
std::optional<Request> makeBrowserRequest(const Request &defaults, const std::string &method,
    const std::string &url, const std::string &filename, const std::string &userAgent,
    const std::string &cookie, const std::string &referrer) {
  if (!replayableBrowserDownload(method, url)) return {};
  Request request = defaults;
  request.url = url;
  request.displayUrl = httpOrigin(referrer) ? referrer : url;
  request.title = request.archiveName = safeArchiveName(filename);
  request.expectedHash.clear(); request.storageIdentity = url;
  request.headers.clear();
  if (!userAgent.empty()) request.headers.emplace_back("User-Agent", userAgent);
  if (!cookie.empty()) request.headers.emplace_back("Cookie", cookie);
  if (const auto origin = httpOrigin(referrer); origin && allowedRedirect(referrer, url)) {
    request.headers.emplace_back("Referer", sameOrigin(referrer, url) ? referrer.substr(0, referrer.find('#')) : *origin + "/");
  }
  return validRequest(request) ? std::optional<Request>(std::move(request)) : std::nullopt;
}
}
