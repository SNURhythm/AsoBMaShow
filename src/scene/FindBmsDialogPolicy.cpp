#include "../i18n/Localization.h"
#include "FindBmsDialogPolicy.h"

FindBmsDialogPolicy findBmsDialogPolicy(bool running,
                                        const BmsSearchResult &result) {
  const bool pending = result.pendingArtifact.has_value();
  return {.canDismiss = !running && !pending,
          .showCloseOrCancel = !pending,
          .showPendingActions = pending && !running,
          .showNormalResultActions = !pending};
}

i18n::Text findBmsDownloadFailureMessage(const BmsSearchResult &result) {
  if (!result.presentationMessage.empty()) {
    return result.presentationMessage;
  }
  return result.message.empty() ? i18n::message("library.find_bms.open_source_try_again.message")
                                : i18n::Text(result.message);
}

std::string findBmsDownloadFailureDetail(const BmsSearchResult &result) {
  return findBmsDownloadFailureMessage(result).resolve();
}
