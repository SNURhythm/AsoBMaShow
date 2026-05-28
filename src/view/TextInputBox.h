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

  [[nodiscard]] std::string getText() const;

  [[nodiscard]] inline bool getSelected() const { return isSelected; }

private:
  bool handleEventsImpl(SDL_Event &event) override;
  void renderImpl(RenderContext &context) override;
  std::string editingText;
  std::string composition;
  int compositionCursor = 0;
  int compositionSelectionLength = 0;
  int compositionX = 0;
  int compositionY = 0;
  int compositionWidth = 0;
  int compositionHeight = 0;
  bool isSelected = false;
  bool isDraggingSelection = false;
  SDL_FingerID activeTouchId = -1;
  SDL_FingerID pendingFocusTouchId = -1;
  float pendingFocusUiX = 0.0f;
  float pendingFocusUiY = 0.0f;
  uint64_t pointerDownListenerId = 0;
  size_t selectionAnchor = 0;
  size_t lastRenderedCaretCursor = static_cast<size_t>(-1);
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
  bool hasSelection() const;
  size_t selectionStart() const;
  size_t selectionEnd() const;
  void clearSelection();
  void setCursor(size_t newCursorPos, bool extendSelection);
  bool deleteSelection();
  bool insertTextAtCursor(const std::string &insertedText);
  void clearComposition();
  bool commitComposition();
  void selectAll();
  void copySelectionToClipboard() const;
  std::string displayedText() const;
  size_t compositionDisplayStart() const;
  size_t compositionDisplayEnd() const;
  size_t compositionCursorDisplayPos() const;
  void refreshDisplay(bool notifyTextChanged, bool notifySubmit = false);
  void updateCompositionGeometry(const std::string &display);
  void renderSelection(RenderContext &context, bgfx::ProgramHandle program);
  void registerPointerDownListener();
  void unregisterPointerDownListener();
  void handlePointerDownOutside(const SDL_Event &event);
  void finishEditing();
  void notifyEditingFinished();
  void syncTextInputRect(int cursorX, int cursorY);
};
