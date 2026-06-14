#include "HorieYuukaDriver.h"

namespace asobmshow::bms_search {

std::vector<std::string> HorieYuukaDriver::searchQueries(
    const std::string &title, const std::string &artist,
    const std::string &sha256, const std::string &md5) {
  std::vector<std::string> queries;
  const std::string trimmedTitle = trimCopy(title);
  const std::string minimalTitle = stripTitleDecorations(trimmedTitle);
  const auto artistQueries = horieArtistQueryVariants(artist);

  auto appendArtistTitleQueries = [&](const std::string &titleQuery) {
    if (titleQuery.empty()) {
      return;
    }
    for (const auto &artistQuery : artistQueries) {
      if (!artistQuery.empty()) {
        appendUniqueQuery(queries, "[" + artistQuery + "] " + titleQuery);
      }
    }
  };

  appendArtistTitleQueries(trimmedTitle);
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendArtistTitleQueries(minimalTitle);
  }

  appendUniqueQuery(queries, trimmedTitle);
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendUniqueQuery(queries, minimalTitle);
  }

  appendUniqueQuery(queries, normalizedSearchText(trimmedTitle));
  if (normalizedSearchText(minimalTitle) != normalizedSearchText(trimmedTitle)) {
    appendUniqueQuery(queries, normalizedSearchText(minimalTitle));
  }

  auto titleTokens = splitSearchTokens(trimmedTitle);
  std::sort(titleTokens.begin(), titleTokens.end(),
            [](const std::string &lhs, const std::string &rhs) {
              if (lhs.size() != rhs.size()) {
                return lhs.size() > rhs.size();
              }
              return lhs < rhs;
            });
  int addedTitleTokens = 0;
  for (const auto &token : titleTokens) {
    if (!isMeaningfulSearchToken(token)) {
      continue;
    }
    appendUniqueQuery(queries, token);
    if (++addedTitleTokens >= 6) {
      break;
    }
  }

  appendUniqueQuery(queries, sha256);
  appendUniqueQuery(queries, md5);
  return queries;
}

bool artistMatchesArchiveResult(const json &item, const std::string &artist) {
  const auto artistVariants = horieArtistQueryVariants(artist);
  if (artistVariants.empty()) {
    return false;
  }

  const std::string haystack = normalizedSearchText(
      jsonStringAt(item, "title") + " " + jsonStringAt(item, "name") + " " +
      jsonStringAt(item, "relativePath"));
  for (const auto &variant : artistVariants) {
    const std::string normalizedArtist = normalizedSearchText(variant);
    if (!normalizedArtist.empty() &&
        haystack.find(normalizedArtist) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool titleMatchesArchiveResult(const json &item, const std::string &title) {
  const std::string rawTitle = trimCopy(title);
  const std::string minimalTitle = stripTitleDecorations(rawTitle);
  const std::string normalizedTitle = normalizedSearchText(title);
  const std::string normalizedMinimalTitle =
      normalizedSearchText(minimalTitle);
  if (requiresExactTitleTokenMatch(normalizedTitle) ||
      requiresExactTitleTokenMatch(normalizedMinimalTitle)) {
    const auto nameParts = parseHorieArchiveName(item);
    const std::string archiveTitle =
        normalizedSearchText(stripTitleDecorations(nameParts.title));
    return (!normalizedTitle.empty() && archiveTitle == normalizedTitle) ||
           (!normalizedMinimalTitle.empty() &&
            archiveTitle == normalizedMinimalTitle);
  }

  const std::string rawHaystack =
      jsonStringAt(item, "title") + " " + jsonStringAt(item, "name") + " " +
      jsonStringAt(item, "relativePath");
  if (allowsRawSubstringTitleMatch(normalizedTitle) && rawTitle.size() >= 4 &&
      lowerCopy(rawHaystack).find(lowerCopy(rawTitle)) != std::string::npos) {
    return true;
  }
  if (allowsRawSubstringTitleMatch(normalizedMinimalTitle) &&
      minimalTitle.size() >= 4 &&
      lowerCopy(rawHaystack).find(lowerCopy(minimalTitle)) !=
          std::string::npos) {
    return true;
  }

  if (normalizedTitle.size() < 4 && !containsNonAscii(normalizedTitle) &&
      normalizedMinimalTitle.size() < 4 &&
      !containsNonAscii(normalizedMinimalTitle)) {
    return false;
  }
  const std::string haystack = normalizedSearchText(rawHaystack);
  if (normalizedHaystackMatchesTitle(haystack, normalizedTitle)) {
    return true;
  }
  if (normalizedHaystackMatchesTitle(haystack, normalizedMinimalTitle)) {
    return true;
  }

  const auto titleTokens = splitSearchTokens(
      normalizedMinimalTitle.empty() ? normalizedTitle : normalizedMinimalTitle);
  if (requiresExactTitleTokenMatch(normalizedMinimalTitle.empty()
                                       ? normalizedTitle
                                       : normalizedMinimalTitle)) {
    return false;
  }
  std::vector<std::string> meaningfulTokens;
  for (const auto &token : titleTokens) {
    if (isMeaningfulSearchToken(token)) {
      meaningfulTokens.push_back(token);
    }
  }
  if (meaningfulTokens.empty()) {
    return false;
  }

  int matchedTokens = 0;
  for (const auto &token : meaningfulTokens) {
    if (haystack.find(token) != std::string::npos) {
      ++matchedTokens;
    }
  }
  if (meaningfulTokens.size() == 1) {
    return matchedTokens == 1;
  }
  const int requiredMatches =
      std::max(2, static_cast<int>((meaningfulTokens.size() * 2 + 2) / 3));
  return matchedTokens >= requiredMatches;
}

std::string HorieYuukaDriver::searchUrl(const std::string &folder,
                                        const std::string &query) {
  return std::string(kHorieApiOrigin) + "/api/v1/folders/" + folder +
         "/files?limit=5&offset=0&q=" + urlEncode(query);
}

BmsSearchCandidate HorieYuukaDriver::candidateFromJson(
    const json &item, const std::string &query, const std::string &sourceUrl) {
  BmsSearchCandidate candidate;
  candidate.source = BmsSearchCandidate::Source::Horie;
  candidate.id = jsonStringAt(item, "id");
  candidate.name = jsonStringAt(item, "name");
  const auto nameParts = parseHorieArchiveName(item);
  candidate.title = nameParts.title;
  candidate.artist = nameParts.artist;
  candidate.query = query;
  candidate.sourceUrl = sourceUrl;
  return candidate;
}

HorieCandidateSearchResult HorieYuukaDriver::findCandidates(
    const std::string &query, const std::string &title,
    const std::string &artist, bool requireTitleMatch,
    bool requireArtistMatch) {
  HorieCandidateSearchResult result;
  for (const char *folder : {"Songs"}) {
    result.sourceUrl = searchUrl(folder, query);
    const auto body = fetchUrlText(result.sourceUrl, result.errorMessage);
    if (!body) {
      continue;
    }

    json payload;
    try {
      payload = json::parse(*body);
    } catch (const std::exception &e) {
      result.errorMessage = std::string("Horie archive returned invalid JSON: ") +
                            e.what();
      continue;
    }
    auto itemsIt = payload.find("items");
    if (itemsIt == payload.end() || !itemsIt->is_array()) {
      itemsIt = payload.find("files");
    }
    if (itemsIt == payload.end() || !itemsIt->is_array()) {
      continue;
    }

    for (const auto &item : *itemsIt) {
      const std::string name = jsonStringAt(item, "name");
      const std::string contentType = jsonStringAt(item, "contentType");
      const std::string archiveExtension = archiveExtensionFromName(name);
      const std::string normalizedContentType =
          lowerCopy(trimCopy(contentType));
      const bool isArchive =
          isSupportedArchiveExtension(archiveExtension) ||
          normalizedContentType == "application/zip" ||
          (ASOBMSHOW_HAS_LIBARCHIVE &&
           isArchiveContentType(normalizedContentType));
      if (!isArchive) {
        continue;
      }
      if (requireTitleMatch && !titleMatchesArchiveResult(item, title)) {
        continue;
      }
      if (requireArtistMatch && !artistMatchesArchiveResult(item, artist)) {
        continue;
      }
      result.candidates.push_back(item);
    }
  }
  return result;
}

bool HorieYuukaDriver::tryDownload(
    const std::vector<std::string> &queries, const std::string &title,
    const std::string &artist, bool requireTitleMatch,
    const std::string &archiveKey, const std::filesystem::path &libraryRoot,
    std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result) {
  std::optional<std::string> lastError;

  for (const auto &query : queries) {
    const std::string trimmedQuery = trimCopy(query);
    if (cancelled.load()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Lookup cancelled.";
      return true;
    }
    if (trimmedQuery.empty()) {
      continue;
    }
    if (progressCallback) {
      progressCallback({.message = "Searching Horie archive"});
    }

    const bool shouldRequireTitleMatch = requireTitleMatch;
    const bool shouldRequireArtistMatch =
        shouldRequireTitleMatch && !trimCopy(artist).empty() &&
        titleNeedsExactCandidateMatch(title);
    const auto searchResult = findCandidates(
        trimmedQuery, title, artist, shouldRequireTitleMatch,
        shouldRequireArtistMatch);
    if (searchResult.candidates.empty()) {
      if (!searchResult.errorMessage.empty()) {
        lastError = searchResult.errorMessage;
      }
      continue;
    }

    std::set<std::string> seenFileIds;
    result.candidates.clear();
    for (const auto &item : searchResult.candidates) {
      auto candidate =
          candidateFromJson(item, trimmedQuery, searchResult.sourceUrl);
      if (candidate.id.empty() || seenFileIds.contains(candidate.id)) {
        continue;
      }
      seenFileIds.insert(candidate.id);
      result.candidates.push_back(std::move(candidate));
    }

    if (result.candidates.empty()) {
      result.status = BmsSearchResult::Status::DownloadFailed;
      result.message = "Horie archive returned candidates without file IDs.";
      return true;
    }

    if (result.candidates.size() > 1) {
      result.status = BmsSearchResult::Status::AmbiguousCandidates;
      result.message = "Horie found multiple matching archives.";
      return true;
    }

    if (progressCallback) {
      progressCallback({.message = "Preparing Horie archive download"});
    }
    return downloadCandidateById(result.candidates.front(), archiveKey,
                                 libraryRoot, cancelled, progressCallback,
                                 result);
  }

  result.status = BmsSearchResult::Status::NotFound;
  result.message = lastError.value_or("Horie did not find a matching BMS archive.");
  return false;
}

bool HorieYuukaDriver::downloadCandidateById(
    const BmsSearchCandidate &candidate, const std::string &archiveKey,
    const std::filesystem::path &libraryRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progressCallback,
    BmsSearchResult &result) {
  if (candidate.source != BmsSearchCandidate::Source::Horie ||
      candidate.id.empty()) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Selected download candidate is not valid.";
    return true;
  }

  const std::string grantUrl =
      std::string(kHorieApiOrigin) + "/api/v1/files/" + candidate.id +
      "/download-grants";
  std::string grantError;
  const auto grantBody = postUrlText(grantUrl, grantError);
  if (!grantBody) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        grantError.empty() ? "Horie archive grant failed." : grantError;
    return true;
  }

  json grant;
  try {
    grant = json::parse(*grantBody);
  } catch (const std::exception &e) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message =
        std::string("Horie archive grant returned invalid JSON: ") + e.what();
    return true;
  }

  const std::string grantDownloadUrl = jsonStringAt(grant, "downloadUrl");
  if (grantDownloadUrl.empty()) {
    result.status = BmsSearchResult::Status::DownloadFailed;
    result.message = "Horie archive did not return a download URL.";
    return true;
  }

  const std::string absoluteUrl = resolveUrl(kHorieApiOrigin, grantDownloadUrl);
  downloadAndExtractArchive(absoluteUrl, absoluteUrl, archiveKey, libraryRoot,
                            cancelled, progressCallback, result,
                            candidate.name);
  result.candidates = {candidate};
  return true;
}

} // namespace asobmshow::bms_search
