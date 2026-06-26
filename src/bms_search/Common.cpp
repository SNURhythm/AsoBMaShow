#include "Internal.h"

#include "../BmsMetadataText.h"

namespace asobmshow::bms_search {

std::string trimCopy(const std::string &value) {
  return asobmshow::bms_metadata::trimCopy(value);
}

std::string lowerCopy(std::string value) {
  return asobmshow::bms_metadata::lowerCopy(std::move(value));
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool hostMatches(const std::string &host, std::string_view domain) {
  return std::string_view(host) == domain ||
         (host.size() > domain.size() &&
          host.ends_with(std::string(".") + std::string(domain)));
}

std::string replaceAll(std::string value, std::string_view needle,
                       std::string_view replacement) {
  if (needle.empty()) {
    return value;
  }
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

std::string htmlDecode(std::string value) {
  value = replaceAll(std::move(value), "&amp;", "&");
  value = replaceAll(std::move(value), "&quot;", "\"");
  value = replaceAll(std::move(value), "&#39;", "'");
  value = replaceAll(std::move(value), "&#x27;", "'");
  value = replaceAll(std::move(value), "&lt;", "<");
  value = replaceAll(std::move(value), "&gt;", ">");
  return value;
}

std::string normalizeEmbeddedUrl(std::string value) {
  value = htmlDecode(std::move(value));
  value = replaceAll(std::move(value), "\\u002F", "/");
  value = replaceAll(std::move(value), "\\/", "/");
  value = replaceAll(std::move(value), "\\u0026", "&");
  value = replaceAll(std::move(value), "\\u003D", "=");
  return value;
}

std::optional<ParsedUrl> parseUrl(const std::string &url) {
  const auto schemePos = url.find("://");
  if (schemePos == std::string::npos) {
    return std::nullopt;
  }
  ParsedUrl parsed;
  parsed.scheme = lowerCopy(url.substr(0, schemePos));
  const size_t authorityStart = schemePos + 3;
  const size_t pathStart = url.find('/', authorityStart);
  std::string authority =
      pathStart == std::string::npos ? url.substr(authorityStart)
                                     : url.substr(authorityStart,
                                                  pathStart - authorityStart);
  const auto atPos = authority.rfind('@');
  if (atPos != std::string::npos) {
    authority = authority.substr(atPos + 1);
  }
  const auto portPos = authority.find(':');
  parsed.host = lowerCopy(portPos == std::string::npos
                              ? authority
                              : authority.substr(0, portPos));

  const size_t queryStart =
      pathStart == std::string::npos ? std::string::npos
                                     : url.find('?', pathStart);
  if (pathStart == std::string::npos) {
    parsed.path = "/";
  } else if (queryStart == std::string::npos) {
    const size_t fragmentStart = url.find('#', pathStart);
    parsed.path = fragmentStart == std::string::npos
                      ? url.substr(pathStart)
                      : url.substr(pathStart, fragmentStart - pathStart);
  } else {
    parsed.path = url.substr(pathStart, queryStart - pathStart);
  }
  if (queryStart != std::string::npos) {
    const size_t fragmentStart = url.find('#', queryStart);
    parsed.query =
        fragmentStart == std::string::npos
            ? url.substr(queryStart + 1)
            : url.substr(queryStart + 1, fragmentStart - queryStart - 1);
  }
  return parsed;
}

bool hasUrlScheme(const std::string &value) {
  const size_t colon = value.find(':');
  if (colon == std::string::npos || colon == 0) {
    return false;
  }
  const size_t firstDelimiter = value.find_first_of("/?#");
  if (firstDelimiter != std::string::npos && firstDelimiter < colon) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  for (size_t i = 1; i < colon; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (std::isalnum(c) == 0 && c != '+' && c != '-' && c != '.') {
      return false;
    }
  }
  return true;
}

std::string trimUrlForBase(std::string url) {
  const auto fragment = url.find('#');
  if (fragment != std::string::npos) {
    url.erase(fragment);
  }
  const auto query = url.find('?');
  if (query != std::string::npos) {
    url.erase(query);
  }
  return url;
}

std::string resolveUrl(const std::string &baseUrl, const std::string &link) {
  if (hasUrlScheme(link)) {
    return link;
  }

  const auto schemePos = baseUrl.find("://");
  if (schemePos == std::string::npos) {
    return link;
  }

  const auto originStart = schemePos + 3;
  const auto pathStart = baseUrl.find('/', originStart);
  const std::string origin =
      pathStart == std::string::npos ? baseUrl : baseUrl.substr(0, pathStart);

  if (link.starts_with("//")) {
    return baseUrl.substr(0, schemePos) + ":" + link;
  }
  if (link.starts_with("/")) {
    return origin + link;
  }

  const std::string cleanBase = trimUrlForBase(baseUrl);
  const auto slash = cleanBase.rfind('/');
  const std::string directory = slash == std::string::npos
                                    ? origin + "/"
                                    : cleanBase.substr(0, slash + 1);
  std::vector<std::string> parts;
  std::stringstream stream(directory.substr(origin.size()) + link);
  std::string part;
  while (std::getline(stream, part, '/')) {
    if (part.empty() || part == ".") {
      continue;
    }
    if (part == "..") {
      if (!parts.empty()) {
        parts.pop_back();
      }
      continue;
    }
    parts.push_back(part);
  }

  std::string resolved = origin;
  for (const auto &urlPart : parts) {
    resolved += "/";
    resolved += urlPart;
  }
  return resolved;
}

std::string queryParam(const ParsedUrl &url, const std::string &name) {
  std::stringstream stream(url.query);
  std::string part;
  while (std::getline(stream, part, '&')) {
    const auto eq = part.find('=');
    const std::string key = eq == std::string::npos ? part : part.substr(0, eq);
    if (key == name) {
      return eq == std::string::npos ? "" : part.substr(eq + 1);
    }
  }
  return "";
}

std::string urlDecode(const std::string &value, bool plusAsSpace) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2])) != 0) {
      const std::string hex = value.substr(i + 1, 2);
      result.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
      i += 2;
    } else if (plusAsSpace && c == '+') {
      result.push_back(' ');
    } else {
      result.push_back(static_cast<char>(c));
    }
  }
  return result;
}

std::string setQueryParameter(std::string url, const std::string &name,
                              const std::string &value,
                              const std::set<std::string> &removeNames) {
  std::string fragment;
  const size_t fragmentStart = url.find('#');
  if (fragmentStart != std::string::npos) {
    fragment = url.substr(fragmentStart);
    url.erase(fragmentStart);
  }

  std::string base = url;
  std::string query;
  const size_t queryStart = url.find('?');
  if (queryStart != std::string::npos) {
    base = url.substr(0, queryStart);
    query = url.substr(queryStart + 1);
  }

  std::vector<std::string> parts;
  std::stringstream stream(query);
  std::string part;
  while (std::getline(stream, part, '&')) {
    if (part.empty()) {
      continue;
    }
    const size_t eq = part.find('=');
    const std::string key = eq == std::string::npos ? part : part.substr(0, eq);
    if (key == name || removeNames.contains(key)) {
      continue;
    }
    parts.push_back(part);
  }
  parts.push_back(name + "=" + urlEncode(value));

  std::string rebuilt = base + "?";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      rebuilt += "&";
    }
    rebuilt += parts[i];
  }
  return rebuilt + fragment;
}

std::string jsonStringAt(const json &object, const char *key,
                         const std::string &fallback) {
  if (!object.is_object()) {
    return fallback;
  }
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) {
    return fallback;
  }
  if (it->is_string()) {
    return it->get<std::string>();
  }
  if (it->is_number_integer()) {
    return std::to_string(it->get<long long>());
  }
  if (it->is_number_unsigned()) {
    return std::to_string(it->get<unsigned long long>());
  }
  return fallback;
}

std::string urlEncode(const std::string &value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      stream << static_cast<char>(c);
    } else if (c == ' ') {
      stream << '+';
    } else {
      stream << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c)
             << std::setfill(' ');
    }
  }
  return stream.str();
}

std::string fileNameFromUrl(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed) {
    return "";
  }
  std::filesystem::path path(parsed->path);
  return lowerCopy(urlDecode(path.filename().string()));
}

std::string displayFileNameFromUrl(const std::string &url) {
  const auto parsed = parseUrl(url);
  if (!parsed) {
    return "";
  }
  std::filesystem::path path(parsed->path);
  return trimCopy(urlDecode(path.filename().string()));
}

std::string archiveExtensionFromName(std::string name) {
  name = lowerCopy(std::move(name));
  static constexpr std::array<std::string_view, 15> kArchiveExtensions = {
      ".tar.bz2", ".tar.gz", ".tar.xz", ".tbz2", ".tgz",
      ".txz",     ".zip",    ".7z",     ".rar",  ".lzh",
      ".lha",     ".tar",    ".bz2",    ".gz",   ".xz",
  };
  for (const std::string_view extension : kArchiveExtensions) {
    if (endsWith(name, extension)) {
      return std::string(extension);
    }
  }
  return "";
}

std::string stripArchiveExtension(std::string name) {
  const std::string extension = archiveExtensionFromName(name);
  if (!extension.empty() && name.size() >= extension.size()) {
    name.erase(name.size() - extension.size());
  }
  return trimCopy(name);
}

std::string safeStorageKey(const std::string &value) {
  std::string result;
  result.reserve(std::min<size_t>(value.size(), 128));
  for (unsigned char c : value) {
    if (c >= 0x80 || std::isalnum(c) || c == '-' || c == '_' || c == '.' ||
        c == ' ' || c == '[' || c == ']' || c == '(' || c == ')') {
      result.push_back(static_cast<char>(c));
    } else if (!result.empty() && result.back() != '_') {
      result.push_back('_');
    }
    if (result.size() >= 128) {
      break;
    }
  }
  while (!result.empty() &&
         (result.back() == '_' || result.back() == ' ' || result.back() == '.')) {
    result.pop_back();
  }
  return result.empty() ? "archive" : result;
}

std::string archiveNameFromUrl(const std::string &url) {
  const std::string name = displayFileNameFromUrl(url);
  return archiveExtensionFromName(name).empty() ? std::string() : name;
}

std::string preferredArchiveName(const std::string &suggestedArchiveName,
                                 const std::string &displayUrl,
                                 const std::string &downloadUrl,
                                 const std::string &archiveKey,
                                 const std::string &archiveExtension) {
  std::string name = trimCopy(suggestedArchiveName);
  if (archiveExtensionFromName(name).empty()) {
    name = archiveNameFromUrl(displayUrl);
  }
  if (archiveExtensionFromName(name).empty()) {
    name = archiveNameFromUrl(downloadUrl);
  }

  if (!name.empty()) {
    return safeStorageKey(name);
  }

  std::string fallback =
      archiveKey.empty()
          ? std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
          : archiveKey;
  fallback = safeStorageKey(fallback);
  return fallback + (archiveExtension.empty() ? ".archive" : archiveExtension);
}

std::string storageKeyFromArchiveName(const std::string &archiveName) {
  std::string key = stripArchiveExtension(archiveName);
  if (key.empty()) {
    key = archiveName;
  }
  return safeStorageKey(key);
}

std::string collapseWhitespace(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isspace(c) != 0) {
      if (!lastWasSpace) {
        result.push_back(' ');
      }
      lastWasSpace = true;
    } else {
      result.push_back(static_cast<char>(c));
      lastWasSpace = false;
    }
  }
  return trimCopy(result);
}

std::string plainTextFromHtmlFragment(const std::string &html) {
  static const std::regex tagPattern(R"(<[^>]*>)", std::regex::icase);
  return collapseWhitespace(
      htmlDecode(std::regex_replace(html, tagPattern, " ")));
}

std::optional<std::string> htmlAttributeValue(const std::string &tag,
                                              const std::string &attribute) {
  const std::regex attributePattern(
      attribute + R"(\s*=\s*["']([^"']*)["'])", std::regex::icase);
  std::smatch match;
  if (std::regex_search(tag, match, attributePattern) && match.size() >= 2) {
    return htmlDecode(match[1].str());
  }
  return std::nullopt;
}

std::string archiveNameFromText(const std::string &text) {
  std::string cleaned = plainTextFromHtmlFragment(text);
  for (const std::string &prefix : {"Download ", "download "}) {
    if (cleaned.starts_with(prefix)) {
      cleaned = trimCopy(cleaned.substr(prefix.size()));
      break;
    }
  }
  return archiveExtensionFromName(cleaned).empty() ? std::string() : cleaned;
}

HorieArchiveNameParts parseHorieArchiveName(const json &item) {
  HorieArchiveNameParts parts;
  std::string name = stripArchiveExtension(jsonStringAt(item, "name"));
  if (name.empty()) {
    name = stripArchiveExtension(jsonStringAt(item, "relativePath"));
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
  }

  if (!name.empty() && name.front() == '[') {
    const size_t close = name.find(']');
    if (close != std::string::npos) {
      parts.artist = trimCopy(name.substr(1, close - 1));
      parts.title = trimCopy(name.substr(close + 1));
      return parts;
    }
  }

  parts.title = trimCopy(name);
  return parts;
}

std::string archiveExtensionFromUrl(const std::string &url) {
  return archiveExtensionFromName(fileNameFromUrl(url));
}

bool isRecognizedArchiveExtension(const std::string &extension) {
  return !extension.empty() &&
         !archiveExtensionFromName("download" + extension).empty();
}

bool isSupportedArchiveExtension(const std::string &extension) {
  if (extension == ".zip") {
    return true;
  }
#if ASOBMSHOW_HAS_LIBARCHIVE
  return isRecognizedArchiveExtension(extension);
#else
  return false;
#endif
}

bool isArchiveContentType(std::string contentType) {
  contentType = lowerCopy(trimCopy(std::move(contentType)));
  return contentType == "application/zip" ||
         contentType == "application/x-zip-compressed" ||
         contentType == "application/x-7z-compressed" ||
         contentType == "application/vnd.rar" ||
         contentType == "application/x-rar-compressed" ||
         contentType == "application/x-lzh-compressed" ||
         contentType == "application/x-lha" ||
         contentType == "application/x-tar" ||
         contentType == "application/gzip" ||
         contentType == "application/x-gzip" ||
         contentType == "application/x-bzip2" ||
         contentType == "application/x-xz";
}

std::string googleSearchUrlForText(const std::string &query) {
  std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    trimmed = "BMS chart";
  }
  return "https://www.google.com/search?q=" + urlEncode("BMS " + trimmed);
}

std::string bmsSearchUrlForText(const std::string &query) {
  std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    trimmed = "BMS chart";
  }
  return std::string(BmsSearchService::kBaseUrl) + "/search?q=" +
         urlEncode(trimmed);
}

std::string normalizedSearchText(const std::string &value) {
  return asobmshow::bms_metadata::normalizedSearchText(value);
}

std::vector<std::string> splitSearchTokens(const std::string &value) {
  return asobmshow::bms_metadata::splitSearchTokens(value);
}

bool containsNonAscii(const std::string &value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char c) {
    return c >= 0x80;
  });
}

size_t utf8CodepointCount(const std::string &value) {
  return static_cast<size_t>(
      std::count_if(value.begin(), value.end(), [](unsigned char c) {
        return (c & 0xc0) != 0x80;
      }));
}

bool isMeaningfulSearchToken(const std::string &token) {
  return token.size() >= 4 || containsNonAscii(token);
}

bool requiresExactTitleTokenMatch(const std::string &normalizedTitle) {
  return containsNonAscii(normalizedTitle) &&
         utf8CodepointCount(normalizedTitle) <= 2;
}

bool normalizedHaystackHasExactToken(const std::string &haystack,
                                     const std::string &needle) {
  std::istringstream stream(haystack);
  std::string token;
  while (stream >> token) {
    if (token == needle) {
      return true;
    }
  }
  return false;
}

bool normalizedHaystackMatchesTitle(const std::string &haystack,
                                    const std::string &normalizedTitle) {
  if (normalizedTitle.empty()) {
    return false;
  }
  if (requiresExactTitleTokenMatch(normalizedTitle)) {
    return normalizedHaystackHasExactToken(haystack, normalizedTitle);
  }
  return haystack.find(normalizedTitle) != std::string::npos;
}

bool allowsRawSubstringTitleMatch(const std::string &normalizedTitle) {
  return !requiresExactTitleTokenMatch(normalizedTitle) &&
         (normalizedTitle.size() >= 4 || containsNonAscii(normalizedTitle));
}

bool isLooseQuerySeparator(char c) {
  return asobmshow::bms_metadata::isLooseQuerySeparator(c);
}

std::string collapseWhitespaceCopy(const std::string &value) {
  return asobmshow::bms_metadata::collapseWhitespaceCopy(value);
}

std::string cleanupDecoratedQueryText(std::string value) {
  return asobmshow::bms_metadata::cleanupDecoratedQueryText(std::move(value));
}

bool looksLikeTitleDecoration(const std::string &value) {
  return asobmshow::bms_metadata::looksLikeTitleDecoration(value);
}

bool looksLikeArtistDecoration(const std::string &value) {
  return asobmshow::bms_metadata::looksLikeArtistDecoration(value);
}

std::string stripBracketedDecorations(
    std::string value, char open, char close,
    const std::function<bool(const std::string &)> &isDecoration) {
  return asobmshow::bms_metadata::stripBracketedDecorations(
      std::move(value), open, close, isDecoration);
}

std::string stripTrailingSquareBracketDecorations(std::string value) {
  return asobmshow::bms_metadata::stripTrailingSquareBracketDecorations(
      std::move(value));
}

std::string stripTitleDecorations(const std::string &title) {
  return asobmshow::bms_metadata::stripTitleDecorations(title);
}

std::string stripArtistDecorations(const std::string &artist) {
  return asobmshow::bms_metadata::stripArtistDecorations(artist);
}

std::string stripArtistAfterSlash(const std::string &artist) {
  const std::string trimmed = trimCopy(artist);
  size_t slash = trimmed.find('/');
  const size_t fullWidthSlash = trimmed.find("／");
  if (fullWidthSlash != std::string::npos &&
      (slash == std::string::npos || fullWidthSlash < slash)) {
    slash = fullWidthSlash;
  }
  if (slash == std::string::npos) {
    return cleanupDecoratedQueryText(trimmed);
  }
  return cleanupDecoratedQueryText(trimmed.substr(0, slash));
}

bool titleNeedsExactCandidateMatch(const std::string &title) {
  const std::string normalizedTitle = normalizedSearchText(title);
  const std::string normalizedMinimalTitle =
      normalizedSearchText(stripTitleDecorations(title));
  return requiresExactTitleTokenMatch(normalizedTitle) ||
         requiresExactTitleTokenMatch(normalizedMinimalTitle);
}

std::optional<HorieLookupTerms>
splitCombinedHorieLookupTerms(const std::string &value) {
  const std::string trimmed = trimCopy(value);
  for (size_t i = trimmed.size(); i > 0; --i) {
    const unsigned char c = static_cast<unsigned char>(trimmed[i - 1]);
    if (std::isspace(c) == 0) {
      continue;
    }

    HorieLookupTerms terms;
    terms.artist = trimCopy(trimmed.substr(0, i - 1));
    terms.title = trimCopy(trimmed.substr(i));
    if (terms.artist.empty() || terms.title.empty()) {
      return std::nullopt;
    }

    const std::string normalizedTitle =
        normalizedSearchText(stripTitleDecorations(terms.title));
    if (!requiresExactTitleTokenMatch(normalizedTitle)) {
      return std::nullopt;
    }

    if (!containsNonAscii(terms.artist) &&
        !looksLikeArtistDecoration(terms.artist)) {
      return std::nullopt;
    }
    return terms;
  }
  return std::nullopt;
}

HorieLookupTerms horieLookupTermsForMeta(const std::string &title,
                                         const std::string &artist) {
  HorieLookupTerms terms{trimCopy(title), trimCopy(artist)};
  if (const auto splitTerms = splitCombinedHorieLookupTerms(terms.title)) {
    if (terms.artist.empty()) {
      return *splitTerms;
    }

    const std::string normalizedArtist =
        normalizedSearchText(stripArtistDecorations(terms.artist));
    const std::string normalizedSplitArtist =
        normalizedSearchText(stripArtistDecorations(splitTerms->artist));
    if (!normalizedArtist.empty() && !normalizedSplitArtist.empty() &&
        (normalizedSplitArtist.find(normalizedArtist) != std::string::npos ||
         normalizedArtist.find(normalizedSplitArtist) != std::string::npos)) {
      terms.title = splitTerms->title;
    }
  }
  return terms;
}

void appendUniqueQuery(std::vector<std::string> &queries,
                       const std::string &query) {
  const std::string trimmed = trimCopy(query);
  if (trimmed.empty()) {
    return;
  }

  const std::string normalized = normalizedSearchText(trimmed);
  const std::string key = normalized.empty() ? lowerCopy(trimmed) : normalized;
  for (const auto &existing : queries) {
    const std::string existingNormalized = normalizedSearchText(existing);
    const std::string existingKey =
        existingNormalized.empty() ? lowerCopy(existing) : existingNormalized;
    if (existingKey == key) {
      return;
    }
  }
  queries.push_back(trimmed);
}

std::vector<std::string> horieArtistQueryVariants(const std::string &artist) {
  std::vector<std::string> variants;
  const std::string trimmedArtist = trimCopy(artist);
  const std::string objectStrippedArtist =
      stripArtistDecorations(trimmedArtist);
  const std::string slashStrippedArtist = stripArtistAfterSlash(trimmedArtist);
  const std::string slashObjectStrippedArtist =
      stripArtistAfterSlash(objectStrippedArtist);

  appendUniqueQuery(variants, trimmedArtist);
  appendUniqueQuery(variants, objectStrippedArtist);
  appendUniqueQuery(variants, slashStrippedArtist);
  appendUniqueQuery(variants, slashObjectStrippedArtist);
  return variants;
}


} // namespace asobmshow::bms_search
