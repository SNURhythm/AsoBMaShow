#include "LuaSkinHttpClient.h"

#include <algorithm>
#include <cmath>
#include <charconv>
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
};

void appendReplacement(DecodedLine &decoded) {
  decoded.text.append("\xEF\xBF\xBD", 3);
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
    index += length;
  }
  return decoded;
}

} // namespace

LuaSkinHttpClient::LuaSkinHttpClient(LuaSkinHttpTransport *transport) noexcept
    : transport_(transport) {}

LuaSkinHttpOpenResult
LuaSkinHttpClient::open(std::string_view url,
                        int timeoutMilliseconds, std::size_t maxBytes) const noexcept {
  try {
    if (maxBytes < 1 || maxBytes > maximumBytes) {
      return {.failure = "HTTP max_bytes must be between 1 and 8388608"};
    }
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
        url, clampTimeout(timeoutMilliseconds), LuaSkinHttpLimits{.maximumBytes = maxBytes});
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
                       int timeoutMilliseconds, std::size_t maxBytes) const noexcept {
  LuaSkinHttpOpenResult opened = open(url, timeoutMilliseconds, maxBytes);
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
  if (body.body->size() > maxBytes) {
    return {.failure = "response is too large"};
  }
  return {.response = LuaSkinHttpResponse{.responseCode = *code.code,
                                          .body = std::move(*body.body)}};
}

LuaSkinHttpLinesResult
LuaSkinHttpClient::readLines(std::string_view body, std::size_t maxBytes) noexcept {
  LuaSkinHttpLinesResult result;
  try {
    if (maxBytes < 1 || maxBytes > maximumBytes) {
      return {.failure = "HTTP max_bytes must be between 1 and 8388608"};
    }
    if (body.size() > maxBytes) return {.failure = "response is too large"};
    result.lines.reserve(std::min<std::size_t>(1024, body.size() / 2 + 1));
    std::size_t offset = 0;
    while (offset < body.size()) {
      const std::size_t end = body.find_first_of("\r\n", offset);
      const std::size_t lineEnd =
          end == std::string_view::npos ? body.size() : end;
      DecodedLine line =
          decodeUtf8ReplacingMalformed(body.substr(offset, lineEnd - offset));
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

std::optional<std::size_t> LuaSkinHttpClient::checkedLimit(double value) noexcept {
  if (!std::isfinite(value) || value != std::floor(value) || value < 1 ||
      value > maximumBytes) return std::nullopt;
  return static_cast<std::size_t>(value);
}

namespace {
std::string_view asyncHost(std::string_view url) {
  auto authority = url.substr(url.find("://") + 3);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  if (authority.starts_with('[')) return authority.substr(1, authority.find(']') - 1);
  return authority.substr(0, authority.find(':'));
}
}

std::optional<std::string> LuaSkinHttpClient::validateAsyncUrl(std::string_view url) {
  if (url.empty() || url.size() > 2048) return "Invalid HTTP URL length";
  if (auto failure = validateUri(url)) return failure;
  auto scheme = urlScheme(url);
  if ((!asciiEqualIgnoringCase(scheme, "http") && !asciiEqualIgnoringCase(scheme, "https")) ||
      url.substr(scheme.size(), 3) != "://") return "Only absolute HTTP/HTTPS URLs are allowed";
  auto authority = url.substr(scheme.size() + 3);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  if (authority.empty() || authority.find('@') != authority.npos)
    return "Only absolute HTTP/HTTPS URLs without credentials are allowed";
  auto host = asyncHost(url);
  if (host.empty()) return "Invalid HTTP host";
  if (authority.front() == '[') {
    if (host.find(':') == host.npos) return "Invalid HTTP host";
    for (unsigned char c : host)
      if (!asciiHex(c) && c != ':' && c != '.') return "Invalid HTTP host";
  }
  for (unsigned char c : host) {
    if (!(asciiAlpha(c) || asciiDigit(c) || c == '-' || c == '.' ||
          (authority.front() == '[' && c == ':'))) return "Invalid HTTP host";
  }
  const auto hostEnd = authority.front() == '[' ? authority.find(']') + 1 : host.size();
  if (hostEnd < authority.size()) {
    if (authority[hostEnd] != ':') return "Invalid HTTP port";
    auto port = authority.substr(hostEnd + 1);
    unsigned number = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
        number < 1 || number > 65535) return "Invalid HTTP port";
  }
  return std::nullopt;
}

bool LuaSkinHttpClient::isPublicAddress(std::span<const unsigned char> bytes) noexcept {
  if (bytes.size() != 4 && bytes.size() != 16) return false;
  const auto a = bytes[0], b = bytes[1];
  if (bytes.size() == 4) {
    return a != 0 && a != 10 && a != 127 && a < 224 &&
        !(a == 100 && b >= 64 && b <= 127) && !(a == 169 && b == 254) &&
        !(a == 172 && b >= 16 && b <= 31) && !(a == 192 && (b == 0 || b == 168)) &&
        !(a == 198 && (b == 18 || b == 19 || b == 51)) &&
        !(a == 203 && b == 0 && bytes[2] == 113);
  }
  return (a & 0xe0) == 0x20 && !(a == 0x20 && b == 0x02) &&
      !(a == 0x20 && b == 0x01 && (bytes[2] < 2 || (bytes[2] == 0x0d && bytes[3] == 0xb8)));
}

std::optional<std::string> LuaSkinHttpClient::validateAsyncRedirect(
    std::string_view previous, std::string_view next, int redirectCount) {
  if (redirectCount > 3) return "Invalid or excessive HTTP redirects";
  if (auto invalid = validateAsyncUrl(next)) return invalid;
  if (asciiEqualIgnoringCase(urlScheme(previous), "https") &&
      !asciiEqualIgnoringCase(urlScheme(next), "https")) return "HTTPS downgrade denied";
  return std::nullopt;
}

} // namespace skin
