#pragma once

#include "../i18n/Localization.h"
#include <string>

namespace public_apology {
inline std::string assetPath(i18n::Language language) {
  const char *locale = "en";
  switch (language) {
  case i18n::Language::English: break;
  case i18n::Language::Korean: locale = "ko"; break;
  case i18n::Language::Japanese: locale = "ja"; break;
  case i18n::Language::SimplifiedChinese: locale = "zh-Hans"; break;
  case i18n::Language::TraditionalChinese: locale = "zh-Hant"; break;
  }
  return std::string("assets/notices/apology-2026-11-08.") + locale + ".txt";
}
}
