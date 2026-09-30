#pragma once
#include "../i18n/Localization.h"

#include <string_view>

enum class ResultPhotoExportPresentation { Ready, Saving, Saved, Failed };

[[nodiscard]] constexpr std::string_view
resultPhotoExportLabel(ResultPhotoExportPresentation presentation) {
  switch (presentation) {
  case ResultPhotoExportPresentation::Ready: return i18n::tr("result.photo_export.export_photo.label");
  case ResultPhotoExportPresentation::Saving: return "Saving...";
  case ResultPhotoExportPresentation::Saved: return i18n::tr("result.photo_export.saved.label");
  case ResultPhotoExportPresentation::Failed: return "Export Failed";
  }
  return i18n::tr("result.photo_export.export_photo.label");
}
