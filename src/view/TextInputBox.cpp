#include "TextInputBox.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "Button.h"
#include "IconText.h"
#include "UiTheme.h"
#include "SDL2/SDL_events.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {
SDL_Cursor *s_ibeamCursor = nullptr;
SDL_Cursor *s_arrowCursor = nullptr;

SDL_Cursor *getCachedCursor(SDL_SystemCursor cursorType) {
  SDL_Cursor **slot =
      cursorType == SDL_SYSTEM_CURSOR_IBEAM ? &s_ibeamCursor : &s_arrowCursor;
  if (*slot == nullptr) {
    *slot = SDL_CreateSystemCursor(cursorType);
  }
  return *slot;
}

void updateHoverCursor(bool useIBeam) {
  SDL_Cursor *target = getCachedCursor(useIBeam ? SDL_SYSTEM_CURSOR_IBEAM
                                                : SDL_SYSTEM_CURSOR_ARROW);
  if (target != nullptr && SDL_GetCursor() != target) {
    SDL_SetCursor(target);
  }
}

bool isInsideTextInput(const TextInputBox &input, float uiX, float uiY) {
  return uiX >= input.getX() && uiX <= input.getX() + input.getWidth() &&
         uiY >= input.getY() && uiY <= input.getY() + input.getHeight();
}

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                     float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}

constexpr float kTouchFocusSlopUi = 12.0f;

bool isUtf8ContinuationByte(unsigned char value) {
  return (value & 0b11000000) == 0b10000000;
}

bool hasShortcutModifier(SDL_Keymod mods) {
  return (mods & (KMOD_CTRL | KMOD_GUI)) != 0;
}

bool hasShiftModifier(SDL_Keymod mods) { return (mods & KMOD_SHIFT) != 0; }

uint32_t sdlColorToAbgr(SDL_Color color) {
  return (static_cast<uint32_t>(color.r) << 24) |
         (static_cast<uint32_t>(color.g) << 16) |
         (static_cast<uint32_t>(color.b) << 8) | static_cast<uint32_t>(color.a);
}

size_t clampUtf8Boundary(const std::string &text, size_t pos) {
  pos = std::min(pos, text.size());
  while (pos < text.size() &&
         isUtf8ContinuationByte(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
  return pos;
}

size_t utf8CharacterOffsetToByteOffset(const std::string &text,
                                       int characterOffset) {
  if (characterOffset <= 0) {
    return 0;
  }

  size_t byteOffset = 0;
  for (int i = 0; i < characterOffset && byteOffset < text.size(); ++i) {
    ++byteOffset;
    while (
        byteOffset < text.size() &&
        isUtf8ContinuationByte(static_cast<unsigned char>(text[byteOffset]))) {
      ++byteOffset;
    }
  }
  return byteOffset;
}

void submitRect(RenderContext &context, bgfx::ProgramHandle program, int x,
                int y, int width, int height, uint32_t color) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const std::array vertices = {
      rendering::PosColorVertex{static_cast<float>(x), static_cast<float>(y),
                                0.0f, color},
      rendering::PosColorVertex{static_cast<float>(x + width),
                                static_cast<float>(y), 0.0f, color},
      rendering::PosColorVertex{static_cast<float>(x + width),
                                static_cast<float>(y + height), 0.0f, color},
      rendering::PosColorVertex{static_cast<float>(x),
                                static_cast<float>(y + height), 0.0f, color}};
  constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
  context.appendUiColor(
      vertices, indices,
      context.makeUiBatchState(
          program, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA));
}
} // namespace

void TextInputBox::releaseCachedCursors() {
  if (s_ibeamCursor != nullptr) {
    SDL_FreeCursor(s_ibeamCursor);
    s_ibeamCursor = nullptr;
  }
  if (s_arrowCursor != nullptr) {
    SDL_FreeCursor(s_arrowCursor);
    s_arrowCursor = nullptr;
  }
}

TextInputBox::TextInputBox(const std::string &fontPath, int fontSize)
    : TextView(fontPath, fontSize) {
  View::setPadding(Edge::Left, 12.0f);
  View::setPadding(Edge::Right, 12.0f);
  viewRect = {getX(), getY(), getWidth(), getHeight()};
}

TextInputBox::~TextInputBox() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  hideNativeTextEditor(false);
#endif
  unregisterPointerDownListener();
  if (isSelected) {
    SDL_StopTextInput();
  }
}

std::string TextInputBox::getText() const { return displayedText(); }

void TextInputBox::beginEditing() {
  if (isSelected) return;
  applyYogaLayout();
  refreshDisplay(false);
  onSelected();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  showNativeTextEditor();
#else
  SDL_StartTextInput();
#endif
}

void TextInputBox::endEditing() { finishEditing(); }

void TextInputBox::setClearable(bool value) {
  clearable = value;
  if (clearable && clearButton == nullptr) {
    clearButton = std::make_unique<Button>();
    auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath,
                              std::max(12, fontSize - 2));
    icon->setText(ui_icons::textForCodepoint(ui_icons::kXmark));
    icon->setAlign(TextView::CENTER);
    icon->setVAlign(TextView::MIDDLE);
    icon->setOverflow(TextView::TextOverflow::Hidden);
    icon->setThemedColor(ui_theme::textMuted);
    clearButton->setContentView(icon);
    clearButton->setOnClickListener([this]() { clearFromButton(); });
  }
  syncClearButton();
}

bool TextInputBox::isClearButtonVisible() const noexcept {
  return clearButton != nullptr && clearButton->getVisible();
}

void TextInputBox::syncTextInputRect(int cursorX, int cursorY) {
  const int lineHeight = std::max(1, rect.h > 0 ? rect.h : textLineHeight());
  const int lineWidth = std::max(1, rect.w);
  viewRect = nativeRectFromUiRect(cursorX, cursorY, lineWidth, lineHeight);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (nativeTextEditorVisible) {
    viewRect.h = std::max(1, viewRect.h + GetIOSNativeTextEditorHeight());
  }
#endif
  SDL_SetTextInputRect(&viewRect);
}

void TextInputBox::setEditingText(const std::string &newText) {
  editingText = newText;
  clearComposition();
  cursorPos = editingText.size();
  selectionAnchor = cursorPos;
  isDraggingSelection = false;
  activeTouchId = -1;
  pendingFocusTouchId = -1;
  lastRenderedCaretCursor = static_cast<size_t>(-1);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  syncNativeTextEditorText();
#endif
  refreshDisplay(false);
}

size_t TextInputBox::getNextUnicodePos(size_t pos) {
  pos = std::min(pos, editingText.size());
  if (pos >= editingText.size()) {
    return pos;
  }

  ++pos;
  while (pos < editingText.size() &&
         isUtf8ContinuationByte(static_cast<unsigned char>(editingText[pos]))) {
    ++pos;
  }
  return pos;
}
size_t TextInputBox::getPrevUnicodePos(size_t pos) {
  pos = std::min(pos, editingText.size());
  if (pos == 0) {
    return 0;
  }

  --pos;
  while (pos > 0 &&
         isUtf8ContinuationByte(static_cast<unsigned char>(editingText[pos]))) {
    --pos;
  }
  return pos;
}
bool TextInputBox::handleEventsImpl(SDL_Event &event) {
  if (isClearButtonVisible() && !clearButton->handleEvents(event)) {
    return false;
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (nativeTextEditorVisible &&
      (event.type == SDL_TEXTINPUT || event.type == SDL_TEXTEDITING ||
       event.type == SDL_TEXTEDITING_EXT || event.type == SDL_KEYDOWN)) {
    return false;
  }
#endif

  bool displayChanged = false;
  bool textChanged = false;
  bool isSubmit = false;
  switch (event.type) {
  case SDL_TEXTINPUT:
    if (!isSelected) {
      return true;
    }
    clearComposition();
    textChanged = insertTextAtCursor(event.text.text);
    displayChanged = true;
    break;
  case SDL_KEYDOWN: {
    if (!isSelected) {
      return true;
    }

    const SDL_Keycode key = event.key.keysym.sym;
    const SDL_Keymod mods = static_cast<SDL_Keymod>(event.key.keysym.mod);
    const bool shortcutHeld = hasShortcutModifier(mods);
    const bool shiftHeld = hasShiftModifier(mods);
    const bool navigationKey = key == SDLK_RIGHT || key == SDLK_LEFT ||
                               key == SDLK_HOME || key == SDLK_END;

    if (!composition.empty()) {
      if (key == SDLK_ESCAPE) {
        const std::string previousText = displayedText();
        clearComposition();
        displayChanged = true;
        textChanged = displayedText() != previousText;
        break;
      }
      if (navigationKey) {
        commitComposition();
      } else {
        break;
      }
    }

    if (shortcutHeld && key == SDLK_a) {
      selectAll();
      displayChanged = true;
      break;
    }
    if (shortcutHeld && key == SDLK_c) {
      copySelectionToClipboard();
      return false;
    }
    if (shortcutHeld && key == SDLK_x) {
      copySelectionToClipboard();
      textChanged = deleteSelection();
      displayChanged = textChanged;
      break;
    }
    if (shortcutHeld && key == SDLK_v) {
      textChanged = pasteClipboardAtCursor();
      displayChanged = textChanged;
      break;
    }

    if (key == SDLK_BACKSPACE) {
      if (hasSelection()) {
        textChanged = deleteSelection();
      } else if (cursorPos > 0) {
        size_t prevPos = getPrevUnicodePos(cursorPos);
        if (mods & KMOD_GUI) {
          prevPos = 0;
        } else if (mods & (KMOD_CTRL | KMOD_ALT)) {
          while (prevPos > 0 && std::isspace(static_cast<unsigned char>(
                                    editingText[getPrevUnicodePos(prevPos)]))) {
            prevPos = getPrevUnicodePos(prevPos);
          }
          while (prevPos > 0 && !std::isspace(static_cast<unsigned char>(
                                    editingText[getPrevUnicodePos(prevPos)]))) {
            prevPos = getPrevUnicodePos(prevPos);
          }
        }
        editingText.erase(prevPos, cursorPos - prevPos);
        setCursor(prevPos, false);
        textChanged = true;
      }
      displayChanged = textChanged;
    } else if (key == SDLK_DELETE) {
      if (hasSelection()) {
        textChanged = deleteSelection();
      } else if (cursorPos < editingText.size()) {
        size_t nextPos = getNextUnicodePos(cursorPos);
        if (mods & KMOD_GUI) {
          nextPos = editingText.size();
        } else if (mods & (KMOD_CTRL | KMOD_ALT)) {
          while (
              nextPos < editingText.size() &&
              std::isspace(static_cast<unsigned char>(editingText[nextPos]))) {
            nextPos = getNextUnicodePos(nextPos);
          }
          while (
              nextPos < editingText.size() &&
              !std::isspace(static_cast<unsigned char>(editingText[nextPos]))) {
            nextPos = getNextUnicodePos(nextPos);
          }
        }
        editingText.erase(cursorPos, nextPos - cursorPos);
        textChanged = true;
      }
      displayChanged = textChanged;
    } else if (key == SDLK_RIGHT) {
      if (hasSelection() && !shiftHeld) {
        setCursor(selectionEnd(), false);
      } else {
        setCursor(getNextUnicodePos(cursorPos), shiftHeld);
      }
      displayChanged = true;
    } else if (key == SDLK_LEFT) {
      if (hasSelection() && !shiftHeld) {
        setCursor(selectionStart(), false);
      } else {
        setCursor(getPrevUnicodePos(cursorPos), shiftHeld);
      }
      displayChanged = true;
    } else if (key == SDLK_HOME) {
      setCursor(0, shiftHeld);
      displayChanged = true;
    } else if (key == SDLK_END) {
      setCursor(editingText.size(), shiftHeld);
      displayChanged = true;
    } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
      isSubmit = true;
    }

    break;
  }
  case SDL_TEXTEDITING:
    if (!isSelected) {
      return true;
    }
    {
      const std::string previousText = displayedText();
      composition = event.edit.text;
      compositionCursor = event.edit.start;
      compositionSelectionLength = event.edit.length;
      textChanged = displayedText() != previousText;
    }
    displayChanged = true;
    break;
  case SDL_TEXTEDITING_EXT:
    if (!isSelected) {
      return true;
    }
    {
      const std::string previousText = displayedText();
      composition = event.editExt.text != nullptr ? event.editExt.text : "";
      compositionCursor = event.editExt.start;
      compositionSelectionLength = event.editExt.length;
      textChanged = displayedText() != previousText;
    }
    displayChanged = true;
    break;
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      break;
    }

    int screenX = static_cast<int>(event.button.x * rendering::widthScale);
    int screenY = static_cast<int>(event.button.y * rendering::heightScale);
    int x = 0;
    int y = 0;
    rendering::screenToUi(screenX, screenY, x, y);
    if (isInsideTextInput(*this, static_cast<float>(x),
                          static_cast<float>(y))) {
      commitComposition();
      onSelected();
      setCursor(posToCursor(x - getX(), y - getY()),
                hasShiftModifier(static_cast<SDL_Keymod>(SDL_GetModState())));
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
      showNativeTextEditor();
      isDraggingSelection = false;
#else
      SDL_StartTextInput();
      isDraggingSelection = true;
#endif
      displayChanged = true;
      break;
    }

    finishEditing();
    break;
  }
  case SDL_MOUSEBUTTONUP:
    if (event.button.button == SDL_BUTTON_LEFT &&
        event.button.which != SDL_TOUCH_MOUSEID && isDraggingSelection) {
      isDraggingSelection = false;
      return false;
    }
    break;
  case SDL_MOUSEMOTION: {
    if (event.motion.which == SDL_TOUCH_MOUSEID) {
      break;
    }

    int screenX = static_cast<int>(event.motion.x * rendering::widthScale);
    int screenY = static_cast<int>(event.motion.y * rendering::heightScale);
    int x = 0;
    int y = 0;
    rendering::screenToUi(screenX, screenY, x, y);

    if (isSelected && isDraggingSelection &&
        (event.motion.state & SDL_BUTTON_LMASK)) {
      setCursor(posToCursor(x - getX(), y - getY()), true);
      displayChanged = true;
      break;
    }

    updateHoverCursor(x >= getX() && x <= getX() + getWidth() && y >= getY() &&
                      y <= getY() + getHeight());
    break;
  }
  case SDL_FINGERDOWN: {
    pendingFocusTouchId = -1;
    if (activeTouchId != -1) {
      return true;
    }

    float x = 0.0f;
    float y = 0.0f;
    fingerEventToUi(event.tfinger, x, y);
    if (isInsideTextInput(*this, x, y)) {
      if (!isSelected) {
        pendingFocusTouchId = event.tfinger.fingerId;
        pendingFocusUiX = x;
        pendingFocusUiY = y;
        return false;
      }
      commitComposition();
      onSelected();
      setCursor(posToCursor(static_cast<int>(x) - getX(),
                            static_cast<int>(y) - getY()),
                false);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
      showNativeTextEditor();
      activeTouchId = event.tfinger.fingerId;
      isDraggingSelection = false;
#else
      SDL_StartTextInput();
      activeTouchId = event.tfinger.fingerId;
      isDraggingSelection = true;
#endif
      displayChanged = true;
      break;
    }

    finishEditing();
    break;
  }
  case SDL_FINGERMOTION: {
    if (event.tfinger.fingerId == pendingFocusTouchId) {
      float x = 0.0f;
      float y = 0.0f;
      fingerEventToUi(event.tfinger, x, y);
      if (std::fabs(x - pendingFocusUiX) >= kTouchFocusSlopUi ||
          std::fabs(y - pendingFocusUiY) >= kTouchFocusSlopUi) {
        pendingFocusTouchId = -1;
      }
      return true;
    }
    if (event.tfinger.fingerId != activeTouchId) {
      return true;
    }

    float x = 0.0f;
    float y = 0.0f;
    fingerEventToUi(event.tfinger, x, y);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    if (isSelected) {
      return false;
    }
#endif
    if (isSelected && isDraggingSelection) {
      setCursor(posToCursor(static_cast<int>(x) - getX(),
                            static_cast<int>(y) - getY()),
                true);
      displayChanged = true;
      break;
    }
    return false;
  }
  case SDL_FINGERUP: {
    if (event.tfinger.fingerId == pendingFocusTouchId) {
      pendingFocusTouchId = -1;
      float x = 0.0f;
      float y = 0.0f;
      fingerEventToUi(event.tfinger, x, y);
      if (!isInsideTextInput(*this, x, y)) {
        return true;
      }
      commitComposition();
      onSelected();
      setCursor(posToCursor(static_cast<int>(x) - getX(),
                            static_cast<int>(y) - getY()),
                false);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
      showNativeTextEditor();
#else
      SDL_StartTextInput();
#endif
      refreshDisplay(false);
      return false;
    }
    if (event.tfinger.fingerId == activeTouchId) {
      activeTouchId = -1;
      isDraggingSelection = false;
      return false;
    }
    break;
  }
  }
  if (displayChanged || textChanged || isSubmit) {
    refreshDisplay(textChanged, isSubmit);
  }

  if (isSelected &&
      (event.type == SDL_TEXTINPUT || event.type == SDL_TEXTEDITING ||
       event.type == SDL_TEXTEDITING_EXT || event.type == SDL_KEYDOWN ||
       ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEMOTION) &&
        displayChanged) ||
       ((event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION) &&
        displayChanged))) {
    return false;
  }
  return true;
}
void TextInputBox::onMove(int newX, int newY) {
  TextView::onMove(newX, newY);
  syncClearButtonFrame();
  refreshDisplay(false);
}
void TextInputBox::onResize(int newWidth, int newHeight) {
  TextView::onResize(newWidth, newHeight);
  syncClearButtonFrame();
  refreshDisplay(false);
}

void TextInputBox::propagateThemeChange() {
  TextView::propagateThemeChange();
  if (clearButton != nullptr) {
    clearButton->propagateThemeChange();
  }
}

void TextInputBox::renderImpl(RenderContext &context) {
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  const auto renderTextContent = [&]() {
    if (isSelected) {
      renderSelection(context, kSimpleProgram);
    }

    TextView::renderImpl(context);
    if (!isSelected) {
      return;
    }

    const size_t caretDisplayCursor =
        composition.empty() ? cursorPos : compositionCursorDisplayPos();
    const Uint32 blinkInterval = 500;
    const Uint32 currentTime = SDL_GetTicks();
    if (lastRenderedCaretCursor != caretDisplayCursor) {
      lastBlink = currentTime;
      lastRenderedCaretCursor = caretDisplayCursor;
    }

    const bool showCaret =
        (currentTime - lastBlink) % (2 * blinkInterval) < blinkInterval;

    if (showCaret) {
      int caretX = 0;
      int caretY = 0;
      cursorToPos(caretDisplayCursor, text, caretX, caretY);
      int height = rect.h;
      if (height == 0) {
        height = textLineHeight();
      }
      submitRect(context, kSimpleProgram, caretX, caretY, 2, height,
                 sdlColorToAbgr(color));
    }

    if (!composition.empty()) {
      submitRect(context, kSimpleProgram, compositionX, compositionY,
                 compositionWidth, 2, 0xFFFFFFFF);
    }
  };

  if (isClearButtonVisible()) {
    ScissorScope scissor(context, getContentX(), getContentY(),
                         getContentWidth(), getContentHeight());
    renderTextContent();
  } else {
    renderTextContent();
  }

  if (isClearButtonVisible()) {
    clearButton->render(context);
  }
}

void TextInputBox::cursorToPos(size_t cursorPos, const std::string &text,
                               int &x, int &y) {
  // TODO: this will not work for multi-line text
  std::string utf8 = text;
  if (cursorPos > utf8.size())
    cursorPos = utf8.size();
  utf8.resize(cursorPos);

  int textWidth = 0;
  if (!utf8.empty()) {
    textWidth = measureTextWidth(utf8);
  }

  const SDL_Rect contentRect = resolvedTextRect();
  x = contentRect.x + textWidth;
  y = contentRect.y;
}

size_t TextInputBox::posToCursor(int x, int y) {
  // TODO: this will not work for multi-line text
  (void)y;
  size_t cursorPos = 0;
  int measuredWidth = 0;
  const SDL_Rect contentRect = resolvedTextRect();
  const int localX = std::max(0, x - (contentRect.x - getX()));

  for (cursorPos = 0; cursorPos < editingText.size();) {
    const size_t nextPos = getNextUnicodePos(cursorPos);
    const int glyphWidth =
        measureTextWidth(editingText.substr(cursorPos, nextPos - cursorPos));
    if (localX < measuredWidth + glyphWidth / 2) {
      return cursorPos;
    }
    measuredWidth += glyphWidth;
    cursorPos = nextPos;
  }
  return cursorPos;
}

bool TextInputBox::hasSelection() const { return selectionAnchor != cursorPos; }

size_t TextInputBox::selectionStart() const {
  return std::min(selectionAnchor, cursorPos);
}

size_t TextInputBox::selectionEnd() const {
  return std::max(selectionAnchor, cursorPos);
}

void TextInputBox::clearSelection() { selectionAnchor = cursorPos; }

void TextInputBox::setCursor(size_t newCursorPos, bool extendSelection) {
  newCursorPos = clampUtf8Boundary(editingText, newCursorPos);
  if (extendSelection) {
    if (!hasSelection()) {
      selectionAnchor = cursorPos;
    }
    cursorPos = newCursorPos;
  } else {
    cursorPos = newCursorPos;
    selectionAnchor = cursorPos;
  }
  lastRenderedCaretCursor = static_cast<size_t>(-1);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (nativeTextEditorVisible) {
    syncNativeTextEditorSelection();
  }
#endif
}

bool TextInputBox::deleteSelection() {
  if (!hasSelection()) {
    return false;
  }
  const size_t start = selectionStart();
  const size_t end = selectionEnd();
  editingText.erase(start, end - start);
  cursorPos = start;
  selectionAnchor = start;
  lastRenderedCaretCursor = static_cast<size_t>(-1);
  return true;
}

bool TextInputBox::insertTextAtCursor(const std::string &insertedText) {
  if (insertedText.empty()) {
    return false;
  }
  deleteSelection();
  editingText.insert(cursorPos, insertedText);
  setCursor(cursorPos + insertedText.size(), false);
  return true;
}

void TextInputBox::selectAll() {
  selectionAnchor = 0;
  cursorPos = editingText.size();
  lastRenderedCaretCursor = static_cast<size_t>(-1);
}

void TextInputBox::copySelectionToClipboard() const {
  if (!hasSelection()) {
    return;
  }
  const std::string selectedText =
      editingText.substr(selectionStart(), selectionEnd() - selectionStart());
  SDL_SetClipboardText(selectedText.c_str());
}

bool TextInputBox::pasteClipboardAtCursor() {
  if (!SDL_HasClipboardText()) {
    return false;
  }

  char *clipboardText = SDL_GetClipboardText();
  if (clipboardText == nullptr) {
    return false;
  }
  const bool textChanged = insertTextAtCursor(clipboardText);
  SDL_free(clipboardText);
  return textChanged;
}

void TextInputBox::clearComposition() {
  composition.clear();
  compositionCursor = 0;
  compositionSelectionLength = 0;
  lastRenderedCaretCursor = static_cast<size_t>(-1);
}

bool TextInputBox::commitComposition() {
  if (composition.empty()) {
    return false;
  }

  const size_t start = compositionDisplayStart();
  const size_t end = hasSelection() ? selectionEnd() : cursorPos;
  editingText.replace(start, end - start, composition);
  cursorPos = start + composition.size();
  selectionAnchor = cursorPos;
  clearComposition();
  SDL_ClearComposition();
  return true;
}

std::string TextInputBox::displayedText() const {
  std::string display = editingText;
  if (!composition.empty()) {
    const size_t replaceStart = compositionDisplayStart();
    const size_t replaceEnd = hasSelection() ? selectionEnd() : cursorPos;
    display.replace(replaceStart, replaceEnd - replaceStart, composition);
  }
  return display;
}

size_t TextInputBox::compositionDisplayStart() const {
  return hasSelection() ? selectionStart() : cursorPos;
}

size_t TextInputBox::compositionDisplayEnd() const {
  return compositionDisplayStart() + composition.size();
}

size_t TextInputBox::compositionCursorDisplayPos() const {
  if (composition.empty()) {
    return cursorPos;
  }
  return compositionDisplayStart() +
         utf8CharacterOffsetToByteOffset(composition, compositionCursor);
}

void TextInputBox::refreshDisplay(bool notifyTextChanged, bool notifySubmit) {
  const std::string display = displayedText();
  TextView::setText(display);
  syncClearButton();
  updateCompositionGeometry(display);

  if (notifyTextChanged) {
    for (auto &callback : onTextChangedCallbacks) {
      callback(display);
    }
  }
  if (notifySubmit) {
    for (auto &callback : onSubmitCallbacks) {
      callback(display);
    }
    notifyEditingFinished();
  }

  int cursorX = 0;
  int cursorY = 0;
  cursorToPos(composition.empty() ? cursorPos : compositionCursorDisplayPos(),
              display, cursorX, cursorY);
  syncTextInputRect(cursorX, cursorY);
}

void TextInputBox::syncClearButton() {
  const bool visible = clearable && !displayedText().empty();
  if (clearButton != nullptr) {
    clearButton->setVisible(visible);
  }

  const int desiredPadding = visible ? 48 : 12;
  if (trailingContentPadding != desiredPadding) {
    trailingContentPadding = desiredPadding;
    View::setPadding(Edge::Right,
                     static_cast<float>(trailingContentPadding));
  }
  syncClearButtonFrame();
}

void TextInputBox::syncClearButtonFrame() {
  if (clearButton == nullptr) {
    return;
  }
  constexpr int kClearButtonWidth = 40;
  clearButton->setSize(kClearButtonWidth, getHeight());
  clearButton->setPositionNoLayout(getX() + getWidth() - kClearButtonWidth,
                                   getY());
}

void TextInputBox::clearFromButton() {
  if (editingText.empty() && composition.empty()) {
    return;
  }
  editingText.clear();
  clearComposition();
  SDL_ClearComposition();
  cursorPos = 0;
  selectionAnchor = 0;
  isDraggingSelection = false;
  lastRenderedCaretCursor = static_cast<size_t>(-1);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  syncNativeTextEditorText();
#endif
  refreshDisplay(true);
}

void TextInputBox::updateCompositionGeometry(const std::string &display) {
  if (composition.empty()) {
    compositionX = 0;
    compositionY = 0;
    compositionWidth = 0;
    compositionHeight = 0;
    return;
  }

  int compositionEndX = 0;
  int compositionEndY = 0;
  cursorToPos(compositionDisplayStart(), display, compositionX, compositionY);
  cursorToPos(compositionDisplayEnd(), display, compositionEndX,
              compositionEndY);
  compositionWidth = std::max(1, compositionEndX - compositionX);
  const int underlineHeight = 2;
  const int contentHeight =
      std::max(underlineHeight, rect.h > 0 ? rect.h : textLineHeight());
  compositionHeight = contentHeight;
  compositionY =
      resolvedTextRect().y + std::max(0, contentHeight - underlineHeight);
}

void TextInputBox::renderSelection(RenderContext &context,
                                   bgfx::ProgramHandle program) {
  int startX = 0;
  int startY = 0;
  int endX = 0;
  int endY = 0;
  if (!composition.empty()) {
    if (compositionSelectionLength <= 0) {
      return;
    }
    const size_t displayStart = compositionDisplayStart();
    const int selectionStartCharacters = std::max(0, compositionCursor);
    const int selectionEndCharacters =
        selectionStartCharacters + std::max(0, compositionSelectionLength);
    const size_t selectionStartOffset =
        utf8CharacterOffsetToByteOffset(composition, selectionStartCharacters);
    const size_t selectionEndOffset =
        utf8CharacterOffsetToByteOffset(composition, selectionEndCharacters);
    if (selectionStartOffset == selectionEndOffset) {
      return;
    }
    cursorToPos(displayStart + selectionStartOffset, text, startX, startY);
    cursorToPos(displayStart + selectionEndOffset, text, endX, endY);
  } else {
    if (!hasSelection()) {
      return;
    }
    cursorToPos(selectionStart(), editingText, startX, startY);
    cursorToPos(selectionEnd(), editingText, endX, endY);
  }

  const int selectionX = std::min(startX, endX);
  const int selectionWidth = std::max(1, std::abs(endX - startX));
  const int selectionHeight = rect.h > 0 ? rect.h : textLineHeight();
  const SDL_Color selectionColor = {70, 128, 210, 118};
  submitRect(context, program, selectionX, resolvedTextRect().y, selectionWidth,
             selectionHeight, sdlColorToAbgr(selectionColor));
}

SDL_Rect TextInputBox::nativeRectFromUiRect(int x, int y, int width,
                                            int height) const {
  SDL_Rect nativeRect = {x, y, std::max(1, width), std::max(1, height)};

  SDL_Window *window = SDL_GetKeyboardFocus();
  if (window == nullptr) {
    window = SDL_GetMouseFocus();
  }

  int logicalW = 0;
  int logicalH = 0;
  if (window != nullptr) {
    SDL_GetWindowSize(window, &logicalW, &logicalH);
  }

  if (logicalW > 0 && logicalH > 0 && rendering::window_width > 0 &&
      rendering::window_height > 0) {
    const float scaleX = static_cast<float>(logicalW) /
                         static_cast<float>(rendering::window_width);
    const float scaleY = static_cast<float>(logicalH) /
                         static_cast<float>(rendering::window_height);
    nativeRect.x =
        static_cast<int>(std::lround(static_cast<float>(x) * scaleX));
    nativeRect.y =
        static_cast<int>(std::lround(static_cast<float>(y) * scaleY));
    nativeRect.w =
        std::max(1, static_cast<int>(std::lround(width * scaleX)));
    nativeRect.h =
        std::max(1, static_cast<int>(std::lround(height * scaleY)));
  }

  return nativeRect;
}

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
void TextInputBox::showNativeTextEditor() {
  if (nativeTextEditorVisible) {
    return;
  }
  commitComposition();
  if (!isSelected) {
    onSelected();
  }
  SDL_StopTextInput();
  nativeTextEditorVisible = true;
  isDraggingSelection = false;
  refreshDisplay(false);

  IOSNativeTextEditorConfig config;
  config.text = editingText;
  config.selectionStart = selectionStart();
  config.selectionEnd = selectionEnd();
  config.fontSize = std::max(12, fontSize);
  ShowIOSNativeTextEditor(config, this,
                          &TextInputBox::handleNativeTextEditorEvent);
}

void TextInputBox::hideNativeTextEditor(bool notifyFinished) {
  if (nativeTextEditorVisible) {
    HideIOSNativeTextEditor(this, notifyFinished);
  }
  nativeTextEditorVisible = false;
}

bool TextInputBox::syncNativeTextEditorState(
    const IOSNativeTextEditorState &state, bool &selectionChanged) {
  const bool textChanged = editingText != state.text || !composition.empty();
  if (textChanged) {
    editingText = state.text;
    clearComposition();
  }

  const size_t newSelectionStart =
      clampUtf8Boundary(editingText, state.selectionStart);
  const size_t newSelectionEnd =
      clampUtf8Boundary(editingText, state.selectionEnd);
  selectionChanged =
      selectionAnchor != newSelectionStart || cursorPos != newSelectionEnd;
  if (selectionChanged) {
    selectionAnchor = newSelectionStart;
    cursorPos = newSelectionEnd;
    lastRenderedCaretCursor = static_cast<size_t>(-1);
  }

  return textChanged;
}

void TextInputBox::syncNativeTextEditorSelection() {
  SetIOSNativeTextEditorSelection(this, selectionStart(), selectionEnd());
}

void TextInputBox::syncNativeTextEditorText() {
  if (!nativeTextEditorVisible) {
    return;
  }
  SetIOSNativeTextEditorState(
      this, {.text = editingText,
             .selectionStart = selectionStart(),
             .selectionEnd = selectionEnd()});
}

void TextInputBox::handleNativeTextEditorEvent(
    IOSNativeTextEditorEvent event, const IOSNativeTextEditorState &state) {
  if (!isSelected) {
    isSelected = true;
    registerPointerDownListener();
  }
  bool selectionChanged = false;
  const bool textChanged = syncNativeTextEditorState(state, selectionChanged);

  switch (event) {
  case IOSNativeTextEditorEvent::Changed:
    nativeTextEditorVisible = true;
    if (textChanged || selectionChanged) {
      refreshDisplay(textChanged);
    }
    return;
  case IOSNativeTextEditorEvent::SelectionChanged:
    nativeTextEditorVisible = true;
    if (textChanged || selectionChanged) {
      refreshDisplay(textChanged);
    }
    return;
  case IOSNativeTextEditorEvent::Submitted:
    nativeTextEditorVisible = false;
    refreshDisplay(textChanged, true);
    onUnselected();
    SDL_StopTextInput();
    return;
  case IOSNativeTextEditorEvent::Finished:
    nativeTextEditorVisible = false;
    if (textChanged) {
      refreshDisplay(true);
    }
    notifyEditingFinished();
    onUnselected();
    SDL_StopTextInput();
    return;
  }
}

void TextInputBox::handleNativeTextEditorEvent(
    void *context, IOSNativeTextEditorEvent event,
    const IOSNativeTextEditorState &state) {
  if (context == nullptr) {
    return;
  }
  static_cast<TextInputBox *>(context)->handleNativeTextEditorEvent(event,
                                                                    state);
}
#endif

void TextInputBox::onSelected() {
  isSelected = true;
  registerPointerDownListener();
}

void TextInputBox::onUnselected() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  hideNativeTextEditor(false);
#endif
  unregisterPointerDownListener();
  isSelected = false;
  isDraggingSelection = false;
  activeTouchId = -1;
  pendingFocusTouchId = -1;
  commitComposition();
  clearSelection();
  TextView::setText(editingText);
}

void TextInputBox::registerPointerDownListener() {
  if (pointerDownListenerId != 0) {
    return;
  }
  pointerDownListenerId = View::addTemporaryEventListener(
      [this](SDL_Event &event) { handlePointerDownOutside(event); });
}

void TextInputBox::unregisterPointerDownListener() {
  View::removeTemporaryEventListener(pointerDownListenerId);
  pointerDownListenerId = 0;
}

void TextInputBox::handlePointerDownOutside(const SDL_Event &event) {
  if (!isSelected) {
    return;
  }

  float uiX = 0.0f;
  float uiY = 0.0f;
  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return;
    }
    int x = 0;
    int y = 0;
    const int screenX =
        static_cast<int>(event.button.x * rendering::widthScale);
    const int screenY =
        static_cast<int>(event.button.y * rendering::heightScale);
    rendering::screenToUi(screenX, screenY, x, y);
    uiX = static_cast<float>(x);
    uiY = static_cast<float>(y);
    break;
  }
  case SDL_FINGERDOWN:
    fingerEventToUi(event.tfinger, uiX, uiY);
    break;
  default:
    return;
  }

  if (!isInsideTextInput(*this, uiX, uiY)) {
    finishEditing();
  }
}

void TextInputBox::finishEditing() {
  if (!isSelected) {
    return;
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  hideNativeTextEditor(false);
#endif
  commitComposition();
  notifyEditingFinished();
  onUnselected();
  SDL_StopTextInput();
}

void TextInputBox::notifyEditingFinished() {
  for (auto &callback : onEditingFinishedCallbacks) {
    callback(getText());
  }
}

size_t
TextInputBox::onTextChanged(std::function<void(const std::string &)> callback) {
  onTextChangedCallbacks.push_back(callback);
  return onTextChangedCallbacks.size() - 1;
}

void TextInputBox::removeOnTextChanged(
    std::function<void(const std::string &)> callback) {
  for (size_t i = 0; i < onTextChangedCallbacks.size(); i++) {
    if (onTextChangedCallbacks[i].target<void(const std::string &)>() ==
        callback.target<void(const std::string &)>()) {
      onTextChangedCallbacks.erase(onTextChangedCallbacks.begin() + i);
    }
  }
}

size_t
TextInputBox::onSubmit(std::function<void(const std::string &)> callback) {
  onSubmitCallbacks.push_back(callback);
  return onSubmitCallbacks.size() - 1;
}

void TextInputBox::removeOnSubmit(
    std::function<void(const std::string &)> callback) {
  for (size_t i = 0; i < onSubmitCallbacks.size(); i++) {
    if (onSubmitCallbacks[i].target<void(const std::string &)>() ==
        callback.target<void(const std::string &)>()) {
      onSubmitCallbacks.erase(onSubmitCallbacks.begin() + i);
    }
  }
}

size_t TextInputBox::onEditingFinished(
    std::function<void(const std::string &)> callback) {
  onEditingFinishedCallbacks.push_back(callback);
  return onEditingFinishedCallbacks.size() - 1;
}

void TextInputBox::removeOnEditingFinished(
    std::function<void(const std::string &)> callback) {
  for (size_t i = 0; i < onEditingFinishedCallbacks.size(); i++) {
    if (onEditingFinishedCallbacks[i].target<void(const std::string &)>() ==
        callback.target<void(const std::string &)>()) {
      onEditingFinishedCallbacks.erase(onEditingFinishedCallbacks.begin() + i);
    }
  }
}
