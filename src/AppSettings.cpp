#include "AppSettings.h"
#include "Utils.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {
std::string trim(std::string value) {
  auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !isSpace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !isSpace(ch); })
                  .base(),
              value.end());
  return value;
}

bool parseBool(const std::string &value, bool &out) {
  if (value == "1" || value == "true" || value == "on" || value == "yes") {
    out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "off" || value == "no") {
    out = false;
    return true;
  }
  return false;
}
} // namespace

std::filesystem::path AppSettings::configPath() {
  return Utils::GetDocumentsPath("settings.cfg");
}

void AppSettings::sanitize() {
  inputOffsetMs =
      std::clamp(inputOffsetMs, kMinInputOffsetMs, kMaxInputOffsetMs);
}

bool AppSettings::save() const {
  AppSettings sanitized = *this;
  sanitized.sanitize();

  const auto path = configPath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    SDL_Log("Failed to create settings directory: %s", ec.message().c_str());
    return false;
  }

  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    SDL_Log("Failed to open settings file for writing: %s",
            path.string().c_str());
    return false;
  }

  file << "# AsoBMaShow settings\n";
  file << "input_offset_ms=" << sanitized.inputOffsetMs << "\n";
  file << "input_keysound_enabled="
       << (sanitized.inputKeysoundEnabled ? 1 : 0) << "\n";
  file << "bga_enabled=" << (sanitized.bgaEnabled ? 1 : 0) << "\n";
  return file.good();
}

AppSettings AppSettings::load() {
  AppSettings settings;
  const auto path = configPath();
  std::ifstream file(path);
  if (!file.is_open()) {
    settings.sanitize();
    return settings;
  }

  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    const std::string key = trim(line.substr(0, separator));
    const std::string value = trim(line.substr(separator + 1));

    try {
      if (key == "input_offset_ms") {
        settings.inputOffsetMs = std::stoi(value);
      } else if (key == "input_keysound_enabled") {
        bool parsed = settings.inputKeysoundEnabled;
        if (parseBool(value, parsed)) {
          settings.inputKeysoundEnabled = parsed;
        }
      } else if (key == "bga_enabled") {
        bool parsed = settings.bgaEnabled;
        if (parseBool(value, parsed)) {
          settings.bgaEnabled = parsed;
        }
      }
    } catch (const std::exception &e) {
      SDL_Log("Ignoring malformed settings line '%s': %s", line.c_str(),
              e.what());
    }
  }

  settings.sanitize();
  return settings;
}
