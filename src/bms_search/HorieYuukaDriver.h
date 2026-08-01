#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

class HorieYuukaDriver {
public:
  static std::vector<std::string>
  searchQueries(const std::string &title, const std::string &artist,
                const std::string &sha256, const std::string &md5);
  static bool tryDownload(
      const std::vector<std::string> &queries, const std::string &title,
      const std::string &artist, bool requireTitleMatch,
      const std::string &archiveKey, const std::filesystem::path &libraryRoot,
      std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      const BmsSearchDownloadOptions &options,
      BmsSearchResult &result);
  static bool downloadCandidateById(
      const BmsSearchCandidate &candidate, const std::string &archiveKey,
      const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
      BmsSearchDownloadProgressCallback progressCallback,
      const BmsSearchDownloadOptions &options,
      BmsSearchResult &result);

private:
  static std::string searchUrl(const std::string &folder,
                               const std::string &query);
  static BmsSearchCandidate candidateFromJson(const json &item,
                                              const std::string &query,
                                              const std::string &sourceUrl);
  static HorieCandidateSearchResult
  findCandidates(const std::string &query, const std::string &title,
                 const std::string &artist, bool requireTitleMatch,
                 bool requireArtistMatch);
};

} // namespace asobmshow::bms_search
