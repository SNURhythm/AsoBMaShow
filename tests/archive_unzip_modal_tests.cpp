#include "scene/ArchiveUnzipModal.h"
#include "ArchiveRAII.h"
#include "rendering/common.h"
#include "rendering/UniformCache.h"
#include "view/Button.h"
#include "view/TextView.h"
#include "sqlite3.h"

#include <archive_entry.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
}

namespace {

struct DeleteTraceGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, released = false, timedOut = false;
};

DeleteTraceGate *deleteTraceGate = nullptr;

int traceArchiveDelete(unsigned, void *context, void *statement, void *) {
  const auto *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
  if (!sql || !std::string_view(sql).starts_with("DELETE FROM solid_archives")) return 0;
  auto &gate = *static_cast<DeleteTraceGate *>(context);
  std::unique_lock lock(gate.mutex);
  gate.entered = true;
  gate.condition.notify_all();
  gate.timedOut = !gate.condition.wait_for(lock, std::chrono::seconds(3), [&] { return gate.released; });
  return 0;
}

int installArchiveDeleteTrace(sqlite3 *database, char **, const sqlite3_api_routines *) {
  return sqlite3_trace_v2(database, SQLITE_TRACE_STMT, traceArchiveDelete, deleteTraceGate);
}

Button *findButton(View *root, const std::string &label) {
  if (auto *button = dynamic_cast<Button *>(root); button && button->getVisible()) {
    const auto *text = dynamic_cast<TextView *>(button->getContentView());
    if (text && text->getText() == label) {
      return button;
    }
  }
  for (auto *child : root->getChildren()) {
    if (auto *button = findButton(child, label)) {
      return button;
    }
  }
  return nullptr;
}

void click(ArchiveUnzipModal &modal, const std::string &label) {
  auto *button = findButton(modal.root(), label);
  assert(button);
  SDL_Event event{};
  event.type = SDL_MOUSEBUTTONDOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = button->getX() + button->getWidth() / 2;
  event.button.y = button->getY() + button->getHeight() / 2;
  modal.handleEvents(event);
  event.type = SDL_MOUSEBUTTONUP;
  modal.handleEvents(event);
}

void assertDescriptionsFit(ArchiveUnzipModal &modal) {
  auto *panel = modal.root()->getChildren().front();
  int descriptions = 0;
  for (auto *child : panel->getChildren()) {
    const auto *text = dynamic_cast<TextView *>(child);
    if (!text || (text->pointSize() != 22 && text->pointSize() != 18)) {
      continue;
    }
    ++descriptions;
    assert(text->textureWidth() > 0 && text->textureHeight() > 0);
    assert(text->textureWidth() <= text->getContentWidth());
    assert(text->textureHeight() <= text->getContentHeight());
    assert(text->getContentX() >= panel->getContentX());
    assert(text->getContentX() + text->textureWidth() <=
           panel->getContentX() + panel->getContentWidth());
    assert(text->getContentY() + text->textureHeight() <=
           panel->getContentY() + panel->getContentHeight());
  }
  assert(descriptions == 2);
}

void preflightRequiresExplicitChoiceAndDispatchesCallbacksOnlyOnUpdate(bool deleteAfter) {
  const auto root = std::filesystem::temp_directory_path() /
      ("archive-unzip-modal-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  {
    ChartRepository repository(root / "library.db");
    auto session = repository.OpenSession();
    assert(session && session->EnsureSchema());
    View parent(0, 0, rendering::window_width, rendering::window_height);
    int changed = 0;
    int finished = 0;
    const auto uiThread = std::this_thread::get_id();
    auto modal = ArchiveUnzipModal::Create(&parent, repository, {
        .libraryChanged = [&]() {
          assert(std::this_thread::get_id() == uiThread);
          ++changed;
        },
        .finished = [&](const ArchiveUnzipResult &result) {
          assert(std::this_thread::get_id() == uiThread);
          assert(result.success && result.chartPath.empty());
          ++finished;
        },
    });
    assert(modal->startAll());
    assert(modal->isVisible());
    assertDescriptionsFit(*modal);
    assert(!modal->startAll());
    assert(findButton(modal->root(), "Keep Archives"));
    assert(findButton(modal->root(), "Delete After Unzip"));
    assert(findButton(modal->root(), "Cancel"));
    for (int frame = 0; frame < 5; ++frame) {
      modal->update();
    }
    assert(changed == 0 && finished == 0);
    click(*modal, "Cancel");
    assert(!modal->isVisible() && !modal->inProgress());
    assert(modal->startAll());
    modal->cancelAndWait();
    assert(!modal->isVisible() && !modal->inProgress());
    assert(modal->startAll());
    SDL_Event escape{};
    escape.type = SDL_KEYDOWN;
    escape.key.keysym.sym = SDLK_ESCAPE;
    assert(!modal->handleEvents(escape));
    assert(!modal->isVisible() && !modal->inProgress());
    assert(modal->startAll());
    SDL_Event background{};
    background.type = SDL_APP_WILLENTERBACKGROUND;
    modal->handleEvents(background);
    assert(!modal->isVisible() && !modal->inProgress());
    assert(modal->startAll());

    const auto archivePath = root / "after-preflight.zip";
    auto writer = makeArchiveWriteHandle();
    assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK);
    const std::string contents = "#TITLE Modal\n#BPM 120\n#00111:01\n";
    auto entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), "chart.bms");
    archive_entry_set_size(entry.get(), contents.size());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    assert(archive_write_close(writer.get()) == ARCHIVE_OK);
    const auto secondArchivePath = root / "second.zip";
    std::filesystem::copy_file(archivePath, secondArchivePath);
    auto batch = session->BeginScanBatch();
    assert(batch && batch->UpsertSolidArchive({.path = archivePath}));
    assert(batch->UpsertSolidArchive({.path = secondArchivePath}));
    assert(batch->Commit());
    batch.reset();
    modal->update();
    assert(session->CountAllChartMeta() == 0);
    assert(changed == 0 && finished == 0);
    ChartMetaRecord record;
    record.solidArchive = true;
    record.meta.BmsPath = archivePath;
    assert(!modal->start(record));
    assert(session->CountAllChartMeta() == 0);
    click(*modal, deleteAfter ? "Delete After Unzip" : "Keep Archives");
    for (auto *child : modal->root()->getChildren().front()->getChildren()) {
      if (const auto *text = dynamic_cast<TextView *>(child); text && text->pointSize() == 18) {
        assert(text->getText().find("sequentially") == std::string::npos);
        assert(text->getText().find("concurrently") != std::string::npos);
      }
    }
    assert(modal->inProgress());
    assert(!modal->startAll());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (session->CountAllChartMeta() < 2 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(session->CountAllChartMeta() == 2);
    assert(changed == 0 && finished == 0);
    while (modal->inProgress() && std::chrono::steady_clock::now() < deadline) {
      modal->update();
      bgfx::frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!modal->inProgress());
    assert(changed == 1 && finished == 1);
    assertDescriptionsFit(*modal);
    assert(std::filesystem::exists(archivePath) == !deleteAfter);
    assert(std::filesystem::exists(secondArchivePath) == !deleteAfter);
    assert(findButton(modal->root(), "Close"));
    assert(!findButton(modal->root(), "Delete Archive"));
    modal->update();
    assert(changed == 1 && finished == 1);
    click(*modal, "Close");
    assert(!modal->isVisible());
  }
  std::filesystem::remove_all(root);
}

void emptyBatchDoesNotNotifyLibraryChange() {
  const auto root = std::filesystem::temp_directory_path() /
      ("archive-unzip-modal-empty-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  {
    ChartRepository repository(root / "library.db");
    View parent(0, 0, rendering::window_width, rendering::window_height);
    int changed = 0;
    bool finished = false;
    auto modal = ArchiveUnzipModal::Create(&parent, repository, {
        .libraryChanged = [&]() { ++changed; },
        .finished = [&](const ArchiveUnzipResult &result) {
          assert(result.batch && result.success && result.archiveCount == 0);
          finished = true;
        },
    });
    assert(modal->startAll());
    click(*modal, "Keep Archives");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
      modal->update();
      bgfx::frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(finished);
    assert(changed == 0);
  }
  std::filesystem::remove_all(root);
}

void singleDeleteDoesNotBlockInputWhileLibraryIsBusy() {
  const auto root = std::filesystem::temp_directory_path() /
      ("archive-unzip-modal-delete-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  {
    ChartRepository repository(root / "library.db");
    auto session = repository.OpenSession();
    assert(session && session->EnsureSchema());
    const auto archivePath = root / "single.zip";
    auto writer = makeArchiveWriteHandle();
    assert(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK);
    const std::string contents = "#TITLE Delete\n#BPM 120\n#00111:01\n";
    auto entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), "chart.bms");
    archive_entry_set_size(entry.get(), contents.size());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    assert(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    assert(archive_write_data(writer.get(), contents.data(), contents.size()) ==
           static_cast<la_ssize_t>(contents.size()));
    assert(archive_write_close(writer.get()) == ARCHIVE_OK);
    auto batch = session->BeginScanBatch();
    assert(batch && batch->UpsertSolidArchive({.path = archivePath}));
    assert(batch->Commit());
    batch.reset();
    View parent(0, 0, rendering::window_width, rendering::window_height);
    const auto uiThread = std::this_thread::get_id();
    int changed = 0, finished = 0;
    auto modal = ArchiveUnzipModal::Create(&parent, repository, {
        .libraryChanged = [&] {
          assert(std::this_thread::get_id() == uiThread);
          ++changed;
        },
        .finished = [&](const ArchiveUnzipResult &result) {
          assert(std::this_thread::get_id() == uiThread);
          assert(result.success);
          ++finished;
        },
    });
    ChartMetaRecord record;
    record.solidArchive = true;
    record.meta.BmsPath = archivePath;
    assert(modal->start(record));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (modal->inProgress() && std::chrono::steady_clock::now() < deadline) {
      modal->update();
      bgfx::frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!modal->inProgress() && changed == 1 && finished == 1);
    assert(findButton(modal->root(), "Delete Archive"));

    DeleteTraceGate gate;
    deleteTraceGate = &gate;
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)(void)>(installArchiveDeleteTrace)) == SQLITE_OK);
    const auto clickedAt = std::chrono::steady_clock::now();
    click(*modal, "Delete Archive");
    const auto clickDuration = std::chrono::steady_clock::now() - clickedAt;
    std::cout << "Delete Archive click: " <<
        std::chrono::duration_cast<std::chrono::milliseconds>(clickDuration).count() << " ms\n";
    {
      std::unique_lock lock(gate.mutex);
      assert(gate.condition.wait_for(lock, std::chrono::seconds(3), [&] { return gate.entered; }));
      assert(!gate.timedOut);
    }
    assert(modal->inProgress());
    assert(!findButton(modal->root(), "Delete Archive"));
    assert(findButton(modal->root(), "Deleting..."));
    assert(!modal->start(record) && !modal->startAll());
    click(*modal, "Deleting...");
    SDL_Event escape{};
    escape.type = SDL_KEYDOWN;
    escape.key.keysym.sym = SDLK_ESCAPE;
    assert(!modal->handleEvents(escape));
    modal->hide();
    assert(modal->inProgress() && modal->isVisible());
    assert(changed == 1 && finished == 1);
    for (int frame = 0; frame < 3; ++frame) {
      modal->update();
      bgfx::frame();
    }
    assert(changed == 1 && finished == 1);
    {
      std::lock_guard lock(gate.mutex);
      gate.released = true;
    }
    gate.condition.notify_all();
    while (modal->inProgress() && std::chrono::steady_clock::now() < deadline) {
      modal->update();
      bgfx::frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!modal->inProgress());
    assert(!std::filesystem::exists(archivePath));
    assert(session->CountSolidArchives() == 0 && session->CountAllChartMeta() == 1);
    assert(changed == 2 && finished == 1);
    assert(sqlite3_cancel_auto_extension(reinterpret_cast<void (*)(void)>(installArchiveDeleteTrace)) == 1);
    deleteTraceGate = nullptr;
    assert(findButton(modal->root(), "Close"));
    modal->update();
    assert(changed == 2 && finished == 1);
    click(*modal, "Close");
    assert(!modal->isVisible());
  }
  std::filesystem::remove_all(root);
}

}

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));
  preflightRequiresExplicitChoiceAndDispatchesCallbacksOnlyOnUpdate(false);
  preflightRequiresExplicitChoiceAndDispatchesCallbacksOnlyOnUpdate(true);
  emptyBatchDoesNotNotifyLibraryChange();
  singleDeleteDoesNotBlockInputWhileLibraryIsBusy();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  std::cout << "archive_unzip_modal_tests passed\n";
}
