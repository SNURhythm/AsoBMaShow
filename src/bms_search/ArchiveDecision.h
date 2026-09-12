#pragma once

#include "../ArchiveFile.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace asobmshow::bms_search {

enum class DirectArchiveDisposition { KeepArchive, Unarchive, HashMismatch, Failed };

struct ArchiveVerificationLimits {
  std::uint64_t maxMemberBytes = 16ULL * 1024 * 1024;
  std::uint64_t maxTotalBytes = 256ULL * 1024 * 1024;
  std::uint64_t maxEntries = 100000;
};

inline constexpr std::size_t archiveVerificationChunkBytes = 64 * 1024;

enum class ArchiveMemberReadResult { Read, Unavailable, Failed };

std::optional<bool> matchesArchiveChartHash(
    const std::vector<unsigned char> &bytes, const std::string &key,
    archive_file::PauseCallback pauseCallback);

struct DirectArchiveDecision {
  DirectArchiveDisposition disposition = DirectArchiveDisposition::Unarchive;
  bool foundBmsFile = false;
  std::string message;
  std::uint64_t verificationBytes = 0;
};

struct ArchiveReaderDependencies {
  std::function<bool(const std::filesystem::path &,
                     std::vector<archive_file::Entry> &, std::uint64_t, std::string *,
                     archive_file::PauseCallback)>
      listEntries;
  std::function<ArchiveMemberReadResult(const std::filesystem::path &,
                     const std::filesystem::path &, std::size_t,
                     std::vector<unsigned char> &, std::string *,
                     archive_file::PauseCallback)>
      readMember;
};

ArchiveReaderDependencies defaultArchiveReaderDependencies();

DirectArchiveDecision decideDownloadedArchive(
    const std::filesystem::path &archivePath, const std::string &archiveKey,
    bool skipUnarchivingForNonSolidArchives,
    archive_file::PauseCallback pauseCallback,
    const ArchiveReaderDependencies &reader,
    ArchiveVerificationLimits limits = {});

} // namespace asobmshow::bms_search
