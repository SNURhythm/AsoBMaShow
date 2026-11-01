#pragma once
#include "DownloadJobs.h"
#include <optional>

namespace downloads {
// Captured once on the application thread; native UI never reaches settings or scenes.
void configureBrowser(Request defaults);
void openBrowser();
std::optional<Request> makeBrowserRequest(const Request &defaults, const std::string &method,
    const std::string &url, const std::string &filename, const std::string &userAgent,
    const std::string &cookie, const std::string &referrer);
std::optional<Request> browserDownloadRequest(const std::string &method, const std::string &url,
    const std::string &filename, const std::string &userAgent, const std::string &cookie,
    const std::string &referrer);
// Empty return means success. These are native UI callbacks, never page-script bridges.
std::string acceptBrowserDownload(const std::string &method, const std::string &url,
    const std::string &filename, const std::string &userAgent, const std::string &cookie,
    const std::string &referrer);
// Accepts a detected HTTP(S) table URL; the importer validates its contents.
bool requestBrowserTableImport(const std::string &pageUrl);
std::optional<std::string> takeBrowserTableImport();
} // namespace downloads
