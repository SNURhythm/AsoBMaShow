#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace downloads {
std::optional<std::string> httpOrigin(std::string_view url);
bool sameOrigin(std::string_view left, std::string_view right);
bool allowedRedirect(std::string_view from, std::string_view to);
std::string safeArchiveName(std::string_view suggested);
bool archiveResponse(std::string_view mime, std::string_view filename);
} // namespace downloads
