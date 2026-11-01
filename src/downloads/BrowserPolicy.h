#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace downloads {
std::optional<std::string> httpOrigin(std::string_view url);
bool sameOrigin(std::string_view left, std::string_view right);
bool allowedRedirect(std::string_view from, std::string_view to);
bool replayableBrowserDownload(std::string_view method, std::string_view url);
// Address-bar input: direct HTTP(S)/host navigation, otherwise an encoded search.
std::optional<std::string> browserAddressTarget(std::string_view input);
std::optional<std::string> gingerTableUrl(std::string_view pageUrl);
// detectedUrl comes from the current top-level document's metadata/header probe.
std::optional<std::string> browserTableUrl(std::string_view pageUrl, std::string_view detectedUrl = {});
const char *browserTableDetectionScript();
std::string safeArchiveName(std::string_view suggested);
bool archiveResponse(std::string_view mime, std::string_view filename);
} // namespace downloads
