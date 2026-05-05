#include "TextInputBox.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "SDL2/SDL_events.h"
#include <cmath>
#include <cstring>

namespace {
SDL_Cursor *getCachedCursor(SDL_SystemCursor cursorType) {
  static SDL_Cursor *s_ibeamCursor = nullptr;
  static SDL_Cursor *s_arrowCursor = nullptr;
  SDL_Cursor **slot = cursorType == SDL_SYSTEM_CURSOR_IBEAM ? &s_ibeamCursor
                                                             : &s_arrowCursor;
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
} // namespace

TextInputBox::TextInputBox(const std::string &fontPath, int fontSize)
    : TextView(fontPath, fontSize) {
  viewRect = {getX(), getY(), getWidth(), getHeight()};
}

TextInputBox::~TextInputBox() {}

void TextInputBox::syncTextInputRect(int cursorX, int cursorY) {
  const int lineHeight =
      std::max(1, rect.h > 0 ? rect.h : (font != nullptr ? TTF_FontHeight(font) : 1));
  const int lineWidth = std::max(1, rect.w);
  SDL_Rect nextRect = {cursorX, cursorY, lineWidth, lineHeight};

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
    const float scaleX =
        static_cast<float>(logicalW) / static_cast<float>(rendering::window_width);
    const float scaleY = static_cast<float>(logicalH) /
                         static_cast<float>(rendering::window_height);
    nextRect.x = static_cast<int>(std::lround(static_cast<float>(cursorX) * scaleX));
    nextRect.y = static_cast<int>(std::lround(static_cast<float>(cursorY) * scaleY));
    nextRect.w =
        std::max(1, static_cast<int>(std::lround(lineWidth * scaleX)));
    nextRect.h =
        std::max(1, static_cast<int>(std::lround(lineHeight * scaleY)));
  }

  viewRect = nextRect;
  SDL_SetTextInputRect(&viewRect);
}

void TextInputBox::setEditingText(const std::string &newText) {
  editingText = newText;
  composition.clear();
  cursorPos = editingText.size();
  lastRenderedCaretCursor = -1;
  TextView::setText(editingText);
  int cursorX = 0;
  int cursorY = 0;
  cursorToPos(cursorPos, editingText, cursorX, cursorY);
  syncTextInputRect(cursorX, cursorY);
}

size_t TextInputBox::getNextUnicodePos(size_t pos) {
  if (pos >= editingText.size())
    return pos;
  auto cp = editingText.data() + pos;
  while (++cp < editingText.data() + editingText.size() && (*cp & 0b10000000) &&
         !(*cp & 0b01000000)) {
  }
  return cp - editingText.data();
}
size_t TextInputBox::getPrevUnicodePos(size_t pos) {
  if (pos == 0)
    return 0;
  auto cp = editingText.data() + pos;
  while (--cp >= editingText.data() &&
         ((*cp & 0b10000000) && !(*cp & 0b01000000))) {
  }
  return cp - editingText.data();
}
bool TextInputBox::handleEventsImpl(SDL_Event &event) {
  bool shouldUpdate = false;
  bool isSubmit = false;
  switch (event.type) {
  case SDL_TEXTINPUT:
    if (!isSelected)
      return true;
    composition.clear();
    shouldUpdate = true;
    // Add new text to the cursor position
    editingText.insert(cursorPos, event.text.text);
    cursorPos += strlen(event.text.text);
    break;
  case SDL_KEYDOWN:
    if (!isSelected)
      return true;
    shouldUpdate = true;
    if (event.key.keysym.sym == SDLK_BACKSPACE && editingText.length() > 0) {
      if (!composition.empty()) {
        break;
      }
      // cmd + backspace - delete whole word
      if (event.key.keysym.sym == SDLK_BACKSPACE &&
          (SDL_GetModState() & KMOD_CTRL)) {
        size_t prevPos = getPrevUnicodePos(cursorPos);
        while (prevPos > 0 && !std::isspace(editingText[prevPos - 1])) {
          prevPos = getPrevUnicodePos(prevPos);
        }
        editingText.erase(prevPos, cursorPos - prevPos);
        cursorPos = prevPos;
      } else {
        size_t prevPos = getPrevUnicodePos(cursorPos);
        editingText.erase(prevPos, cursorPos - prevPos);
        cursorPos = prevPos;
      }
    } else if (event.key.keysym.sym == SDLK_DELETE &&
               editingText.length() > 0) {
      if (!composition.empty()) {
        break;
      }
      size_t nextPos = getNextUnicodePos(cursorPos);
      editingText.erase(cursorPos, nextPos - cursorPos);
    } else if (event.key.keysym.sym == SDLK_RIGHT) {
      cursorPos = getNextUnicodePos(cursorPos);

    } else if (event.key.keysym.sym == SDLK_LEFT) {
      cursorPos = getPrevUnicodePos(cursorPos);

    } // paste
    else if (event.key.keysym.sym == SDLK_v &&
             (SDL_GetModState() & KMOD_CTRL) && SDL_HasClipboardText()) {
      std::string clipboard = SDL_GetClipboardText();
      editingText.insert(cursorPos, clipboard);
      cursorPos += clipboard.size();
    } else if (event.key.keysym.sym == SDLK_RETURN ||
               event.key.keysym.sym == SDLK_KP_ENTER) {
      isSubmit = true;
    }

    break;
  case SDL_TEXTEDITING:
    if (!isSelected)
      return true;
    // Update the composition text.
    composition = event.edit.text;
    shouldUpdate = true;
    break;
  case SDL_TEXTEDITING_EXT:

    if (!isSelected)
      return true;
    shouldUpdate = true;
    // Update the composition text.
    composition = event.editExt.text;
    break;
  case SDL_MOUSEBUTTONDOWN: {
    int screenX = static_cast<int>(event.button.x * rendering::widthScale);
    int screenY = static_cast<int>(event.button.y * rendering::heightScale);
    int x = 0;
    int y = 0;
    rendering::screenToUi(screenX, screenY, x, y);
    // check if the mouse is inside the text box
    if (event.button.button == SDL_BUTTON_LEFT &&
        isInsideTextInput(*this, static_cast<float>(x), static_cast<float>(y))) {

      cursorPos = posToCursor(x - getX(), y - getY());
      int cursorX = 0;
      int cursorY = 0;
      cursorToPos(cursorPos, editingText, cursorX, cursorY);
      syncTextInputRect(cursorX, cursorY);
      onSelected();
      SDL_StartTextInput();
      shouldUpdate = true;
      lastRenderedCaretCursor = -1;
      return false;
    } else {
      if (isSelected) {
        notifyEditingFinished();
      }
      onUnselected();
      SDL_StopTextInput();
    }

    break;
  }
  case SDL_MOUSEMOTION: {
    // change mouse pointer to I-beam
    int screenX = static_cast<int>(event.motion.x * rendering::widthScale);
    int screenY = static_cast<int>(event.motion.y * rendering::heightScale);
    int x = 0;
    int y = 0;
    rendering::screenToUi(screenX, screenY, x, y);
    if (x >= getX() && x <= getX() + getWidth() && y >= getY() &&
        y <= getY() + getHeight()) {
      updateHoverCursor(true);
    } else {
      updateHoverCursor(false);
    }
    break;
  }
  }
  if (shouldUpdate) {
    std::string composited = editingText;
    if (!composition.empty()) {
      composited.insert(cursorPos, composition);
      cursorToPos(cursorPos, editingText, compositionX, compositionY);
      cursorToPos(cursorPos + composition.size(), composited, compositionWidth,
                  compositionHeight);
      compositionWidth -= compositionX;
      const int underlineHeight = 2;
      const int contentHeight = std::max(
          underlineHeight,
          rect.h > 0 ? rect.h : (font != nullptr ? TTF_FontHeight(font) : 0));
      compositionHeight = contentHeight;
      compositionY += std::max(0, contentHeight - underlineHeight);
    }
    setText(composited);
    for (auto &callback : onTextChangedCallbacks) {
      callback(text);
    }
    if (isSubmit) {
      for (auto &callback : onSubmitCallbacks) {
        callback(text);
      }
      notifyEditingFinished();
    }
    int cursorX, cursorY;
    cursorToPos(cursorPos, editingText, cursorX, cursorY);
    syncTextInputRect(cursorX, cursorY);
  }
  return true;
}
void TextInputBox::onMove(int newX, int newY) {
  TextView::onMove(newX, newY);
  int cursorX, cursorY;
  cursorToPos(cursorPos, editingText, cursorX, cursorY);
  syncTextInputRect(cursorX, cursorY);
}
void TextInputBox::onResize(int newWidth, int newHeight) {
  TextView::onResize(newWidth, newHeight);
  int cursorX, cursorY;
  cursorToPos(cursorPos, editingText, cursorX, cursorY);
  syncTextInputRect(cursorX, cursorY);
}

void TextInputBox::renderImpl(RenderContext &context) {
  TextView::renderImpl(context);
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  if (isSelected) {
    // Caret blink interval
    Uint32 blinkInterval = 500;
    Uint32 currentTime = SDL_GetTicks();
    if (lastRenderedCaretCursor != cursorPos) {
      lastBlink = currentTime;
      lastRenderedCaretCursor = cursorPos;
    }

    // Determine whether to show the caret
    bool showCaret =
        (currentTime - lastBlink) % (2 * blinkInterval) < blinkInterval;

    if (showCaret) {
      bgfx::TransientVertexBuffer tvb;
      bgfx::TransientIndexBuffer tib;
      int caretX, caretY;
      cursorToPos(cursorPos, editingText, caretX, caretY);
      int height = rect.h;
      if (height == 0) {
        height = TTF_FontHeight(font);
      }

      if (!composition.empty()) {
        caretX = compositionX + compositionWidth;
        caretY = resolvedTextRect().y;
      }

      uint32_t xcolor;
      // sdl color to abgr
      SDL_Color &c = color;
      xcolor = ((c.r << 24) | (c.g << 16) | (c.b << 8) | c.a);
      rendering::createRect(tvb, tib, caretX, caretY, 2, height, xcolor);
      bgfx::setVertexBuffer(0, &tvb);
      bgfx::setIndexBuffer(&tib);
      rendering::setScissorUI(context.scissor.x, context.scissor.y,
                              context.scissor.width, context.scissor.height);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
      bgfx::submit(rendering::ui_view, kSimpleProgram);
    }
    // render blue underline for composition text
    if (!composition.empty()) {

      bgfx::TransientVertexBuffer tvb2;
      bgfx::TransientIndexBuffer tib2;
      rendering::createRect(tvb2, tib2, compositionX, compositionY,
                            compositionWidth, 2, 0xFFFFFFFF);
      // SDL_Log("Draw Composition x: %d, y: %d, w: %d, h: %d", compositionX,
      //         compositionY - 20, compositionWidth, 200);
      bgfx::setVertexBuffer(0, &tvb2);
      bgfx::setIndexBuffer(&tib2);
      rendering::setScissorUI(context.scissor.x, context.scissor.y,
                              context.scissor.width, context.scissor.height);
      bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
      bgfx::submit(rendering::ui_view, kSimpleProgram);
    }
  }
}

void TextInputBox::cursorToPos(size_t cursorPos, const std::string &text,
                               int &x, int &y) {
  // TODO: this will not work for multi-line text
  std::string utf8 = text;
  if (cursorPos > utf8.size())
    cursorPos = utf8.size();
  if (cursorPos < 0)
    cursorPos = 0;
  utf8.resize(cursorPos);

  int textWidth = 0;
  int textHeight = 0;
  if (font != nullptr && !utf8.empty()) {
    TTF_SizeUTF8(font, utf8.c_str(), &textWidth, &textHeight);
  }

  const SDL_Rect contentRect = resolvedTextRect();
  x = contentRect.x + textWidth;
  y = contentRect.y;
}

size_t TextInputBox::posToCursor(int x, int y) {
  // TODO: this will not work for multi-line text
  (void)y;
  size_t cursorPos = 0;
  int w = 0, h = 0;
  int dw = 0;
  int dh = 0;
  size_t glyphs = 0;
  const SDL_Rect contentRect = resolvedTextRect();
  const int localX = std::max(0, x - (contentRect.x - getX()));

  for (cursorPos = 0; cursorPos < editingText.size();) {

    int prevW = w;
    int prevH = h;
    TTF_SizeUTF8(font, editingText.substr(0, cursorPos).c_str(), &w, &h);
    if (prevW != 0)
      dw = w - prevW;
    else
      dw = w;
    if (prevH != 0)
      dh = h - prevH;
    else
      dh = h;
    if (glyphs == 1 && dw / 2 > localX)
      return 0;
    if (w + dw / 2 > localX) {

      break;
    }
    glyphs++;
    cursorPos = getNextUnicodePos(cursorPos);
  }
  return cursorPos;
}

void TextInputBox::onSelected() { isSelected = true; }

void TextInputBox::onUnselected() { isSelected = false; }

void TextInputBox::notifyEditingFinished() {
  for (auto &callback : onEditingFinishedCallbacks) {
    callback(editingText);
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
