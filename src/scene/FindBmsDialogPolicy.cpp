#include "FindBmsDialogPolicy.h"

FindBmsDialogPolicy findBmsDialogPolicy(bool running,
                                        const BmsSearchResult &result) {
  const bool pending = result.pendingArtifact.has_value();
  return {.canDismiss = !running && !pending,
          .showCloseOrCancel = !pending,
          .showPendingActions = pending && !running,
          .showNormalResultActions = !pending};
}

std::string findBmsDownloadFailureDetail(const BmsSearchResult &result) {
  return result.message.empty() ? "Open the source or try again."
                                : result.message;
}
