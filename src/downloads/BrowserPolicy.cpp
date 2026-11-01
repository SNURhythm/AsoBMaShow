#include "BrowserPolicy.h"
#include <algorithm>
#include <cctype>

namespace downloads {
namespace {
std::string lower(std::string_view value) {
  std::string result(value);
  for (auto &c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}
bool clean(std::string_view value) {
  return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; });
}
std::string addressEncoded(std::string_view value, bool preserveAscii) {
  std::string result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : value) {
    if ((preserveAscii && c >= 32 && c < 127) ||
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += c;
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
}
std::optional<std::string> httpOrigin(std::string_view url) {
  if (url.empty() || url.size() > 16384 || !clean(url)) return {};
  const auto separator = url.find("://");
  if (separator == url.npos) return {};
  const auto scheme = lower(url.substr(0, separator));
  if (scheme != "http" && scheme != "https") return {};
  const auto start = separator + 3;
  const auto end = url.find_first_of("/?#", start);
  auto authority = lower(url.substr(start, end == url.npos ? url.size() - start : end - start));
  if (authority.empty() || authority.find_first_of("@%") != authority.npos) return {};
  std::string host = authority, port;
  if (host.front() == '[') {
    const auto close = host.find(']');
    if (close == host.npos || close == 1) return {};
    if (close + 1 < host.size()) {
      if (host[close + 1] != ':') return {};
      port = host.substr(close + 2);
      if (port.empty()) return {};
    }
    host.resize(close + 1);
  } else {
    const auto colon = host.find(':');
    if (colon != host.npos) {
      port = host.substr(colon + 1);
      host.resize(colon);
      if (port.empty()) return {};
    }
    if (host.empty() || std::any_of(host.begin(), host.end(), [](unsigned char c) {
          return !(std::isalnum(c) || c == '-' || c == '.');
        })) return {};
  }
  if (!port.empty()) {
    unsigned number = 0;
    for (const auto c : port) {
      if (c < '0' || c > '9') return {};
      number = number * 10 + (c - '0');
      if (number > 65535) return {};
    }
    if (number == 0) return {};
    port = std::to_string(number);
    if ((scheme == "https" && port == "443") || (scheme == "http" && port == "80")) port.clear();
  }
  return scheme + "://" + host + (port.empty() ? "" : ":" + port);
}
bool sameOrigin(std::string_view left, std::string_view right) {
  const auto a = httpOrigin(left), b = httpOrigin(right);
  return a && b && *a == *b;
}
bool allowedRedirect(std::string_view from, std::string_view to) {
  const auto a = httpOrigin(from), b = httpOrigin(to);
  return a && b && !(a->starts_with("https:") && b->starts_with("http:"));
}
bool replayableBrowserDownload(std::string_view method, std::string_view url) {
  return method == "GET" && httpOrigin(url).has_value();
}
std::optional<std::string> browserAddressTarget(std::string_view input) {
  const auto start = input.find_first_not_of(" \t\r\n\f\v");
  if (start == input.npos) return {};
  input = input.substr(start, input.find_last_not_of(" \t\r\n\f\v") - start + 1);
  if (input.size() > 16384) return {};
  if (httpOrigin(input)) {
    const auto scheme = input.find("://");
    return lower(input.substr(0, scheme)) + addressEncoded(input.substr(scheme), true);
  }
  const auto candidate = "https://" + std::string(input);
  if (httpOrigin(candidate)) {
    auto host = input.substr(0, input.find_first_of("/?#"));
    const bool ipv6 = host.starts_with("[") && host.find(':') != host.npos;
    if (!ipv6) host = host.substr(0, host.find(':'));
    if (ipv6 || lower(host) == "localhost" ||
        (host.find('.') != host.npos && !host.starts_with('.') && !host.ends_with('.'))) return addressEncoded(candidate, true);
  }
  std::string search = "https://www.google.com/search?q=" + addressEncoded(input, false);
  return search.size() <= 16384 ? std::optional(std::move(search)) : std::nullopt;
}
std::optional<std::string> gingerTableUrl(std::string_view pageUrl) {
  if (httpOrigin(pageUrl) != "https://gingerrush.com") return {};
  const auto start = pageUrl.find('/', pageUrl.find("://") + 3);
  if (start == pageUrl.npos) return {};
  auto path = pageUrl.substr(start);
  if (path.starts_with("/#/")) path.remove_prefix(2);
  const auto suffix = path.find_first_of("?#");
  path = path.substr(0, suffix);
  constexpr std::string_view page = "/download/table/", header = "/api/v1/table/meta/header/";
  std::string_view id;
  if (path.starts_with(page)) id = path.substr(page.size());
  else if (path.starts_with(header) && path.ends_with(".json"))
    id = path.substr(header.size(), path.size() - header.size() - 5);
  else return {};
  if (id.empty() || id.size() > 18 || id.front() == '0' ||
      std::any_of(id.begin(), id.end(), [](char c) { return c < '0' || c > '9'; })) return {};
  return "https://gingerrush.com/api/v1/table/meta/header/" + std::string(id) + ".json";
}
std::optional<std::string> browserTableUrl(std::string_view pageUrl, std::string_view detectedUrl) {
  if (!httpOrigin(pageUrl)) return {};
  if (auto ginger = gingerTableUrl(pageUrl)) return ginger;
  if (!httpOrigin(detectedUrl)) return {};
  return std::string(detectedUrl);
}
const char *browserTableDetectionScript() {
  // Read only the top-level document; no fetches, page callbacks or native bridge.
  return R"bmstable((function() {
    const result = {pageUrl: location.href, tableUrl: ''};
    function httpUrl(value) {
      if (typeof value !== 'string' || !value.trim()) return '';
      try {
        const url = new URL(value.trim(), document.baseURI);
        return (url.protocol === 'https:' || url.protocol === 'http:') && !url.username && !url.password
          ? url.href : '';
      } catch (_) { return ''; }
    }
    for (const meta of document.querySelectorAll('meta[name][content]')) {
      if ((meta.getAttribute('name') || '').trim().toLowerCase() !== 'bmstable') continue;
      result.tableUrl = httpUrl(meta.getAttribute('content'));
      if (result.tableUrl) return result;
    }
    const body = document.body ? document.body.textContent : '';
    if (body && body.length <= 8 * 1024 * 1024) {
      try {
        const header = JSON.parse(body);
        if (header && !Array.isArray(header) && typeof header.name === 'string' &&
            typeof header.symbol === 'string' && httpUrl(header.data_url))
          result.tableUrl = httpUrl(location.href);
      } catch (_) {}
    }
    return result;
  })())bmstable";
}
std::string safeArchiveName(std::string_view suggested) {
  const auto slash = suggested.find_last_of("/\\");
  if (slash != suggested.npos) suggested.remove_prefix(slash + 1);
  std::string name;
  for (unsigned char c : suggested) {
    if (c < 32 || c == 127 || c == ':' || c == '"') continue;
    name += static_cast<char>(c);
  }
  if (name.empty() || name == "." || name == ".." || name.size() > 240) return "download.archive";
  return name;
}
bool archiveResponse(std::string_view mime, std::string_view filename) {
  const auto type = lower(mime.substr(0, mime.find(';'))), name = lower(filename);
  return type == "application/zip" || type == "application/x-zip-compressed" ||
      type == "application/x-rar-compressed" || type == "application/vnd.rar" ||
      type == "application/x-7z-compressed" || type == "application/octet-stream" ||
      name.ends_with(".zip") || name.ends_with(".7z") || name.ends_with(".rar");
}
} // namespace downloads
