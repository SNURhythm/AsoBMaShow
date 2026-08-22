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

LuaSkinHttpResult
LuaSkinHttpClient::get(std::string_view url,
                       int timeoutMilliseconds) const noexcept {
  const std::string_view scheme = urlScheme(url);
  if (!asciiEqualIgnoringCase(scheme, "http") &&
      !asciiEqualIgnoringCase(scheme, "https")) {
    try {
      return {.failure = "unsupported scheme: " +
                         std::string(scheme.empty() ? "null" : scheme)};
    } catch (...) {
      return {.failure = "unsupported scheme"};
    }
  }
  if (transport_ == nullptr) {
    return {.failure = "HTTP transport is unavailable"};
  }
  try {
    LuaSkinHttpResult result =
        transport_->get(url, clampTimeout(timeoutMilliseconds), responseLimits);
    if (!result.response && !result.failure) {
      return {.failure = "HTTP transport returned no response"};
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
