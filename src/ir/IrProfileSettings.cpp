#include "IrProfileSettings.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace ir {
namespace {

char asciiLower(char value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

bool isForbiddenByte(unsigned char value) {
  return value <= 0x20U || value == 0x7fU || value == '\\';
}

bool validDnsHost(std::string_view host) {
  if (host.empty() || host.front() == '.' || host.back() == '.') {
    return false;
  }
  bool previousDot = false;
  for (const unsigned char character : host) {
    if (character == '.') {
      if (previousDot) {
        return false;
      }
      previousDot = true;
      continue;
    }
    previousDot = false;
    if (!(std::isalnum(character) != 0 || character == '-')) {
      return false;
    }
  }
  return true;
}

bool validIpv6Literal(std::string_view host) {
  if (host.size() < 4 || host.front() != '[' || host.back() != ']') {
    return false;
  }
  bool hasColon = false;
  for (const unsigned char character : host.substr(1, host.size() - 2)) {
    if (character == ':') {
      hasColon = true;
      continue;
    }
    if (!(std::isxdigit(character) != 0 || character == '.')) {
      return false;
    }
  }
  return hasColon;
}

} // namespace

std::optional<std::string>
normalizeServerOrigin(std::string_view value) noexcept {
  try {
    if (value.empty() || value.size() > kMaximumServerOriginBytes ||
        std::ranges::any_of(value, [](unsigned char character) {
          return isForbiddenByte(character);
        })) {
      return std::nullopt;
    }

    const std::size_t schemeSeparator = value.find("://");
    if (schemeSeparator == std::string_view::npos) {
      return std::nullopt;
    }
    std::string scheme(value.substr(0, schemeSeparator));
    std::ranges::transform(scheme, scheme.begin(), asciiLower);
    if (scheme != "http" && scheme != "https") {
      return std::nullopt;
    }

    const std::string_view remainder = value.substr(schemeSeparator + 3);
    const std::size_t suffixStart = remainder.find_first_of("/?#");
    const std::string_view authority = remainder.substr(0, suffixStart);
    const std::string_view suffix =
        suffixStart == std::string_view::npos
            ? std::string_view{}
            : remainder.substr(suffixStart);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        (!suffix.empty() && suffix != "/")) {
      return std::nullopt;
    }

    std::string_view host;
    std::string_view port;
    if (authority.front() == '[') {
      const std::size_t closingBracket = authority.find(']');
      if (closingBracket == std::string_view::npos) {
        return std::nullopt;
      }
      host = authority.substr(0, closingBracket + 1);
      const std::string_view afterHost = authority.substr(closingBracket + 1);
      if (!afterHost.empty()) {
        if (afterHost.front() != ':' || afterHost.size() == 1) {
          return std::nullopt;
        }
        port = afterHost.substr(1);
      }
      if (!validIpv6Literal(host)) {
        return std::nullopt;
      }
    } else {
      const std::size_t colon = authority.find(':');
      if (colon == std::string_view::npos) {
        host = authority;
      } else {
        if (authority.find(':', colon + 1) != std::string_view::npos) {
          return std::nullopt;
        }
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (port.empty()) {
          return std::nullopt;
        }
      }
      if (!validDnsHost(host)) {
        return std::nullopt;
      }
    }

    unsigned int portNumber = 0;
    if (!port.empty()) {
      const auto [end, error] =
          std::from_chars(port.data(), port.data() + port.size(), portNumber);
      if (error != std::errc{} || end != port.data() + port.size() ||
          portNumber == 0 || portNumber > 65535) {
        return std::nullopt;
      }
    }

    std::string normalizedHost(host);
    std::ranges::transform(normalizedHost, normalizedHost.begin(), asciiLower);
    std::string normalized = scheme + "://" + normalizedHost;
    const bool defaultPort =
        (scheme == "http" && portNumber == 80) ||
        (scheme == "https" && portNumber == 443);
    if (!port.empty() && !defaultPort) {
      normalized += ':';
      normalized += std::to_string(portNumber);
    }
    return normalized;
  } catch (...) {
    return std::nullopt;
  }
}

void sanitizeProviderSettings(IrProviderSettings &settings) noexcept {
  if (const auto normalized = normalizeServerOrigin(settings.serverOrigin)) {
    settings.serverOrigin = *normalized;
  } else {
    settings.serverOrigin = std::string(kDefaultTachiServerOrigin);
  }
}

} // namespace ir
