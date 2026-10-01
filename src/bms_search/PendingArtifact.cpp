#include "DownloadStaging.h"

BmsSearchResult BmsSearchService::resolvePendingArtifact(
    BmsSearchResult result,
    BmsSearchPendingArtifactDecision decision) const {
  result.removedPaths.clear();
  result.presentationMessage = {};
  if (!result.pendingArtifact) {
    result.message = "No downloaded files are awaiting a decision.";
    result.presentationMessage = i18n::message("library.find_bms.result.no_pending_files");
    return result;
  }
  const auto artifact = *result.pendingArtifact;
  std::string error;
  const bool resolved = decision == BmsSearchPendingArtifactDecision::Keep
                            ? asobmshow::bms_search::commitFindBmsPendingArtifact(
                                  artifact, error, {}, &result.removedPaths)
                            : asobmshow::bms_search::deleteFindBmsPendingArtifact(
                                  artifact, error);
  if (!resolved) {
    result.message = error.empty() ? "Could not resolve downloaded files."
                                   : error;
    if (error.empty()) {
      result.presentationMessage = i18n::message("library.find_bms.result.resolve_failed");
    }
    return result;
  }
  result.pendingArtifact.reset();
  if (decision == BmsSearchPendingArtifactDecision::Keep) {
    result.outputPath = artifact.destinationPath;
    result.message = "Mismatched files kept.";
    result.presentationMessage = i18n::message("library.find_bms.result.files_kept");
  } else {
    result.outputPath.clear();
    result.message = "Mismatched files deleted.";
    result.presentationMessage = i18n::message("library.find_bms.result.files_deleted");
  }
  return result;
}
