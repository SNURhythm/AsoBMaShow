#pragma once

#include <filesystem>

class AppSettings {
public:
  static constexpr int kMinInputOffsetMs = -300;
  static constexpr int kMaxInputOffsetMs = 300;
  static constexpr int kMinVisualOffsetMs = -500;
  static constexpr int kMaxVisualOffsetMs = 500;
  static constexpr int kMinVisibleTimeGreenNumber = 60;
  static constexpr int kMaxVisibleTimeGreenNumber = 1200;
  static constexpr int kMinVisibleTimeMs = 100;
  static constexpr int kMaxVisibleTimeMs = 2000;

  int inputOffsetMs = 0;
  int visualOffsetMs = 0;
  int visibleTimeGreenNumber = 400;
  bool visibleTimeUseMilliseconds = false;
  bool inputKeysoundEnabled = true;
  bool bgaEnabled = true;

  void sanitize();
  bool save() const;
  static AppSettings load();

private:
  static std::filesystem::path configPath();
};
