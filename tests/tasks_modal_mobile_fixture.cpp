// Exercise the local task panel on mobile with real views.
#include "targets.h"
#undef TARGET_OS_ANDROID
#define TARGET_OS_ANDROID 1
#include "../src/scene/TasksModal.cpp"
#include <cassert>
#include <random>
#include <fstream>
#include <chrono>
#include <thread>

namespace {
Button *findAction(View *view, const std::string &text) {
  if (!view) return nullptr;
  if (auto *button = dynamic_cast<Button *>(view)) {
    if (auto *label = dynamic_cast<TextView *>(button->getContentView()); label && label->getText() == text)
      return button;
  }
  if (auto *scroll = dynamic_cast<ScrollView *>(view))
    if (auto *found = findAction(scroll->getContentView(), text)) return found;
  for (auto *child : view->getChildren())
    if (auto *found = findAction(child, text)) return found;
  return nullptr;
}
void click(TasksModal &modal, Button *button) {
  assert(button);
  modal.root()->applyYogaLayout();
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.button = SDL_BUTTON_LEFT;
  event.button.x = button->getX() + button->getWidth() / 2;
  event.button.y = button->getY() + button->getHeight() / 2;
  button->handleEvents(event);
  event.type = SDL_EVENT_MOUSE_BUTTON_UP;
  button->handleEvents(event);
}
}

void testTasksModalTabs() {
  View parent;
  int refreshes = 0;
  TasksModal modal(&parent, [&] { ++refreshes; });
  modal.setLibraryCount(3);
  modal.libraryText()->setText("Library task progress");
  assert(modal.notificationCount() == 3);
  assert(!modal.root()->findViewByName("tasksDownloadsContent"));
  modal.show();
  assert(modal.isVisible());
  for (const auto [width, height] : {std::pair{360, 640}, std::pair{844, 390}}) {
    modal.resize(width, height); modal.root()->applyYogaLayout();
    auto *library = modal.libraryScroll();
    assert(library->getWidth() > 0 && library->getHeight() > 0);
    assert(library->getX() >= 0 && library->getX() + library->getWidth() <= width);
    assert(library->getY() >= 0 && library->getY() + library->getHeight() <= height);
  }
  assert(modal.libraryText()->getText() == "Library task progress");
  click(modal, findAction(modal.root(), i18n::tr("menu.refresh_list.label")));
  assert(refreshes == 1 && !modal.isVisible());
  modal.show();
  click(modal, findAction(modal.root(), i18n::tr("menu.tasks.close.label")));
  assert(!modal.isVisible());
  modal.show();
  SDL_Event escape{}; escape.type = SDL_EVENT_KEY_DOWN; escape.key.key = SDLK_ESCAPE;
  assert(!modal.handleEvents(escape) && !modal.isVisible());
  assert(modal.handleEvents(escape));
  modal.setLibraryCount(-1);
  assert(modal.notificationCount() == 0);
}
