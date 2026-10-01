"""Exercise production preset feedback splitting with localized UTF-8 text."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler

ROOT = Path(__file__).resolve().parents[1]

class PresetMessageTests(unittest.TestCase):
    def test_split_preserves_utf8_and_text(self):
        method = extract((ROOT / "src/scene/PracticePanelView.cpp").read_text(),
                         "void PracticePanelView::setPresetMessage(")
        fixture = r'''
#include "i18n/Localization.h"
#include <cassert>
#include <iostream>
#include <string>
#include <utility>
using SDL_Color = int;
constexpr int YGDisplayNone = 0, YGDisplayFlex = 1;
namespace ui_theme { int coral() { return 1; } int cyan() { return 2; } int sdl(int c) { return c; } }
struct TextView {
  std::string text;
  int height = 0, display = 0;
  void setText(std::string value) { text = std::move(value); }
  void setColor(int) {}
  void setHeight(int value) { height = value; }
  void setDisplay(int value) { display = value; }
};
struct PracticePanelView {
  TextView first, second;
  TextView *presetMessageText = &first, *presetMessageSecondLine = &second;
  void setPresetMessage(std::string, bool = false);
};
// METHOD
int main() {
  PracticePanelView panel;
  for (auto lang : {i18n::Language::English, i18n::Language::Korean, i18n::Language::Japanese}) {
    i18n::setLanguage(lang);
    panel.setPresetMessage(i18n::tr("chart_viewer.preset_renamed.message"));
    std::cout << panel.first.text << "\n" << panel.second.text << "\n";
  }
  const std::string longText = std::string(39, 'a') + "😀日本語";
  panel.setPresetMessage(longText, true);
  assert(panel.first.text + panel.second.text == longText);
  std::cout << panel.first.text << "\n" << panel.second.text << "\n";
  panel.setPresetMessage("");
  assert(panel.first.text.empty() && panel.second.text.empty());
  assert(panel.first.display == YGDisplayNone && panel.second.display == YGDisplayNone);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            source = folder / "fixture.cpp"
            source.write_text(fixture.replace("// METHOD", method))
            compiler = FixtureCompiler.from_environment()
            binary = folder / ("fixture" + compiler.executable_suffix)
            compiler.build([source, ROOT / "src/i18n/Localization.cpp"], binary,
                           folder, includes=[ROOT / "src"])
            result = subprocess.run([str(binary)], capture_output=True, check=True)
            lines = result.stdout.decode("utf-8", errors="strict").splitlines()
            self.assertEqual("".join(lines[4:6]), "プリセットの名前を変更しました。")
            self.assertEqual("".join(lines[6:8]), "a" * 39 + "😀日本語")

if __name__ == "__main__":
    unittest.main()
