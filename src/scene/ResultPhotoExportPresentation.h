#pragma once

#include <string_view>

enum class ResultPhotoExportPresentation { Ready, Saving, Saved, Failed };

[[nodiscard]] constexpr std::string_view
resultPhotoExportLabel(ResultPhotoExportPresentation presentation) {
  switch (presentation) {
  case ResultPhotoExportPresentation::Ready: return "Export Photo";
  case ResultPhotoExportPresentation::Saving: return "Saving...";
  case ResultPhotoExportPresentation::Saved: return "Saved";
  case ResultPhotoExportPresentation::Failed: return "Export Failed";
  }
  return "Export Photo";
}
