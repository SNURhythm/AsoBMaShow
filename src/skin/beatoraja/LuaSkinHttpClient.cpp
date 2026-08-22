#include "LuaSkinHttpClient.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace skin {
namespace {

std::string_view urlScheme(std::string_view url) noexcept {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return {};
  }
  return url.substr(0, colon);
}

bool asciiAlpha(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

bool asciiDigit(char value) noexcept { return value >= '0' && value <= '9'; }

bool asciiHex(char value) noexcept {
  return asciiDigit(value) || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

std::optional<std::string> validateUri(std::string_view uri) {
  const std::size_t colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0 ||
      !asciiAlpha(uri.front())) {
    return "URI is not absolute";
  }
  for (std::size_t index = 1; index < colon; ++index) {
    const char value = uri[index];
    if (!asciiAlpha(value) && !asciiDigit(value) && value != '+' &&
        value != '-' && value != '.') {
      return "URI scheme is malformed";
    }
  }
  constexpr std::string_view allowedAscii =
      "-._~:/?#[]@!$&'()*+,;=";
  int brackets = 0;
  for (std::size_t index = colon + 1; index < uri.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(uri[index]);
    if (value == '%') {
      if (index + 2 >= uri.size() || !asciiHex(uri[index + 1]) ||
          !asciiHex(uri[index + 2])) {
        return "Malformed escape pair in URI";
      }
      index += 2;
      continue;
    }
    if (value < 0x80 && !asciiAlpha(static_cast<char>(value)) &&
        !asciiDigit(static_cast<char>(value)) &&
        allowedAscii.find(static_cast<char>(value)) == std::string_view::npos) {
      return "Illegal character in URI";
    }
    if (value == '[') {
      ++brackets;
    } else if (value == ']') {
      if (--brackets < 0) {
        return "Malformed bracketed URI authority";
      }
    }
  }
  if (brackets != 0) {
    return "Malformed bracketed URI authority";
  }
  if (uri.substr(colon + 1) == "//") {
    return "Expected URI authority";
  }
  return std::nullopt;
}

bool asciiEqualIgnoringCase(std::string_view left,
                            std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(left[index]);
    const char folded = character >= 'A' && character <= 'Z'
                            ? static_cast<char>(character - 'A' + 'a')
                            : static_cast<char>(character);
    if (folded != right[index]) {
      return false;
    }
  }
  return true;
}

struct DecodedLine {
  std::string text;
  std::size_t utf16Characters = 0;
};

void appendReplacement(DecodedLine &decoded) {
  decoded.text.append("\xEF\xBF\xBD", 3);
  ++decoded.utf16Characters;
}

DecodedLine decodeUtf8ReplacingMalformed(std::string_view input) {
  DecodedLine decoded;
  decoded.text.reserve(input.size());
  for (std::size_t index = 0; index < input.size();) {
    const auto first = static_cast<unsigned char>(input[index]);
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7f) {
      length = 1;
      codepoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      codepoint = first & 0x07;
    } else {
      appendReplacement(decoded);
      ++index;
      continue;
    }

    std::size_t continuation = 1;
    for (; continuation < length && index + continuation < input.size();
         ++continuation) {
      const auto byte = static_cast<unsigned char>(input[index + continuation]);
      if ((byte & 0xc0) != 0x80) {
        break;
      }
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if (continuation != length) {
      appendReplacement(decoded);
      index += continuation;
      continue;
    }
    const bool valid =
        !((length == 3 && codepoint < 0x800) ||
          (length == 4 && codepoint < 0x10000) ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
          codepoint > 0x10ffff);
    if (!valid) {
      appendReplacement(decoded);
      ++index;
      continue;
    }

    decoded.text.append(input.substr(index, length));
    decoded.utf16Characters += codepoint > 0xffff ? 2 : 1;
    index += length;
  }
  return decoded;
}

} // namespace

LuaSkinHttpClient::LuaSkinHttpClient(LuaSkinHttpTransport *transport) noexcept
    : transport_(transport) {}

LuaSkinHttpOpenResult
LuaSkinHttpClient::open(std::string_view url,
                        int timeoutMilliseconds) const noexcept {
  try {
    if (const auto invalid = validateUri(url)) {
      return {.failure = *invalid};
    }
    const std::string_view scheme = urlScheme(url);
    if (!asciiEqualIgnoringCase(scheme, "http") &&
        !asciiEqualIgnoringCase(scheme, "https")) {
      return {.failure = "unsupported scheme: " + std::string(scheme)};
    }
    if (transport_ == nullptr) {
      return {.failure = "HTTP transport is unavailable"};
    }
    LuaSkinHttpOpenResult result = transport_->open(
        url, clampTimeout(timeoutMilliseconds), responseLimits);
    if (!result.connection && !result.failure) {
      result.failure = "HTTP transport returned no connection";
    }
    if (result.failure && result.failure->empty()) {
      result.failure = "HTTP transport failed";
    }
    return result;
  } catch (const std::exception &error) {
    try {
      return {.failure = error.what()};
    } catch (...) {
      return {.failure = "HTTP transport failed"};
    }
  } catch (...) {
    return {.failure = "HTTP transport failed"};
  }
}

LuaSkinHttpResult
LuaSkinHttpClient::get(std::string_view url,
                       int timeoutMilliseconds) const noexcept {
  LuaSkinHttpOpenResult opened = open(url, timeoutMilliseconds);
  if (opened.failure) {
    return {.failure = std::move(opened.failure)};
  }
  if (!opened.connection) {
    return {.failure = "HTTP transport returned no connection"};
  }
  struct DisconnectGuard {
    LuaSkinHttpConnection *connection = nullptr;
    ~DisconnectGuard() {
      if (connection != nullptr) {
        connection->disconnect();
      }
    }
  } disconnect{opened.connection.get()};
  if (auto failure = opened.connection->connect()) {
    return {.failure = std::move(failure)};
  }
  LuaSkinHttpCodeResult code = opened.connection->responseCode();
  if (code.failure) {
    return {.failure = std::move(code.failure)};
  }
  if (!code.code) {
    return {.failure = "HTTP transport returned no response code"};
  }
  LuaSkinHttpBodyResult body = opened.connection->readBody();
  if (body.failure) {
    return {.failure = std::move(body.failure)};
  }
  if (!body.body) {
    return {.failure = "HTTP transport returned no response body"};
  }
  return {.response = LuaSkinHttpResponse{.responseCode = *code.code,
                                          .body = std::move(*body.body)}};
}

LuaSkinHttpLinesResult
LuaSkinHttpClient::readLines(std::string_view body) noexcept {
  LuaSkinHttpLinesResult result;
  try {
    result.lines.reserve(
        std::min(responseLimits.maximumLines, body.size() / 2 + 1));
    std::size_t offset = 0;
    std::size_t characters = 0;
    while (offset < body.size() &&
           result.lines.size() < responseLimits.maximumLines) {
      const std::size_t end = body.find_first_of("\r\n", offset);
      const std::size_t lineEnd =
          end == std::string_view::npos ? body.size() : end;
      DecodedLine line =
          decodeUtf8ReplacingMalformed(body.substr(offset, lineEnd - offset));
      if (line.utf16Characters >
          responseLimits.maximumCharacters - characters) {
        return {.failure = "response is too large"};
      }
      characters += line.utf16Characters;
      result.lines.push_back(std::move(line.text));
      if (end == std::string_view::npos) {
        break;
      }
      offset = end + 1;
      if (body[end] == '\r' && offset < body.size() && body[offset] == '\n') {
        ++offset;
      }
    }
    return result;
  } catch (...) {
    return {.failure = "HTTP response allocation failed"};
  }
}

int LuaSkinHttpClient::clampTimeout(int timeoutMilliseconds) noexcept {
  return std::clamp(timeoutMilliseconds, 1, maximumTimeoutMilliseconds);
}

} // namespace skin
