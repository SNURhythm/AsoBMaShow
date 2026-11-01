#pragma once
#include "DownloadCoordinator.h"
#include <memory>

namespace downloads {
std::shared_ptr<DownloadCoordinator> service();
void setGameForeground(bool foreground);
void setBrowserForeground(bool foreground);
void requestDownloadsPresentation(const std::string &id = {});
std::optional<std::string> takeDownloadsPresentation();
} // namespace downloads
