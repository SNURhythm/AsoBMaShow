#include "ArchiveDecision.h"

#include "../BmsChartFile.h"
#include "../CanonicalDigest.h"

#include <algorithm>
#include <cctype>
#include <limits>
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

} // namespace

std::optional<bool> matchesArchiveChartHash(
    const std::vector<unsigned char> &bytes, const std::string &key,
    archive_file::PauseCallback pauseCallback) {
  const bool matchSha256 = canonical_digest::isCanonicalLowerHex(key, 64);
  const bool matchMd5 = canonical_digest::isCanonicalLowerHex(key, 32);
  bms_parser::SHA256 sha256;
  bms_parser::MD5 md5;
  sha256.init();
  for (std::size_t offset = 0; offset < bytes.size();) {
    if (pauseCallback && !pauseCallback()) return std::nullopt;
    const auto count =
        std::min(archiveVerificationChunkBytes, bytes.size() - offset);
    if (matchSha256) {
      sha256.update(bytes.data() + offset, static_cast<unsigned int>(count));
    }
    if (matchMd5) {
      md5.update(bytes.data() + offset, static_cast<unsigned int>(count));
    }
    offset += count;
  }
  if (pauseCallback && !pauseCallback()) return std::nullopt;
  if (matchMd5) return md5.finalize().hexdigest() == key;
  if (matchSha256) {
    std::array<unsigned char, bms_parser::SHA256::DIGEST_SIZE> digest{};
    sha256.final(digest.data());
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto byte : digest) {
      text.push_back(hex[byte >> 4]);
      text.push_back(hex[byte & 15]);
    }
    return text == key;
  }
  return true;
}

ArchiveReaderDependencies defaultArchiveReaderDependencies() {
  return {
      .listEntries =
          [](const std::filesystem::path &archivePath,
             std::vector<archive_file::Entry> &entries,
             std::uint64_t maximumEntries,
             std::string *errorMessage,
             archive_file::PauseCallback pauseCallback) {
            return archive_file::listEntriesBounded(
                archivePath, entries, maximumEntries, errorMessage,
                std::move(pauseCallback));
          },
      .readMember =
          [](const std::filesystem::path &archivePath,
             const std::filesystem::path &innerPath, std::size_t maximumBytes,
             std::vector<unsigned char> &bytes,
             std::string *errorMessage,
             archive_file::PauseCallback pauseCallback) {
            std::string error;
            if (archive_file::readFileBoundedWithCheckpoint(
                    archive_file::makeVirtualPath(archivePath, innerPath),
                    bytes, maximumBytes, &error, {}, std::move(pauseCallback))) {
              return ArchiveMemberReadResult::Read;
            }
            if (errorMessage) *errorMessage = error;
            if (error == "ZIP entry is not supported by direct reader." ||
                error == "Archive format has no bounded reader available.") {
              return ArchiveMemberReadResult::Unavailable;
            }
            return ArchiveMemberReadResult::Failed;
          }};
}

DirectArchiveDecision decideDownloadedArchive(
    const std::filesystem::path &archivePath, const std::string &archiveKey,
    bool skipUnarchivingForNonSolidArchives,
    archive_file::PauseCallback pauseCallback,
    const ArchiveReaderDependencies &reader, ArchiveVerificationLimits limits) {
  if (!skipUnarchivingForNonSolidArchives) {
    return {};
  }
  std::atomic_bool cancelled = false;
  const auto checkpoint = [pauseCallback, &cancelled] {
    if (cancelled.load()) return false;
    if (pauseCallback && !pauseCallback()) cancelled.store(true);
    return !cancelled.load();
  };
  const auto failure = [&](const std::string &message) {
    return DirectArchiveDecision{
        .disposition = DirectArchiveDisposition::Failed,
        .message = cancelled.load() ? "Archive verification cancelled." : message};
  };
  if (!checkpoint()) return failure({});
  if (!reader.listEntries || !reader.readMember) {
    return {.message = "Archive inspection is unavailable; unarchiving."};
  }

  std::vector<archive_file::Entry> entries;
  std::string listError;
  if (!reader.listEntries(archivePath, entries, limits.maxEntries, &listError,
                           checkpoint)) {
    if (!checkpoint()) return failure({});
    if (listError.find("limit") != std::string::npos) return failure(listError);
    return {.message = listError.empty()
                           ? "Could not inspect the archive; unarchiving."
                           : listError};
  }

  std::vector<std::filesystem::path> bmsPaths;
  bool foundRegularFile = false;
  bool solid = false;
  std::uint64_t declaredBytes = 0;
  if (entries.size() > limits.maxEntries) {
    return failure("Archive exceeds the BMS verification entry-count limit.");
  }
  for (const auto &entry : entries) {
    if (!checkpoint()) return failure({});
    if (entry.directory) {
      continue;
    }
    foundRegularFile = true;
    if (entry.solid) {
      solid = true;
    }
    if (asobmshow::bms_chart_file::isBmsChartPath(entry.path)) {
      if (entry.size > limits.maxMemberBytes ||
          entry.size > limits.maxTotalBytes - declaredBytes) {
        return failure("Archive exceeds the BMS verification byte limit.");
      }
      declaredBytes += entry.size;
      bmsPaths.push_back(entry.path);
    }
  }
  if (solid) return {.message = "Solid archive detected; unarchiving."};
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

  std::uint64_t actualBytes = 0;
  bool matched = false;
  for (const auto &path : bmsPaths) {
    if (!checkpoint()) return failure({});
    std::vector<unsigned char> bytes;
    std::string readError;
    const auto maximumBytes = static_cast<std::size_t>(std::min({
        limits.maxMemberBytes, limits.maxTotalBytes - actualBytes,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())}));
    const auto read = reader.readMember(archivePath, path, maximumBytes, bytes,
                                        &readError, checkpoint);
    if (!checkpoint()) return failure({});
    if (read == ArchiveMemberReadResult::Failed || bytes.size() > maximumBytes) {
      return failure(readError.empty()
                         ? "BMS verification read failed or exceeded its limit."
                         : readError);
    }
    if (read == ArchiveMemberReadResult::Unavailable) {
      return {.foundBmsFile = true,
              .message = "Bounded direct reader unavailable; unarchiving.",
              .verificationBytes = actualBytes};
    }
    actualBytes += bytes.size();
    const auto match = matchesArchiveChartHash(bytes, key, checkpoint);
    if (!match) return failure({});
    matched = matched || *match;
  }
  if (matched) {
    return {.disposition = DirectArchiveDisposition::KeepArchive,
            .foundBmsFile = true,
            .message = "Downloaded BMS archive.",
            .verificationBytes = actualBytes};
  }

  return {.disposition = DirectArchiveDisposition::HashMismatch,
          .foundBmsFile = true,
          .message = "Archive did not contain the selected BMS chart."};
}

} // namespace asobmshow::bms_search
