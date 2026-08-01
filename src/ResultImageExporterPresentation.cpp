#include "ResultImageExporter.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace {

bool validPresentationTimestamp(std::string_view timestamp) noexcept {
  if (timestamp.size() != 15 || timestamp[8] != '_') {
    return false;
  }
  return std::ranges::all_of(timestamp, [index = std::size_t{0}](
                                            unsigned char character) mutable {
    const bool valid = index == 8 ? character == '_' : std::isdigit(character);
    ++index;
    return valid;
  });
}

ResultImageExportResult exportFailure(std::string message,
                                      std::filesystem::path outputPath = {}) {
  return {.success = false,
          .outputPath = std::move(outputPath),
          .message = std::move(message)};
}

} // namespace

ResultImageExportResult ResultImageExporter::Export(
    const ResultPresentationModel &presentation,
    const result_image_export::PresentationExportDestination &destination,
    const result_image_export::PresentationRenderBackend &renderBackend) {
  if (destination.outputDirectory.empty() || !renderBackend) {
    return exportFailure("Result export destination or renderer is invalid");
  }
  if (!validPresentationTimestamp(destination.timestamp)) {
    return exportFailure("Result export timestamp is invalid");
  }

  std::error_code error;
  std::filesystem::create_directories(destination.outputDirectory, error);
  if (error ||
      !std::filesystem::is_directory(destination.outputDirectory, error) ||
      error) {
    return exportFailure("Failed to create result export directory");
  }

  auto plan = result_image_export::presentationPlanFor(presentation,
                                                       destination.timestamp);
  const std::filesystem::path outputPath =
      destination.outputDirectory / plan.filename;
  try {
    ResultImageExportResult result = renderBackend(
        std::move(plan.skinData), std::move(plan.gauge), outputPath);
    if (result.outputPath.empty()) {
      result.outputPath = outputPath;
    }
    if (result.outputPath != outputPath) {
      return exportFailure("Result renderer returned an unexpected output path",
                           outputPath);
    }
    if (!result.success) {
      return result;
    }
    if (result.artifactRetained) {
      error.clear();
      if (!std::filesystem::is_regular_file(outputPath, error) || error) {
        return exportFailure(
            "Result renderer did not produce an image artifact", outputPath);
      }
    }
    return result;
  } catch (...) {
    return exportFailure("Result image rendering failed", outputPath);
  }
}
