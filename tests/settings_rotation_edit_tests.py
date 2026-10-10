"""Exercise the production rotation edit boundary without a graphics device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class SettingsRotationEditTests(unittest.TestCase):
    def test_rotation_commits_scroll_content_before_switching_orientation(self):
        root = Path(__file__).resolve().parents[1]
        methods = extract((root / "src/scene/SettingsScene.cpp").read_text(),
                          "void SettingsScene::onPresentationOrientationWillChange()")
        methods += "\n" + extract((root / "src/scene/SettingsSceneControls.cpp").read_text(),
                                   "void SettingsScene::closeAppearanceColorPopup()")
        methods += "\n" + extract((root / "src/view/ScrollView.cpp").read_text(),
                                   "void ScrollView::setContentView(View *view)")
        text_input = (root / "src/view/TextInputBox.cpp").read_text()
        for signature in ("void TextInputBox::endEditing()",
                          "void TextInputBox::finishEditing()",
                          "void TextInputBox::notifyEditingFinished()"):
            methods += "\n" + extract(text_input, signature)
        methods = ("namespace platform {\n" + extract(
            (root / "src/platform/SDLMainThread.h").read_text(),
            "inline void stopFocusedTextInput()") + "\n}\n" + methods)
        fixture = r'''
#include <array>
#include "platform/IOSApplicationRuntime.h"
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
struct SDL_Window {};
SDL_Window *SDL_GetKeyboardFocus() { return nullptr; }
bool SDL_StopTextInput(SDL_Window *) { return true; }
struct View {
  virtual ~View() = default;
  std::vector<View *> children;
  auto &getChildren() { return children; }
};
struct ScrollView : View {
  std::unique_ptr<View> contentView;
  void setContentView(View *view);
  PRODUCTION_CONTENT_ACCESSOR
  void refreshContentLayout() {}
};
struct TextInputBox : View {
  bool isSelected = true;
  std::string draft = "37";
  std::vector<std::function<void(const std::string &)>> onEditingFinishedCallbacks;
  bool getSelected() const { return isSelected; }
  std::string getText() const { return draft; }
  void commitComposition() {}
  void onUnselected() { isSelected = false; }
  void endEditing();
  void finishEditing();
  void notifyEditingFinished();
};
struct OverlayPortal {
  bool dismissed = false;
  void dismiss(int *) { dismissed = true; }
};
struct SettingsScene {
  std::vector<View *> views;
  std::unique_ptr<int> appearanceColorPopup = std::make_unique<int>(1);
  std::function<void(int)> appearanceColorApply = [](int) {};
  std::function<void(int)> appearanceColorPreview = [](int) {};
  int previewRestores = 0;
  void syncPreviewPresentationConfiguration() { ++previewRestores; }
  OverlayPortal *overlayPortal = nullptr;
  void closeAppearanceColorPopup();
  void onPresentationOrientationWillChange();
};
PRODUCTION_METHODS
int main() {
  // Cover a plain child, settings scroll content, and nested preview content.
  for (int scrollDepth = 0; scrollDepth <= 2; ++scrollDepth) {
    View root;
    ScrollView emptyScroll, settingsScroll;
    TextInputBox input;
    root.children.push_back(&emptyScroll);
    View *container = &root;
    if (scrollDepth > 0) {
      root.children.push_back(&settingsScroll);
      settingsScroll.setContentView(new View());
      container = settingsScroll.getContentView();
    }
    ScrollView previewScroll;
    if (scrollDepth > 1) {
      container->children.push_back(&previewScroll);
      previewScroll.setContentView(new View());
      container = previewScroll.getContentView();
    }
    container->children.push_back(&input);
    SettingsScene scene{{&root}};
    OverlayPortal portal;
    scene.overlayPortal = &portal;
    int orientation = 0;
    int commits = 0;
    std::array<int, 2> storedValues{10, 20};
    input.onEditingFinishedCallbacks.push_back([&](const std::string &value) {
      storedValues[orientation] = std::stoi(value);
      ++commits;
    });
    scene.onPresentationOrientationWillChange();
    if (scene.appearanceColorPopup || scene.appearanceColorApply || scene.appearanceColorPreview ||
        scene.previewRestores != 1 || !portal.dismissed) {
      std::cerr << "Rotation must discard the color draft before changing orientation";
      return 1;
    }
    orientation = 1;
    scene.onPresentationOrientationWillChange();
    if (storedValues[0] != 37 || storedValues[1] != 20 || commits != 1 ||
        input.getSelected()) {
      std::cerr << "Unfinished edit was not committed to its original orientation"
                << " at scroll depth " << scrollDepth << '\n';
      return 1;
    }
  }
}
'''.replace("PRODUCTION_METHODS", methods).replace(
            "PRODUCTION_CONTENT_ACCESSOR",
            extract((root / "src/view/ScrollView.h").read_text(),
                    "View *getContentView() const"))
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "settings_rotation.cpp"
            executable = Path(directory) / ("settings_rotation" + compiler.executable_suffix)
            source.write_text(fixture)
            compiler.build([str(source)], executable, directory, standard=20, includes=[str(root / "src")])
            result = subprocess.run([str(executable)], cwd=directory,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
