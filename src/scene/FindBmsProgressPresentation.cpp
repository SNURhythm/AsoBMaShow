#include "../i18n/Localization.h"
#include "FindBmsProgressPresentation.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace {

std::string progressPercentText(double ratio) {
  const int percent =
      static_cast<int>(std::lround(std::clamp(ratio, 0.0, 1.0) * 100.0));
  return std::to_string(percent) + "%";
}

} // namespace

std::string formatFindBmsBytes(std::uint64_t bytes) {
  constexpr double kKib = 1024.0;
  constexpr double kMib = kKib * 1024.0;
  constexpr double kGib = kMib * 1024.0;
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(bytes >= 10 * 1024 ? 1 : 0);
  if (bytes >= static_cast<std::uint64_t>(kGib)) {
    stream << static_cast<double>(bytes) / kGib << " GB";
  } else if (bytes >= static_cast<std::uint64_t>(kMib)) {
    stream << static_cast<double>(bytes) / kMib << " MB";
  } else if (bytes >= static_cast<std::uint64_t>(kKib)) {
    stream << static_cast<double>(bytes) / kKib << " KB";
  } else {
    stream.str("");
    stream.clear();
    stream << bytes << " B";
  }
  return stream.str();
}

i18n::Text findBmsProgressDisplayMessage(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes) {
  if (message == "Downloading archive" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    auto text = i18n::message("library.find_bms.progress.download_percentage",
                                    {{"progress", progressPercentText(ratio)}});
    if (includeBytes) {
      text = i18n::message("library.find_bms.progress.with_bytes",
          {{"progress", text}, {"downloaded", formatFindBmsBytes(downloadedBytes)},
           {"total", formatFindBmsBytes(totalBytes)}});
    }
    return text;
  }
  if (message == "Downloading archive" && downloadedBytes > 0) {
    return i18n::message("library.find_bms.progress.download_size",
                        {{"size", formatFindBmsBytes(downloadedBytes)}});
  }
  if (message == "Download complete" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    return i18n::message("library.find_bms.progress.download_complete_percentage",
                        {{"progress", progressPercentText(ratio)}});
  }
  if (message == "Searching Horie archive") return i18n::message("library.find_bms.progress.searching_horie");
  if (message == "Preparing Horie archive download") return i18n::message("library.find_bms.progress.preparing_horie");
  if (message == "Confirming Google Drive download") return i18n::message("library.find_bms.progress.confirming_drive");
  if (message == "Inspecting downloaded archive") return i18n::message("library.find_bms.progress.inspecting_archive");
  if (message == "Validating archive contents") return i18n::message("library.find_bms.progress.validating_archive");
  if (message == "Saving downloaded archive") return i18n::message("library.find_bms.progress.saving_archive");
  if (message == "Unarchiving archive") return i18n::message("library.find_bms.progress.unarchiving_archive");
  for (const auto &[prefix, suffix, key] : {
           std::tuple{"Searching ", " package source", "library.find_bms.progress.searching_package"},
           std::tuple{"Preparing ", " package download", "library.find_bms.progress.preparing_package"}}) {
    const std::string_view value = message;
    if (value.starts_with(prefix) && value.ends_with(suffix) &&
        value.size() > std::string_view(prefix).size() + std::string_view(suffix).size()) {
      return i18n::message(key, {{"source", value.substr(
          std::string_view(prefix).size(), value.size() -
          std::string_view(prefix).size() - std::string_view(suffix).size())}});
    }
  }
  if (message == "Preparing lookup") return i18n::message("library.find_bms.progress.preparing_lookup.status");
  if (message == "Opening BMS Search pattern page") return i18n::message("library.find_bms.progress.opening_bms_search_pattern_page.status");
  if (message == "Opening BMS Search details page") return i18n::message("library.find_bms.progress.opening_bms_search_details_page.status");
  if (message == "Downloading archive") return i18n::message("library.find_bms.progress.downloading_archive.status");
  if (message == "Download complete") return i18n::message("library.find_bms.progress.download_complete.status");
  if (message == "Extracting archive") return i18n::message("library.find_bms.progress.extracting_archive.status");
  constexpr std::string_view extractionPrefix = "Extracting ";
  if (message.starts_with(extractionPrefix)) {
    return i18n::message("library.find_bms.progress.extracting_file.status",
                        {{"filename", std::string_view(message).substr(extractionPrefix.size())}});
  }
  if (message == "Archive finished") return i18n::message("library.find_bms.progress.archive_finished.status");
  if (message == "Download failed") return i18n::message("library.find_bms.progress.download_failed.status");
  return message;
}

std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes) {
  return findBmsProgressDisplayMessage(message, downloadedBytes, totalBytes,
                                       includeBytes).resolve();
}

i18n::Text findBmsRunningDetailMessage(const std::string &message) {
  if (!message.empty()) {
    return findBmsProgressDisplayMessage(message, 0, 0, false);
  }
  return i18n::message("library.find_bms.searching_available_sources.progress");
}
