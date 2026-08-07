#pragma once

#include "../input/VirtualControllerConfig.h"
#include "../scene/play/VirtualControllerLayout.h"
#include "../view/View.h"

#include <functional>

// A compact direct-manipulation preview used by Settings > Input. It retains
// edits locally during a drag and commits only when the pointer is lifted.
class VirtualControllerEditorView final : public View {
public:
  using CommitCallback = std::function<void(input::VirtualControllerConfig)>;

  VirtualControllerEditorView(input::VirtualControllerConfig config,
                              CommitCallback onCommit);

protected:
  void renderImpl(RenderContext &context) override;
  bool handleEventsImpl(SDL_Event &event) override;

private:
  enum class DragMode : unsigned char { None, Move, Resize, Spacing };

  [[nodiscard]] gameplay::VirtualControllerCanvas canvas() const noexcept;
  [[nodiscard]] gameplay::VirtualControllerLayout layout() const;
  [[nodiscard]] DragMode hitDragMode(float uiX, float uiY) const;
  [[nodiscard]] bool beginDrag(std::int64_t pointerId, float uiX,
                               float uiY) noexcept;
  bool updateDrag(float uiX, float uiY) noexcept;
  void endDrag(std::int64_t pointerId);
  static bool contains(const gameplay::VirtualControllerRect &rect, float x,
                       float y) noexcept;
  static gameplay::VirtualControllerRect handleRect(float centerX,
                                                     float centerY) noexcept;
  static bool mousePoint(const SDL_MouseButtonEvent &event, float &x,
                         float &y) noexcept;
  static bool mousePoint(const SDL_MouseMotionEvent &event, float &x,
                         float &y) noexcept;
  static void fingerPoint(const SDL_TouchFingerEvent &event, float &x,
                          float &y) noexcept;

  input::VirtualControllerConfig config_;
  CommitCallback onCommit_;
  DragMode dragMode_ = DragMode::None;
  std::int64_t activePointerId_ = -1;
  input::VirtualControllerConfig dragStartConfig_;
  float dragStartX_ = 0.0F;
  float dragStartY_ = 0.0F;
};
