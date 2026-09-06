#include "MusicSelectLaunchPolicy.h"

#include <string>
#include <utility>

std::string musicSelectSkinEntryPath(const skin::SkinEntryId &entry) {
  if (entry.package.directoryName.empty()) return entry.packageRelativePath;
  if (entry.packageRelativePath.empty()) return entry.package.directoryName;
  return entry.package.directoryName + "/" + entry.packageRelativePath;
}

std::string
musicSelectSkinFailureReason(std::size_t index,
                             const skin::SkinDiagnostic &diagnostic) {
  std::string reason = std::to_string(index + 1) + ". ";
  if (!diagnostic.code.empty()) reason += diagnostic.code + ": ";
  reason += diagnostic.message;
  if (diagnostic.source && !diagnostic.source->virtualPath.empty()) {
    reason += " (" + diagnostic.source->virtualPath;
    if (diagnostic.source->line != 0) {
      reason += ":" + std::to_string(diagnostic.source->line);
      if (diagnostic.source->column != 0) {
        reason += ":" + std::to_string(diagnostic.source->column);
      }
    }
    reason += ")";
  } else if (!diagnostic.virtualPath.empty()) {
    reason += " (" + diagnostic.virtualPath + ")";
  }
  return reason;
}

MusicSelectLaunchDecision
decideMusicSelectLaunch(skin::GameplaySkinAcquisition acquisition) {
  switch (acquisition.disposition) {
  case skin::GameplaySkinAcquisitionDisposition::BuiltIn:
    return {.kind = MusicSelectLaunchKind::BuiltIn};
  case skin::GameplaySkinAcquisitionDisposition::Ready:
    if (acquisition.request) {
      return {.kind = MusicSelectLaunchKind::SelectedSkin,
              .selectedSkinPath = musicSelectSkinEntryPath(
                  acquisition.request->activation.entry),
              .request = std::move(acquisition.request)};
    }
    break;
  case skin::GameplaySkinAcquisitionDisposition::Failed:
    break;
  }

  MusicSelectLaunchDecision result{.kind = MusicSelectLaunchKind::Error};
  if (acquisition.failure) {
    if (acquisition.failure->entry) {
      result.selectedSkinPath =
          musicSelectSkinEntryPath(*acquisition.failure->entry);
    }
    result.diagnostics.push_back(std::move(acquisition.failure->diagnostic));
  } else {
    result.diagnostics.push_back(skin::SkinDiagnostic{
        .code = "skin.music_select.acquisition_incomplete",
        .message =
            "The selected music-select skin acquisition did not provide its "
            "activation or failure diagnostic."});
  }
  return result;
}
