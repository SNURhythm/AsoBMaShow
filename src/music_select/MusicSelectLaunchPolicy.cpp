#include "MusicSelectLaunchPolicy.h"

#include <utility>

MusicSelectLaunchDecision
decideMusicSelectLaunch(skin::GameplaySkinAcquisition acquisition) {
  switch (acquisition.disposition) {
  case skin::GameplaySkinAcquisitionDisposition::BuiltIn:
    return {.kind = MusicSelectLaunchKind::BuiltIn};
  case skin::GameplaySkinAcquisitionDisposition::Ready:
    if (acquisition.request) {
      return {.kind = MusicSelectLaunchKind::SelectedSkin,
              .request = std::move(acquisition.request)};
    }
    break;
  case skin::GameplaySkinAcquisitionDisposition::Failed:
    break;
  }

  MusicSelectLaunchDecision result{.kind = MusicSelectLaunchKind::Error};
  if (acquisition.failure) {
    result.diagnostics.push_back(std::move(acquisition.failure->diagnostic));
  }
  return result;
}
