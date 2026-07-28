#include "BeatorajaReplayPath.h"

#include "BeatorajaLongNoteMode.h"
#include "ReplayFormat.h"

#include <array>
#include <charconv>
#include <limits>
#include <ranges>

namespace replay {
namespace {

constexpr std::array<std::string_view, 3> kLongNotePrefixes{"", "C", "H"};
constexpr std::size_t kReplayExtensionBytes = 4;
constexpr std::size_t kMaximumHistorySuffixBytes =
    1 + std::numeric_limits<std::int64_t>::digits10 + 1 + kReplayExtensionBytes;

std::optional<std::string_view> longNotePrefix(int longNoteMode,
                                               bool hasUndefinedLongNotes,
                                               std::string &diagnostic) {
  const auto stockMode = stockLongNoteMode(longNoteMode);
  if (!stockMode || (hasUndefinedLongNotes && longNoteMode == 0)) {
    diagnostic = "Unsupported long-note mode for replay path";
    return std::nullopt;
  }
  if (!hasUndefinedLongNotes) {
    return std::string_view{};
  }
  return kLongNotePrefixes[static_cast<std::size_t>(*stockMode)];
}

bool isCanonicalConstraintSuffix(std::string_view constraints) noexcept {
  if (constraints.empty() || constraints.size() % 2 != 0) {
    return false;
  }
  for (std::size_t offset = 0; offset < constraints.size(); offset += 2) {
    const char tens = constraints[offset];
    const char ones = constraints[offset + 1];
    if (tens < '0' || tens > '9' || ones < '0' || ones > '9') {
      return false;
    }
    const int id = (tens - '0') * 10 + (ones - '0');
    if (id <= kBeatorajaLastExcludedCourseMarkerId ||
        id > kBeatorajaLastConstraintId) {
      return false;
    }
  }
  return true;
}

bool isCanonicalStem(std::string_view stem,
                     const ReplayLimits &limits) noexcept {
  if (stem.empty()) {
    return false;
  }
  if (stem.front() == 'C' || stem.front() == 'H') {
    stem.remove_prefix(1);
  }
  const std::size_t separator = stem.find('_');
  const std::string_view hashes = stem.substr(0, separator);
  const bool chart = isCanonicalLowerHex(hashes, 64);
  const bool course = hashes.size() >= 10 && hashes.size() % 10 == 0 &&
                      hashes.size() / 10 <= limits.maxCourseStages &&
                      isCanonicalLowerHex(hashes, hashes.size());
  if (!chart && !course) {
    return false;
  }
  if (separator == std::string_view::npos) {
    return true;
  }
  return course && isCanonicalConstraintSuffix(stem.substr(separator + 1));
}

void appendTwoDigits(std::string &value, int number) {
  value.push_back(static_cast<char>('0' + number / 10));
  value.push_back(static_cast<char>('0' + number % 10));
}

} // namespace

std::optional<std::string> chartStem(std::string_view lowerSha256,
                                     int longNoteMode,
                                     bool hasUndefinedLongNotes,
                                     std::string &diagnostic,
                                     const ReplayLimits &limits) {
  diagnostic.clear();
  if (!limits.valid() || !isCanonicalLowerHex(lowerSha256, 64)) {
    diagnostic = "Replay chart SHA-256 must be canonical lowercase hex";
    return std::nullopt;
  }
  const auto prefix =
      longNotePrefix(longNoteMode, hasUndefinedLongNotes, diagnostic);
  if (!prefix) {
    return std::nullopt;
  }
  std::string result(*prefix);
  result.append(lowerSha256);
  return result;
}

bool chartStemMatches(std::string_view stem, std::string_view lowerSha256,
                      int longNoteMode,
                      std::optional<bool> hasUndefinedLongNotes,
                      std::string &diagnostic, const ReplayLimits &limits) {
  const auto expected =
      chartStem(lowerSha256, longNoteMode,
                hasUndefinedLongNotes.value_or(false), diagnostic, limits);
  if (!expected || *expected == stem || hasUndefinedLongNotes.has_value()) {
    return expected && *expected == stem;
  }
  const auto prefixed =
      chartStem(lowerSha256, longNoteMode, true, diagnostic, limits);
  return prefixed && *prefixed == stem;
}

std::optional<std::string> courseStem(const CoursePathInput &input,
                                      std::string &diagnostic,
                                      const ReplayLimits &limits) {
  diagnostic.clear();
  if (!limits.valid() || input.stageSha256.empty() ||
      !withinReplayCountLimit(input.stageSha256.size(),
                              limits.maxCourseStages)) {
    diagnostic = "Replay course stage count is invalid";
    return std::nullopt;
  }
  const auto prefix = longNotePrefix(input.longNoteMode,
                                     input.hasUndefinedLongNotes, diagnostic);
  if (!prefix) {
    return std::nullopt;
  }

  std::string result(*prefix);
  result.reserve(prefix->size() + input.stageSha256.size() * 10 +
                 input.beatorajaConstraintIds.size() * 2 + 1);
  for (const auto &sha256 : input.stageSha256) {
    if (!isCanonicalLowerHex(sha256, 64)) {
      diagnostic =
          "Replay course stage SHA-256 must be canonical lowercase hex";
      return std::nullopt;
    }
    result.append(sha256, 0, 10);
  }

  std::string constraints;
  constraints.reserve(input.beatorajaConstraintIds.size() * 2);
  for (const int id : input.beatorajaConstraintIds) {
    if (id < kBeatorajaFirstConstraintId || id > kBeatorajaLastConstraintId) {
      diagnostic = "Replay course constraint ID is invalid";
      return std::nullopt;
    }
    if (id > kBeatorajaLastExcludedCourseMarkerId) {
      appendTwoDigits(constraints, id);
    }
  }
  if (!constraints.empty()) {
    result.push_back('_');
    result.append(constraints);
  }
  if (result.size() >
      limits.maxFilenameBytes -
          std::min(limits.maxFilenameBytes, kMaximumHistorySuffixBytes)) {
    diagnostic = "Replay course filename exceeds the filesystem limit";
    return std::nullopt;
  }
  return result;
}

bool courseStemMatches(std::string_view stem, CoursePathInput input,
                       std::optional<bool> hasUndefinedLongNotes,
                       std::string &diagnostic, const ReplayLimits &limits) {
  input.hasUndefinedLongNotes = hasUndefinedLongNotes.value_or(false);
  const auto expected = courseStem(input, diagnostic, limits);
  if (!expected || *expected == stem || hasUndefinedLongNotes.has_value()) {
    return expected && *expected == stem;
  }
  input.hasUndefinedLongNotes = true;
  const auto prefixed = courseStem(input, diagnostic, limits);
  return prefixed && *prefixed == stem;
}

std::optional<ReplayPathIdentity> pathForStem(std::string_view stem,
                                              std::int64_t historyIndex,
                                              std::string &diagnostic,
                                              const ReplayLimits &limits) {
  diagnostic.clear();
  if (!limits.valid() || !isCanonicalStem(stem, limits)) {
    diagnostic = "Replay filename stem is not canonical";
    return std::nullopt;
  }
  if (historyIndex < 0) {
    diagnostic = "Replay history index cannot be negative";
    return std::nullopt;
  }
  std::string filename(stem);
  if (historyIndex > 0) {
    filename.push_back('_');
    filename.append(std::to_string(historyIndex));
  }
  filename.append(".brd");
  if (filename.size() > limits.maxFilenameBytes) {
    diagnostic = "Replay filename exceeds the filesystem limit";
    return std::nullopt;
  }
  return ReplayPathIdentity{
      .stem = std::string(stem),
      .historyIndex = historyIndex,
      .relativePath = std::filesystem::path("replay") / filename,
  };
}

bool isCanonicalReplayRelativePath(const std::filesystem::path &relativePath,
                                   std::string &diagnostic,
                                   const ReplayLimits &limits) {
  diagnostic.clear();
  if (!limits.valid() || relativePath.empty() || relativePath.is_absolute() ||
      relativePath.has_root_path() ||
      relativePath.lexically_normal() != relativePath ||
      relativePath.parent_path() != "replay") {
    diagnostic = "Replay path is outside the contained replay directory";
    return false;
  }
  const std::string filename = relativePath.filename().string();
  if (filename.empty() || filename.size() > limits.maxFilenameBytes ||
      !filename.ends_with(".brd") || filename.find('/') != std::string::npos ||
      filename.find('\\') != std::string::npos) {
    diagnostic = "Replay filename is not canonical";
    return false;
  }

  std::string_view stem(filename);
  stem.remove_suffix(kReplayExtensionBytes);
  std::string rebuildDiagnostic;
  if (const auto direct = pathForStem(stem, 0, rebuildDiagnostic, limits);
      direct && direct->relativePath == relativePath) {
    return true;
  }

  const std::size_t separator = stem.rfind('_');
  if (separator != std::string_view::npos && separator + 1 < stem.size()) {
    const std::string_view historyText = stem.substr(separator + 1);
    std::int64_t historyIndex = 0;
    const auto [end, error] =
        std::from_chars(historyText.data(),
                        historyText.data() + historyText.size(), historyIndex);
    if (error == std::errc{} &&
        end == historyText.data() + historyText.size() && historyIndex > 0) {
      if (const auto history =
              pathForStem(stem.substr(0, separator), historyIndex,
                          rebuildDiagnostic, limits);
          history && history->relativePath == relativePath) {
        return true;
      }
    }
  }
  diagnostic = "Replay path does not match the Beatoraja path grammar";
  return false;
}

} // namespace replay
