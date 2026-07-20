#include "DownloadStaging.h"

BmsSearchResult BmsSearchService::resolvePendingArtifact(
    BmsSearchResult result,
    BmsSearchPendingArtifactDecision decision) const {
  if (!result.pendingArtifact) {
    result.message = "No downloaded files are awaiting a decision.";
    return result;
  }
  const auto artifact = *result.pendingArtifact;
  std::string error;
  const bool resolved = decision == BmsSearchPendingArtifactDecision::Keep
                            ? asobmshow::bms_search::commitFindBmsPendingArtifact(
                                  artifact, error)
                            : asobmshow::bms_search::deleteFindBmsPendingArtifact(
                                  artifact, error);
  if (!resolved) {
    result.message = error.empty() ? "Could not resolve downloaded files."
                                   : error;
    return result;
  }
  result.pendingArtifact.reset();
  if (decision == BmsSearchPendingArtifactDecision::Keep) {
    result.outputPath = artifact.destinationPath;
    result.message = "Mismatched files kept.";
  } else {
    result.outputPath.clear();
    result.message = "Mismatched files deleted.";
  }
  return result;
}
