#pragma once
#include "DownloadJobs.h"

#ifdef __OBJC__
@class WKDownload, WKWebView, NSURLResponse, NSString, NSURL;
namespace downloads::platform {
// Takes ownership of the destination callback, including on admission failure.
std::string admitWebKitDownload(WKDownload *download, WKWebView *web,
    NSURLResponse *response, NSString *filename, void (^destination)(NSURL *));
}
#endif
namespace downloads::platform {
std::string startWebKitTransfer(const Job &job);
void cancelWebKitTransfer(const Job &job);
}
