#include "ArchiveDecision.h"

#include "../BmsChartFile.h"
#include "../CanonicalDigest.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace asobmshow::bms_search {
namespace {

std::string normalizedKey(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  const auto last = std::find_if_not(
                        value.rbegin(), value.rend(),
                        [](unsigned char character) {
                          return std::isspace(character) != 0;
                        })
                        .base();
  if (first >= last) {
    return {};
  }
  std::string result(first, last);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

bool completeRead(const std::vector<std::filesystem::path> &requested,
                  const std::vector<archive_file::FileData> &files) {
  if (requested.size() != files.size()) {
    return false;
  }
  std::vector<bool> matched(files.size(), false);
  for (const auto &path : requested) {
    bool found = false;
    for (std::size_t index = 0; index < files.size(); ++index) {
      if (!matched[index] && files[index].path == path) {
        matched[index] = true;
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

} // namespace

ArchiveReaderDependencies defaultArchiveReaderDependencies() {
  return {
      .listEntries =
          [](const std::filesystem::path &archivePath,
             std::vector<archive_file::Entry> &entries,
             std::string *errorMessage,
             archive_file::PauseCallback pauseCallback) {
            return archive_file::listEntries(archivePath, entries, errorMessage,
                                             std::move(pauseCallback));
          },
      .readEntries =
          [](const std::filesystem::path &archivePath,
             const std::vector<std::filesystem::path> &innerPaths,
             std::vector<archive_file::FileData> &files,
             std::string *errorMessage,
             archive_file::PauseCallback pauseCallback) {
            return archive_file::readArchiveEntries(
                archivePath, innerPaths, files, errorMessage,
                std::move(pauseCallback));
          }};
}

DirectArchiveDecision decideDownloadedArchive(
    const std::filesystem::path &archivePath, const std::string &archiveKey,
    bool skipUnarchivingForNonSolidArchives,
    archive_file::PauseCallback pauseCallback,
    const ArchiveReaderDependencies &reader) {
  if (!skipUnarchivingForNonSolidArchives) {
    return {};
  }
  if (!reader.listEntries || !reader.readEntries) {
    return {.message = "Archive inspection is unavailable; unarchiving."};
  }

  std::vector<archive_file::Entry> entries;
  std::string listError;
  if (!reader.listEntries(archivePath, entries, &listError, pauseCallback)) {
    return {.message = listError.empty()
                           ? "Could not inspect the archive; unarchiving."
                           : listError};
  }

  std::vector<std::filesystem::path> bmsPaths;
  bool foundRegularFile = false;
  for (const auto &entry : entries) {
    if (entry.directory) {
      continue;
    }
    foundRegularFile = true;
    if (entry.solid) {
      return {.message = "Solid archive detected; unarchiving."};
    }
    if (asobmshow::bms_chart_file::isBmsChartPath(entry.path)) {
      bmsPaths.push_back(entry.path);
    }
  }
  if (!foundRegularFile) {
    return {.message = "Archive listing was empty; unarchiving."};
  }

  const std::string key = normalizedKey(archiveKey);
  const bool matchSha256 =
      canonical_digest::isCanonicalLowerHex(key, 64);
  const bool matchMd5 = canonical_digest::isCanonicalLowerHex(key, 32);
  if (!matchSha256 && !matchMd5 && bmsPaths.empty()) {
    return {.disposition = DirectArchiveDisposition::KeepArchive,
            .foundBmsFile = false,
            .message = "Archive kept, but no BMS file was found."};
  }
  if (bmsPaths.empty()) {
    return {.disposition = DirectArchiveDisposition::HashMismatch,
            .message = "Archive did not contain a BMS chart file."};
  }

  std::vector<archive_file::FileData> files;
  std::string readError;
  if (!reader.readEntries(archivePath, bmsPaths, files, &readError,
                          pauseCallback) ||
      !completeRead(bmsPaths, files)) {
    return {.foundBmsFile = true,
            .message = readError.empty()
                           ? "Could not read every BMS entry; unarchiving."
                           : readError};
  }

  if (!matchSha256 && !matchMd5) {
    return {.disposition = DirectArchiveDisposition::KeepArchive,
            .foundBmsFile = true,
            .message = "Downloaded BMS archive."};
  }

  for (const auto &file : files) {
    if (matchSha256 && bms_parser::sha256(file.bytes) == key) {
      return {.disposition = DirectArchiveDisposition::KeepArchive,
              .foundBmsFile = true,
              .message = "Downloaded BMS archive."};
    }
    if (matchMd5) {
      const std::string text(file.bytes.begin(), file.bytes.end());
      if (bms_parser::md5(text) == key) {
        return {.disposition = DirectArchiveDisposition::KeepArchive,
                .foundBmsFile = true,
                .message = "Downloaded BMS archive."};
      }
    }
  }

  return {.disposition = DirectArchiveDisposition::HashMismatch,
          .foundBmsFile = true,
          .message = "Archive did not contain the selected BMS chart."};
}

} // namespace asobmshow::bms_search
