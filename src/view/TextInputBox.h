#pragma once

#include "View.h"
#include "TextView.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <functional>
#include <vector>

class TextInputBox : public TextView {
public:
  TextInputBox(const std::string &fontPath, int fontSize);
  ~TextInputBox() override;

  void setEditingText(const std::string &newText);

  void onSelected() override;
  void onUnselected() override;
  void onMove(int newX, int newY) override;
  void onResize(int newWidth, int newHeight) override;

  size_t onTextChanged(std::function<void(const std::string &)> callback);
  void removeOnTextChanged(std::function<void(const std::string &)> callback);
  size_t onSubmit(std::function<void(const std::string &)> callback);
  void removeOnSubmit(std::function<void(const std::string &)> callback);
  size_t onEditingFinished(std::function<void(const std::string &)> callback);
  void removeOnEditingFinished(
      std::function<void(const std::string &)> callback);

  [[nodiscard]] inline std::string getText() const { return editingText; }

  [[nodiscard]] inline bool getSelected() const { return isSelected; }

private:
  bool handleEventsImpl(SDL_Event &event) override;
  void renderImpl(RenderContext &context) override;
  std::string editingText;
  std::string composition;
  int compositionX = 0;
  int compositionY = 0;
  int compositionWidth = 0;
  int compositionHeight = 0;
  bool isSelected = false;
  int lastRenderedCaretCursor = -1;
  Uint32 lastBlink = 0;
  SDL_Rect viewRect;
  size_t cursorPos = 0;
  std::vector<std::function<void(const std::string &)>> onTextChangedCallbacks;
  std::vector<std::function<void(const std::string &)>> onSubmitCallbacks;
  std::vector<std::function<void(const std::string &)>>
      onEditingFinishedCallbacks;

  // convert cursor position to x, y position
  void cursorToPos(size_t cursorPos, const std::string &text, int &x, int &y);

  // convert x, y position to cursor position
  size_t posToCursor(int x, int y);

  size_t getNextUnicodePos(size_t pos);
  size_t getPrevUnicodePos(size_t pos);
  void notifyEditingFinished();
};
