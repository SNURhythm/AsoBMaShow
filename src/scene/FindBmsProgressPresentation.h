#pragma once

#include "../i18n/Localization.h"

#include <cstdint>
#include <string>

std::string formatFindBmsBytes(std::uint64_t bytes);
std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes);

i18n::Text findBmsProgressDisplayMessage(const std::string &message,
                                        std::uint64_t downloadedBytes,
                                        std::uint64_t totalBytes,
                                        bool includeBytes);
