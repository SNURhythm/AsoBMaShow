#include "scene/SettingsScenePreviewAuthority.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPreviewLaneCoverAuthorityMirrorsConfiguredEnablement() {
  AppSettings enabled;
  enabled.noteStartPositionPercent = 63;
  enabled.laneCoverEnabled = true;
  const auto enabledAuthority =
      settings_scene::previewLaneCoverAuthority(enabled);
  expect(enabledAuthority.percent == 63 && enabledAuthority.enabled,
         "lane preview authority keeps an enabled cover at its configured "
         "position");

  AppSettings disabled;
  disabled.noteStartPositionPercent = 37;
  disabled.laneCoverEnabled = false;
  const auto disabledAuthority =
      settings_scene::previewLaneCoverAuthority(disabled);
  expect(disabledAuthority.percent == 37 && !disabledAuthority.enabled,
         "lane preview authority keeps a disabled cover disabled");
}

} // namespace

int main() {
  testPreviewLaneCoverAuthorityMirrorsConfiguredEnablement();
  if (failures != 0) {
    std::cerr << failures << " settings scene preview authority test(s) failed\n";
    return 1;
  }
  std::cout << "settings scene preview authority tests passed\n";
  return 0;
}
