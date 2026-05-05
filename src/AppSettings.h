#pragma once

#include <filesystem>

class AppSettings {
public:
  static constexpr int kMinInputOffsetMs = -300;
  static constexpr int kMaxInputOffsetMs = 300;

  int inputOffsetMs = 0;
  bool inputKeysoundEnabled = true;
  bool bgaEnabled = true;

  void sanitize();
  bool save() const;
  static AppSettings load();

private:
  static std::filesystem::path configPath();
};
