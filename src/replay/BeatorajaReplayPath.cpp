#include "BeatorajaReplayPath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace replay {
namespace {

constexpr std::array<std::string_view, 3> kLongNotePrefixes = {"", "C", "H"};

bool isLowerSha256(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::optional<std::string_view> longNotePrefix(int longNoteMode,
                                               bool hasUndefinedLongNotes,
                                               std::string &diagnostic) {
  if (!hasUndefinedLongNotes) {
    return std::string_view{};
  }
  if (longNoteMode < 0 ||
      longNoteMode >= static_cast<int>(kLongNotePrefixes.size())) {
    diagnostic = "Unsupported long-note mode for replay path";
    return std::nullopt;
  }
  return kLongNotePrefixes[static_cast<std::size_t>(longNoteMode)];
}

bool isCanonicalStem(std::string_view stem) {
  if (stem.empty()) {
    return false;
  }
  if (stem.front() == 'C' || stem.front() == 'H') {
    stem.remove_prefix(1);
  }
  const std::size_t separator = stem.find('_');
  const std::string_view hashPart = stem.substr(0, separator);
  if (hashPart.size() < 10 ||
      !std::ranges::all_of(hashPart, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return false;
  }
  if (separator == std::string_view::npos) {
    return true;
  }
  const std::string_view constraintPart = stem.substr(separator + 1);
  return !constraintPart.empty() && constraintPart.size() % 2 == 0 &&
         std::ranges::all_of(constraintPart, [](unsigned char ch) {
           return ch >= '0' && ch <= '9';
         });
}

void appendTwoDigits(std::string &value, int number) {
  value.push_back(static_cast<char>('0' + number / 10));
  value.push_back(static_cast<char>('0' + number % 10));
}

} // namespace

std::optional<std::string>
chartStem(std::string_view lowerSha256, int longNoteMode,
          bool hasUndefinedLongNotes, std::string &diagnostic) {
  diagnostic.clear();
  if (!isLowerSha256(lowerSha256)) {
    diagnostic = "Replay chart SHA-256 must be canonical lowercase hex";
    return std::nullopt;
  }
  const auto prefix =
      longNotePrefix(longNoteMode, hasUndefinedLongNotes, diagnostic);
  if (!prefix) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(prefix->size() + lowerSha256.size());
  result.append(*prefix);
  result.append(lowerSha256);
  return result;
}

std::optional<std::string> courseStem(const CoursePathInput &input,
                                      std::string &diagnostic) {
  diagnostic.clear();
  if (input.stageSha256.empty() ||
      input.stageSha256.size() > kMaximumCourseReplayStages) {
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
  for (const std::string &sha256 : input.stageSha256) {
    if (!isLowerSha256(sha256)) {
      diagnostic =
          "Replay course stage SHA-256 must be canonical lowercase hex";
      return std::nullopt;
    }
    result.append(sha256, 0, 10);
  }

  std::string constraints;
  constraints.reserve(input.beatorajaConstraintIds.size() * 2);
  for (const int id : input.beatorajaConstraintIds) {
    if (id < kBeatorajaFirstConstraintId ||
        id > kBeatorajaLastConstraintId) {
      diagnostic = "Replay course constraint ID is invalid";
      return std::nullopt;
    }
    if (id <= kBeatorajaLastExcludedCourseMarkerId) {
      continue;
    }
    appendTwoDigits(constraints, id);
  }
  if (!constraints.empty()) {
    result.push_back('_');
    result.append(constraints);
  }
  return result;
}

std::optional<ReplayPathIdentity>
pathForStem(std::string_view stem, std::int64_t historyIndex,
            std::string &diagnostic) {
  diagnostic.clear();
  if (!isCanonicalStem(stem)) {
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

  return ReplayPathIdentity{
      .stem = std::string(stem),
      .historyIndex = historyIndex,
      .relativePath = std::filesystem::path("replay") / filename,
  };
}

} // namespace replay
