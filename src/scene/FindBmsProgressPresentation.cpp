#include "FindBmsProgressPresentation.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

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

std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes) {
  if (message == "Downloading archive" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    std::string text = "Downloading archive - " + progressPercentText(ratio);
    if (includeBytes) {
      text += " (" + formatFindBmsBytes(downloadedBytes) + " / " +
              formatFindBmsBytes(totalBytes) + ")";
    }
    return text;
  }
  if (message == "Downloading archive" && downloadedBytes > 0) {
    return "Downloading archive (" + formatFindBmsBytes(downloadedBytes) +
           ")";
  }
  if (message == "Download complete" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    return "Download complete - " + progressPercentText(ratio);
  }
  return message;
}
