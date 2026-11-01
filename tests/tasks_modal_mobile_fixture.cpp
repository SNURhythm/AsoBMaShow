// Exercise the mobile tab path with real views and a temporary download store.
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
std::shared_ptr<downloads::DownloadCoordinator> testDownloads;
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
namespace downloads {
std::shared_ptr<DownloadCoordinator> service() { return testDownloads; }
}
namespace platform_open {
bool openExternalUrl(std::string_view, std::string &) { return false; }
}

void testTasksModalTabs() {
  const auto path = std::filesystem::temp_directory_path() /
      ("asobms-task-tabs-" + std::to_string(std::random_device{}()));
  testDownloads = std::make_shared<downloads::DownloadCoordinator>(path, downloads::CoordinatorHooks{});
  downloads::Request request;
  request.url = "https://example.com/chart.zip";
  request.archiveName = "chart.zip";
  request.libraryRoot = path / "library";
  const auto first = testDownloads->submit(request);
  const auto second = testDownloads->submit(request);
  {
    View parent;
    int refreshes = 0;
    TasksModal modal(&parent, [&] { ++refreshes; }, [](const auto &) { return false; });
    modal.setLibraryCount(3);
    modal.libraryText()->setText("Library task progress");
    assert(modal.notificationCount() == 5);
    auto *library = modal.root()->findViewByName("tasksLibraryContent");
    auto *downloads = modal.root()->findViewByName("tasksDownloadsContent");
    auto *libraryTab = dynamic_cast<Button *>(modal.root()->findViewByName("tasksLibraryTab"));
    auto *downloadsTab = dynamic_cast<Button *>(modal.root()->findViewByName("tasksDownloadsTab"));
    assert(library && downloads && libraryTab && downloadsTab);
    modal.show(TasksModal::Tab::Downloads);
    assert(modal.isVisible() && !library->getVisible() && downloads->getVisible());
    assert(findAction(modal.root(), i18n::tr("library.find_bms.cancel.label")));
    click(modal, findAction(modal.root(), i18n::tr("library.find_bms.cancel.label")));
    assert(testDownloads->get(second.id)->state == downloads::State::Cancelled);
    modal.show(TasksModal::Tab::Downloads);
    assert(modal.notificationCount() == 4);
    click(modal, libraryTab);
    assert(library->getVisible() && !downloads->getVisible());
    assert(modal.libraryText()->getText() == "Library task progress");
    click(modal, downloadsTab);
    assert(!library->getVisible() && downloads->getVisible());
    for (const auto [width, height] : {std::pair{360, 640}, std::pair{844, 390}}) {
      modal.resize(width, height); modal.root()->applyYogaLayout();
      assert(libraryTab->getWidth() > 0 && downloadsTab->getWidth() > 0);
      assert(downloadsTab->getX() >= libraryTab->getX() + libraryTab->getWidth());
      assert(downloadsTab->getX() + downloadsTab->getWidth() <= width);
      assert(downloads->getHeight() > 0);
      for (auto *tab : {libraryTab, downloadsTab}) {
        const auto *label = tab->getContentView()->getChildren().front();
        assert(std::abs(label->getX() + label->getWidth() / 2 -
                        (tab->getX() + tab->getWidth() / 2)) < 1.0f);
      }
    }
    click(modal, libraryTab);
    click(modal, findAction(modal.root(), i18n::tr("menu.refresh_list.label")));
    assert(refreshes == 1 && !modal.isVisible());
    modal.show(TasksModal::Tab::Downloads);
    SDL_Event escape{}; escape.type = SDL_EVENT_KEY_DOWN; escape.key.key = SDLK_ESCAPE;
    assert(!modal.handleEvents(escape) && !modal.isVisible());
    assert(modal.handleEvents(escape));
  }
  testDownloads.reset();
  std::filesystem::remove_all(path);

  // Both failed transition writes and failed prepared-completion writes must
  // expose a usable Retry action without another network transfer.
  for (const bool prepared : {false, true}) {
    int transfers = 0;
    const auto directory = path / (prepared ? "completion" : "transition");
    const auto manifest = directory / "jobs.json", saved = directory / "saved.json";
    const auto blockWrites = [&] {
      std::filesystem::rename(manifest, saved);
      std::filesystem::create_directory(manifest);
    };
    downloads::CoordinatorHooks hooks;
    hooks.startTransfer = [&](const auto &, const auto &) { ++transfers; return std::string{}; };
    hooks.importArchive = [&](const auto &, const auto &, auto &, auto, auto prepare) {
      if (prepared) {
        assert(prepare(BmsSearchPendingArtifact{}));
        blockWrites();
      }
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = directory / "installed"};
    };
    hooks.resolveArtifact = [&](const auto &, auto, auto &, auto) {
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = directory / "installed"};
    };
    testDownloads = std::make_shared<downloads::DownloadCoordinator>(directory, hooks);
    testDownloads->setForeground(true);
    const auto job = testDownloads->submit(request);
    const auto file = directory / "completed";
    std::ofstream(file) << "archive";
    assert(testDownloads->acceptFile(job.id, job.attempt, file));
    if (!prepared) blockWrites();
    testDownloads->enableImports(true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (testDownloads->error().empty() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(!testDownloads->error().empty());
    assert(testDownloads->get(job.id)->state == (prepared ? downloads::State::Importing : downloads::State::Downloaded));
    {
      View parent;
      TasksModal modal(&parent, {}, {});
      modal.show(TasksModal::Tab::Downloads);
      auto *retry = findAction(modal.root(), i18n::tr("library.find_bms.retry.label"));
      assert(retry && "persistently blocked imports need Retry even before restart");
      std::filesystem::remove(manifest);
      std::filesystem::rename(saved, manifest);
      click(modal, retry);
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (testDownloads->get(job.id)->state != downloads::State::Complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      assert(testDownloads->get(job.id)->state == downloads::State::Complete);
      assert(testDownloads->error().empty() && transfers == 1);
    }
    testDownloads.reset();
  }
  std::filesystem::remove_all(path);
}
