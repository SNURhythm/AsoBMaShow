#include "AppSettings.h"
#include "Utils.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
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

float sanitizeFloat(float value, float fallback, float minValue,
                    float maxValue) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, minValue, maxValue);
}

AppSettings::BgaDisplayMode
parseBgaDisplayMode(const std::string &value,
                    AppSettings::BgaDisplayMode fallback) {
  if (value == "fit" || value == "0") {
    return AppSettings::BgaDisplayMode::Fit;
  }
  if (value == "fill" || value == "1") {
    return AppSettings::BgaDisplayMode::Fill;
  }
  if (value == "stretch" || value == "2") {
    return AppSettings::BgaDisplayMode::Stretch;
  }
  return fallback;
}

const char *bgaDisplayModeToString(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return "fit";
  case AppSettings::BgaDisplayMode::Fill:
    return "fill";
  case AppSettings::BgaDisplayMode::Stretch:
    return "stretch";
  }
  return "fit";
}
} // namespace

std::filesystem::path AppSettings::configPath() {
  return Utils::GetDocumentsPath("settings.cfg");
}

void AppSettings::sanitize() {
  inputOffsetMs =
      std::clamp(inputOffsetMs, kMinInputOffsetMs, kMaxInputOffsetMs);
  visualOffsetMs =
      std::clamp(visualOffsetMs, kMinVisualOffsetMs, kMaxVisualOffsetMs);
  visibleTimeGreenNumber =
      std::clamp(visibleTimeGreenNumber, kMinVisibleTimeGreenNumber,
                 kMaxVisibleTimeGreenNumber);
  bgaBrightnessPercent = std::clamp(
      bgaBrightnessPercent, kMinBgaBrightnessPercent, kMaxBgaBrightnessPercent);
  bgaBlurStrength = sanitizeFloat(bgaBlurStrength, kDefaultBgaBlurStrength,
                                  kMinBgaBlurStrength, kMaxBgaBlurStrength);
  switch (bgaDisplayMode) {
  case BgaDisplayMode::Fit:
  case BgaDisplayMode::Fill:
  case BgaDisplayMode::Stretch:
    break;
  default:
    bgaDisplayMode = BgaDisplayMode::Fit;
    break;
  }
  laneAngleDegrees = sanitizeFloat(laneAngleDegrees, kDefaultLaneAngleDegrees,
                                   kMinLaneAngleDegrees, kMaxLaneAngleDegrees);
  laneLength = sanitizeFloat(laneLength, kDefaultLaneLength, kMinLaneLength,
                             kMaxLaneLength);
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
  file << "visual_offset_ms=" << sanitized.visualOffsetMs << "\n";
  file << "visible_time_green_number=" << sanitized.visibleTimeGreenNumber
       << "\n";
  file << "visible_time_use_milliseconds="
       << (sanitized.visibleTimeUseMilliseconds ? 1 : 0) << "\n";
  file << "input_keysound_enabled=" << (sanitized.inputKeysoundEnabled ? 1 : 0)
       << "\n";
  file << "bga_enabled=" << (sanitized.bgaEnabled ? 1 : 0) << "\n";
  file << "bga_brightness_percent=" << sanitized.bgaBrightnessPercent << "\n";
  file << "bga_blur_strength=" << sanitized.bgaBlurStrength << "\n";
  file << "bga_display_mode="
       << bgaDisplayModeToString(sanitized.bgaDisplayMode) << "\n";
  file << "lane_angle_degrees=" << sanitized.laneAngleDegrees << "\n";
  file << "lane_length=" << sanitized.laneLength << "\n";
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
      } else if (key == "visual_offset_ms") {
        settings.visualOffsetMs = std::stoi(value);
      } else if (key == "visible_time_green_number") {
        settings.visibleTimeGreenNumber = std::stoi(value);
      } else if (key == "visible_time_use_milliseconds") {
        bool parsed = settings.visibleTimeUseMilliseconds;
        if (parseBool(value, parsed)) {
          settings.visibleTimeUseMilliseconds = parsed;
        }
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
      } else if (key == "bga_brightness_percent") {
        settings.bgaBrightnessPercent = std::stoi(value);
      } else if (key == "bga_blur_strength") {
        settings.bgaBlurStrength = std::stof(value);
      } else if (key == "bga_display_mode") {
        settings.bgaDisplayMode =
            parseBgaDisplayMode(value, settings.bgaDisplayMode);
      } else if (key == "lane_angle_degrees") {
        settings.laneAngleDegrees = std::stof(value);
      } else if (key == "lane_length") {
        settings.laneLength = std::stof(value);
      }
    } catch (const std::exception &e) {
      SDL_Log("Ignoring malformed settings line '%s': %s", line.c_str(),
              e.what());
    }
  }

  settings.sanitize();
  return settings;
}
