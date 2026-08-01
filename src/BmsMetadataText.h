#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asobmshow::bms_metadata {

inline std::string trimCopy(const std::string &value) {
  const auto begin =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
  const auto end =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
      }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

inline std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline std::string normalizedHash(const std::string &value) {
  return lowerCopy(trimCopy(value));
}

inline std::string normalizedSearchText(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isalnum(c)) {
      result.push_back(static_cast<char>(std::tolower(c)));
      lastWasSpace = false;
    } else if (c >= 0x80) {
      result.push_back(static_cast<char>(c));
      lastWasSpace = false;
    } else if (!lastWasSpace) {
      result.push_back(' ');
      lastWasSpace = true;
    }
  }
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

inline std::vector<std::string> splitSearchTokens(const std::string &value) {
  std::vector<std::string> tokens;
  std::istringstream stream(normalizedSearchText(value));
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

inline bool isLooseQuerySeparator(char c) {
  switch (c) {
  case '-':
  case '_':
  case '/':
  case ',':
  case ':':
  case ';':
  case '|':
  case '~':
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
    return true;
  default:
    return false;
  }
}

inline std::string collapseWhitespaceCopy(const std::string &value) {
  std::string result;
  bool lastWasSpace = true;
  for (unsigned char c : value) {
    if (std::isspace(c) != 0) {
      if (!lastWasSpace) {
        result.push_back(' ');
        lastWasSpace = true;
      }
      continue;
    }
    result.push_back(static_cast<char>(c));
    lastWasSpace = false;
  }
  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

inline std::string cleanupDecoratedQueryText(std::string value) {
  value = collapseWhitespaceCopy(trimCopy(value));
  while (!value.empty() &&
         isLooseQuerySeparator(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
    value = trimCopy(value);
  }
  while (!value.empty() &&
         isLooseQuerySeparator(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
    value = trimCopy(value);
  }
  return collapseWhitespaceCopy(value);
}

inline bool looksLikeTitleDecoration(const std::string &value) {
  const auto tokens = splitSearchTokens(value);
  if (tokens.empty()) {
    return false;
  }

  static constexpr std::string_view kDecorationTokens[] = {
      "another", "hyper", "normal", "beginner", "easy", "hard",
      "insane",  "black", "lunatic", "legend", "extra",    "ex",
      "ent",     "entry", "entrance", "sp",     "dp",       "spn",
      "sph",     "spa",   "dpn",     "dph",     "dpa",      "iidx",
      "bga"};
  for (const auto &token : tokens) {
    if (token == "key" || token == "keys" ||
        token.find("key") != std::string::npos) {
      return true;
    }
    if (std::find(std::begin(kDecorationTokens), std::end(kDecorationTokens),
                  token) != std::end(kDecorationTokens)) {
      return true;
    }
  }
  return false;
}

inline bool looksLikeArtistDecoration(const std::string &value) {
  const std::string lower = lowerCopy(value);
  return lower.find("obj.") != std::string::npos ||
         lower.find("obj:") != std::string::npos ||
         lower.find("obj-") != std::string::npos ||
         lower.find("obj ") != std::string::npos ||
         lower.find("object.") != std::string::npos ||
         lower.find("object:") != std::string::npos ||
         lower.find("object-") != std::string::npos ||
         lower.find("object ") != std::string::npos;
}

inline std::string stripBracketedDecorations(
    std::string value, char open, char close,
    const std::function<bool(const std::string &)> &isDecoration) {
  size_t position = 0;
  while (position < value.size()) {
    const size_t start = value.find(open, position);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = value.find(close, start + 1);
    if (end == std::string::npos) {
      break;
    }

    const std::string inner = value.substr(start + 1, end - start - 1);
    if (isDecoration(inner)) {
      value.erase(start, end - start + 1);
      position = start;
      continue;
    }
    position = end + 1;
  }
  return cleanupDecoratedQueryText(value);
}

inline std::string stripTrailingSquareBracketDecorations(std::string value) {
  value = trimCopy(std::move(value));
  while (!value.empty() && value.back() == ']') {
    const size_t open = value.rfind('[');
    if (open == std::string::npos) {
      break;
    }

    const std::string prefix = cleanupDecoratedQueryText(value.substr(0, open));
    if (prefix.empty()) {
      break;
    }
    value = prefix;
  }
  return cleanupDecoratedQueryText(value);
}

inline std::string stripTitleDecorations(const std::string &title) {
  std::string result = trimCopy(title);
  result = stripTrailingSquareBracketDecorations(std::move(result));
  result =
      stripBracketedDecorations(std::move(result), '[', ']',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '(', ')',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '{', '}',
                                [](const std::string &value) {
                                  return looksLikeTitleDecoration(value);
                                });
  return stripTrailingSquareBracketDecorations(std::move(result));
}

inline std::string stripArtistDecorations(const std::string &artist) {
  std::string result = trimCopy(artist);
  result =
      stripBracketedDecorations(std::move(result), '[', ']',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '(', ')',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });
  result =
      stripBracketedDecorations(std::move(result), '{', '}',
                                [](const std::string &value) {
                                  return looksLikeArtistDecoration(value);
                                });

  static const std::regex objectSegmentPattern(
      R"((^|[\s,/;&+|()\[\]{}-])\bobj(?:ect)?\b[.:-]?\s*(?:\([^)]*\)|\[[^\]]*\]|\{[^}]*\}|[^,/;&+|()\[\]{}-]+))",
      std::regex::icase);
  result = std::regex_replace(result, objectSegmentPattern, "$1");
  return cleanupDecoratedQueryText(result);
}

inline bool hasArtistObjectNotation(const std::string &artist) {
  const std::string cleaned = cleanupDecoratedQueryText(artist);
  return !cleaned.empty() && (looksLikeArtistDecoration(cleaned) ||
                              stripArtistDecorations(cleaned) != cleaned);
}

inline bool hasArtistObjectNotation(const std::string &artist,
                                    const std::string &subArtist) {
  return hasArtistObjectNotation(artist) || hasArtistObjectNotation(subArtist);
}

} // namespace asobmshow::bms_metadata
