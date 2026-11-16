#include "scene/VirtualControllerEditorView.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = 1000, window_height = 600;
int render_width = 1000, render_height = 600;
float widthScale = 1, heightScale = 1;
float ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 0, ui_offset_y = 0;
int ui_view_width = 1000, ui_view_height = 600;
}

namespace {

void sendFinger(VirtualControllerEditorView &editor, Uint32 type, float x, float y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.fingerID = 1;
  event.tfinger.x = x / 1000;
  event.tfinger.y = y / 600;
  editor.handleEvents(event);
}

const gameplay::VirtualControllerElement &findControl(
    const gameplay::VirtualControllerLayout &layout, int index) {
  const auto found = std::ranges::find_if(layout.elements,
      [index](const auto &element) { return element.configIndex() == index; });
  assert(found != layout.elements.end());
  return *found;
}

void testSelectionChangeCancelsCapturedGesture() {
  input::VirtualControllerConfig config;
  config.enabled = true;
  config.centerY = 0.5F;
  const auto layout = gameplay::makeVirtualControllerLayout(config, 7, {28, 28, 944, 544});
  const auto &first = findControl(layout, 0).bounds;
  const auto &second = findControl(layout, 1).bounds;
  for (const bool resize : {false, true}) {
    int saves = 0;
    auto saved = config;
    VirtualControllerEditorView editor(config, 7, [&](auto next) { saved = next; ++saves; },
        VirtualControllerEditorView::EditMode::Controls, 0);
    editor.setSize(1000, 600);
    const float x = resize ? first.x + first.width + 22 : first.centerX();
    const float y = resize ? first.y + first.height + 22 : first.centerY();
    sendFinger(editor, SDL_EVENT_FINGER_DOWN, x, y);
    sendFinger(editor, SDL_EVENT_FINGER_MOTION, x + 20, y + 10);
    // A second finger activates a selection arrow while the first remains down.
    editor.selectControl(1);
    sendFinger(editor, SDL_EVENT_FINGER_MOTION, x + 40, y + 20);
    sendFinger(editor, SDL_EVENT_FINGER_UP, x + 40, y + 20);
    assert(saves == 0);
    sendFinger(editor, SDL_EVENT_FINGER_DOWN, second.centerX(), second.centerY());
    sendFinger(editor, SDL_EVENT_FINGER_UP, second.centerX() + 10, second.centerY());
    assert(saves == 1);
    assert(saved.controls[0] == config.controls[0]);
    assert(saved.controls[1].offsetX > 0);
  }
}

void testCancellationPreservesClampedConfiguration() {
  input::VirtualControllerConfig config;
  config.enabled = true;
  config.centerY = 0.5F;
  config.controls[6].offsetX = 10;
  config.controls[8] = {.sizeScale = 3, .offsetX = -10, .offsetY = 10};
  const auto layout = gameplay::makeVirtualControllerLayout(config, 7, {28, 28, 944, 544});
  for (const int index : {6, 8}) {
    int saves = 0;
    auto saved = config;
    VirtualControllerEditorView editor(config, 7, [&](auto next) { saved = next; ++saves; },
        VirtualControllerEditorView::EditMode::Controls, index);
    editor.setSize(1000, 600);
    const auto &bounds = findControl(layout, index).bounds;
    const float x = index == 8 ? std::min(972.0F, bounds.x + bounds.width + 22) : bounds.centerX();
    const float y = index == 8 ? std::min(572.0F, bounds.y + bounds.height + 22) : bounds.centerY();
    sendFinger(editor, SDL_EVENT_FINGER_DOWN, x, y);
    sendFinger(editor, SDL_EVENT_FINGER_MOTION, x - 20, y - 20);
    sendFinger(editor, SDL_EVENT_FINGER_CANCELED, x - 20, y - 20);
    assert(saves == 0);
    // A later unrelated save must not persist the cancelled control's normalized baseline.
    editor.selectControl(0);
    const auto &other = findControl(layout, 0).bounds;
    sendFinger(editor, SDL_EVENT_FINGER_DOWN, other.centerX(), other.centerY());
    sendFinger(editor, SDL_EVENT_FINGER_UP, other.centerX() + 10, other.centerY());
    assert(saves == 1);
    assert(saved.controls[index] == config.controls[index]);
    assert(saved.controls[0].offsetX > 0);
  }
}

} // namespace

int main() {
  testSelectionChangeCancelsCapturedGesture();
  testCancellationPreservesClampedConfiguration();
  using Mode = VirtualControllerEditorView::EditMode;
  input::VirtualControllerConfig config;
  config.enabled = true;
  config.centerY = 0.5F;
  int saves = 0;
  int selected = -1;
  auto saved = config;
  VirtualControllerEditorView editor(config, 7, [&](auto next) { saved = next; ++saves; },
                                     Mode::Controls, 0, [&](int index) { selected = index; });
  editor.setSize(1000, 600);
  const auto layout = gameplay::makeVirtualControllerLayout(config, 7, {28, 28, 944, 544});
  const auto key = std::ranges::find_if(layout.elements, [](const auto &e) { return e.keyPosition == 0; });
  assert(key != layout.elements.end());
  const float x = key->bounds.centerX(), y = key->bounds.centerY();
  const auto finger = [&](Uint32 type, float px, float py, SDL_FingerID id = 1) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.fingerID = id;
    event.tfinger.x = px / 1000;
    event.tfinger.y = py / 600;
    return editor.handleEvents(event);
  };
  assert(!finger(SDL_EVENT_FINGER_DOWN, x, y));
  assert(selected == 0);
  assert(!finger(SDL_EVENT_FINGER_MOTION, x + 20, y + 10));
  assert(saves == 0); // No disk writes during a gesture.
  finger(SDL_EVENT_FINGER_DOWN, x, y, 2);
  finger(SDL_EVENT_FINGER_UP, x, y, 2);
  assert(saves == 0); // A second contact cannot finish the active gesture.
  assert(!finger(SDL_EVENT_FINGER_UP, x + 20, y + 10));
  assert(saves == 1);
  assert(std::abs(saved.controls[0].offsetX - 20 / layout.unit) < 0.0001F);
  assert(std::abs(saved.controls[0].offsetY - 10 / layout.unit) < 0.0001F);
  assert(saved.controls[1] == config.controls[1]);

  // Native cancellation rolls back in-progress edits and never persists them.
  finger(SDL_EVENT_FINGER_DOWN, x + 20, y + 10);
  finger(SDL_EVENT_FINGER_MOTION, x + 60, y + 40);
  finger(SDL_EVENT_FINGER_CANCELED, x + 60, y + 40);
  assert(saves == 1);
  finger(SDL_EVENT_FINGER_DOWN, x + 20, y + 10);
  finger(SDL_EVENT_FINGER_UP, x + 20, y + 10);
  assert(saves == 2);
  assert(std::abs(saved.controls[0].offsetX - 20 / layout.unit) < 0.0001F);

  // A toolbar can consume release above the canvas; the editor must not stay captured.
  finger(SDL_EVENT_FINGER_DOWN, x + 20, y + 10);
  finger(SDL_EVENT_FINGER_MOTION, x + 50, y + 40);
  SDL_Event coveredRelease{};
  coveredRelease.type = SDL_EVENT_FINGER_UP;
  coveredRelease.tfinger.fingerID = 1;
  editor.notifyPointerEventConsumed(coveredRelease);
  assert(saves == 2);

  const auto moved = gameplay::makeVirtualControllerLayout(saved, 7, {28, 28, 944, 544});
  const auto movedKey = std::ranges::find_if(moved.elements, [](const auto &e) { return e.keyPosition == 0; });
  const float hx = movedKey->bounds.x + movedKey->bounds.width + 22;
  const float hy = movedKey->bounds.y + movedKey->bounds.height + 22;
  finger(SDL_EVENT_FINGER_DOWN, hx, hy);
  finger(SDL_EVENT_FINGER_MOTION, hx + 15, hy + 30);
  finger(SDL_EVENT_FINGER_UP, hx + 15, hy + 30);
  assert(saves == 3 && saved.controls[0].sizeScale > 1);
  assert(saved.controls[1] == config.controls[1]);

  const auto mouseDrag = [](VirtualControllerEditorView &view, float x, float y, float dx) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x; event.button.y = y;
    assert(!view.handleEvents(event));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.x += dx;
    assert(!view.handleEvents(event));
  };
  auto edgeConfig = config;
  edgeConfig.controls[6].offsetX = 10;
  VirtualControllerEditorView edgeEditor(edgeConfig, 7, [&](auto next) { saved = next; }, Mode::Controls, 6);
  edgeEditor.setSize(1000, 600);
  const auto edgeLayout = gameplay::makeVirtualControllerLayout(edgeConfig, 7, {28, 28, 944, 544});
  const auto edgeKey = std::ranges::find_if(edgeLayout.elements, [](const auto &e) { return e.keyPosition == 6; });
  mouseDrag(edgeEditor, edgeKey->bounds.centerX(), edgeKey->bounds.centerY(), -20);
  const auto movedEdge = gameplay::makeVirtualControllerLayout(saved, 7, {28, 28, 944, 544});
  const auto movedEdgeKey = std::ranges::find_if(movedEdge.elements, [](const auto &e) { return e.keyPosition == 6; });
  assert(std::abs(movedEdgeKey->bounds.centerX() - edgeKey->bounds.centerX() + 20) < 0.001F);

  auto overlap = config;
  overlap.controls[2].offsetX = -2 * (1 + config.keySpacingX);
  VirtualControllerEditorView overlapEditor(overlap, 7, [&](auto next) { saved = next; }, Mode::Controls, 2);
  overlapEditor.setSize(1000, 600);
  mouseDrag(overlapEditor, x, y, 20);
  assert(saved.controls[0] == config.controls[0]);
  assert(saved.controls[2].offsetX > overlap.controls[2].offsetX);

  VirtualControllerEditorView compensation(config, 7, [&](auto next) { saved = next; }, Mode::Compensation);
  compensation.setSize(1000, 600);
  SDL_Event mouse{};
  mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  mouse.button.button = SDL_BUTTON_LEFT;
  mouse.button.x = 500; mouse.button.y = 300;
  assert(!compensation.handleEvents(mouse));
  mouse.type = SDL_EVENT_MOUSE_BUTTON_UP;
  mouse.button.x = 736;
  assert(!compensation.handleEvents(mouse));
  assert(std::abs(saved.touchCompensation - 1.0F) < 0.0001F);
}
