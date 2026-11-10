#pragma once

#include "../view/BlockingOverlayView.h"

#include <array>
#include <functional>
#include <string>

class Button;
class TextView;

enum class NewcomerTutorialStep { Language, Tables, Download, Folder, PlayOptions };

struct NewcomerTutorialCallbacks {
  std::function<bool(const std::string &)> saveLanguage;
  std::function<bool()> complete;
  std::function<View *(NewcomerTutorialStep)> target;
  std::function<void(NewcomerTutorialStep)> stepChanged;
};

// Owned by the scene's view tree. The spotlight blocks interactions with the
// highlighted controls: this tour explains actions without performing them.
class NewcomerTutorialView final : public BlockingOverlayView {
public:
  explicit NewcomerTutorialView(NewcomerTutorialCallbacks callbacks,
                                bool folderImportCopies = false);
  NewcomerTutorialStep step() const { return step_; }
  void chooseLanguage(const std::string &language);
  void advance();
  void back();
  void skip();
  void updateLayout(int width, int height);

private:
  bool handleEventsImpl(SDL_Event &event) override;
  void refresh();
  void changeStep(NewcomerTutorialStep step);
  bool saveLanguage();
  void finish();

  NewcomerTutorialCallbacks callbacks_;
  NewcomerTutorialStep step_ = NewcomerTutorialStep::Language;
  std::string language_;
  bool folderImportCopies_ = false;
  bool saveFailed_ = false;
  std::array<View *, 4> scrims_{};
  View *highlight_ = nullptr;
  View *panel_ = nullptr;
  View *languages_ = nullptr;
  std::array<Button *, 5> languageButtons_{};
  TextView *progress_ = nullptr;
  TextView *title_ = nullptr;
  TextView *body_ = nullptr;
  TextView *status_ = nullptr;
  TextView *nextText_ = nullptr;
  Button *back_ = nullptr;
};
