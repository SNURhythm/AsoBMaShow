#include "DownloadStorageIdentity.h"

#include "../ArchiveFile.h"
#include "../bms_parser.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace asobmshow::bms_search {
namespace {

constexpr std::size_t kMaximumArchiveNameBytes = 128;
constexpr std::size_t kStorageIdLength = 16;

std::string asciiLower(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (unsigned char character : value) {
    result.push_back(static_cast<char>(std::tolower(character)));
  }
  return result;
}

std::string sanitizeStorageBase(std::string_view value,
                                std::size_t maximumBytes) {
  std::string result;
  result.reserve(std::min(value.size(), maximumBytes));
  for (unsigned char character : value) {
    if (character >= 0x80 || std::isalnum(character) || character == '-' ||
        character == '_' || character == '.' || character == ' ' ||
        character == '[' || character == ']' || character == '(' ||
        character == ')') {
      result.push_back(static_cast<char>(character));
    } else if (!result.empty() && result.back() != '_') {
      result.push_back('_');
    }
    if (result.size() >= maximumBytes) {
      break;
    }
  }
  while (!result.empty() &&
         (result.back() == '_' || result.back() == ' ' ||
          result.back() == '.')) {
    result.pop_back();
  }
  return result.empty() ? "archive" : result;
}

std::string storageId(std::string_view identitySeed,
                      std::string_view archiveName) {
  const std::string_view seed =
      identitySeed.empty() ? archiveName : identitySeed;
  const std::vector<unsigned char> bytes(seed.begin(), seed.end());
  return bms_parser::sha256(bytes).substr(0, kStorageIdLength);
}

} // namespace

FindBmsStorageNames findBmsStorageNames(
    std::string_view archiveName, std::string_view fallbackExtension,
    std::string_view identitySeed) {
  const std::string nameExtension =
      archive_file::archiveExtensionFromName(archiveName);
  std::string extension = nameExtension;
  if (extension.empty() &&
      archive_file::isRecognizedArchiveExtension(fallbackExtension)) {
    extension = asciiLower(fallbackExtension);
  }
  if (extension.empty()) {
    extension = ".archive";
  }

  std::string_view base = archiveName;
  if (!nameExtension.empty()) {
    base.remove_suffix(nameExtension.size());
  }
  const std::string id = storageId(identitySeed, archiveName);
  const std::size_t fixedBytes = 2 + id.size() + extension.size();
  const std::size_t maximumBaseBytes =
      kMaximumArchiveNameBytes > fixedBytes
          ? kMaximumArchiveNameBytes - fixedBytes
          : 1;
  const std::string safeBase =
      sanitizeStorageBase(base, maximumBaseBytes);
  const std::string storageKey = safeBase + "--" + id;
  return {.storageKey = storageKey,
          .archiveName = storageKey + extension};
}

} // namespace asobmshow::bms_search
