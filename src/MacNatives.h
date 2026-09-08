#pragma once
#include "targets.h"
#include <string>

void setSmoothScrolling(bool smoothScrolling);

#if TARGET_OS_OSX
bool RevealPathInFinder(const std::string &path, std::string &errorMessage);
bool OpenPathWithDefaultApplication(const std::string &path,
                                    std::string &errorMessage);
bool OpenURLInDefaultBrowser(const std::string &url, std::string &errorMessage);
#endif
