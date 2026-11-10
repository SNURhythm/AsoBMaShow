#include "../targets.h"
#if !TARGET_OS_IPHONE && !TARGET_OS_ANDROID
#include "DownloadPlatform.h"
#include "../Utils.h"
namespace downloads::platform {
std::filesystem::path privateRoot() { return Utils::GetDocumentsPath(".asobmashow-private/GingerRushDownloadsV1"); }
void initialize() {}
std::string startTransfer(const Job &, const std::filesystem::path &) {
  return "Background download jobs are available on mobile. Use Find BMS on this platform.";
}
void cancelTransfer(const Job &) {}
void beginImport(const Job &) {}
void finishActivity(const Job &, bool) {}
void reportProgress(const Job &) {}
}
#endif
