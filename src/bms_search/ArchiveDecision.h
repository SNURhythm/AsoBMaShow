#pragma once

#include "../ArchiveFile.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace asobmshow::bms_search {

enum class DirectArchiveDisposition { KeepArchive, Unarchive, HashMismatch };

struct DirectArchiveDecision {
  DirectArchiveDisposition disposition = DirectArchiveDisposition::Unarchive;
  bool foundBmsFile = false;
  std::string message;
};

struct ArchiveReaderDependencies {
  std::function<bool(const std::filesystem::path &,
                     std::vector<archive_file::Entry> &, std::string *,
                     archive_file::PauseCallback)>
      listEntries;
  std::function<bool(const std::filesystem::path &,
                     const std::vector<std::filesystem::path> &,
                     std::vector<archive_file::FileData> &, std::string *,
                     archive_file::PauseCallback)>
      readEntries;
};

ArchiveReaderDependencies defaultArchiveReaderDependencies();

DirectArchiveDecision decideDownloadedArchive(
    const std::filesystem::path &archivePath, const std::string &archiveKey,
    bool skipUnarchivingForNonSolidArchives,
    archive_file::PauseCallback pauseCallback,
    const ArchiveReaderDependencies &reader);

} // namespace asobmshow::bms_search
