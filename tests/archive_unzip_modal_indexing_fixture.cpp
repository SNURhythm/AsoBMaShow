#include <cassert>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct Progress {
  double fraction = 0.95;
  std::string message = "Indexing extracted charts";
  std::uint64_t current = 1, total = 2;
  bool indexing = true;
  std::vector<std::string> activeArchives;
};
struct Result {
  bool success = false, cancelled = true, batch = true;
  std::size_t archiveCount = 2, completedCount = 1;
  std::string message = "Unzip All cancelled. Library indexed.";
};
struct Operation {
  std::optional<Progress> progress;
  std::optional<Result> result;
  bool active = true, changed = false;
  int cancelRequests = 0;
  auto takeProgress() { return std::exchange(progress, std::nullopt); }
  auto takeResult() {
    if (result) active = false;
    return std::exchange(result, std::nullopt);
  }
  bool inProgress() const { return active; }
  bool canDeleteArchive() const { return false; }
  bool takeLibraryChanged() { return std::exchange(changed, false); }
  void requestCancel() { ++cancelRequests; }
};
struct Control {
  std::string text;
  bool enabled = true;
  void setText(const std::string &value) { text = value; }
  void setMinHeight(float) {}
  void setHeight(float) {}
  void setEnabled(bool value) { enabled = value; }
  void applyYogaLayout() {}
};
class ArchiveUnzipModal {
public:
  Operation operation_;
  bool cancelling_ = false, batchMode_ = true, indexing_ = false;
  bool libraryChangedPending_ = false, hidden = false;
  Control title, message, cancel, cancelText, detail, root;
  Control *title_ = &title, *message_ = &message, *cancelButton_ = &cancel;
  Control *cancelText_ = &cancelText, *detail_ = &detail, *root_ = &root;
  struct {
    std::function<void()> libraryChanged;
    std::function<void(const Result &)> finished;
  } callbacks_;
  std::string displayedProgress;
  std::uint64_t displayedCurrent = 0;
  void updateProgress(double, const std::string &value,
                      std::uint64_t current = 0, std::uint64_t = 0) {
    displayedProgress = value;
    displayedCurrent = current;
  }
  void setDeleteVisible(bool) {}
  void hide() { hidden = true; }
  void update();
  void cancelOrClose();
};

MODAL_METHODS

int main() {
  ArchiveUnzipModal parallel;
  parallel.operation_.progress = Progress{
      .fraction = 0.2, .message = "b.zip - Writing", .current = 0, .total = 4,
      .indexing = false, .activeArchives = {"a.zip - Writing", "b.zip - Writing"}};
  parallel.update();
  assert(parallel.displayedProgress == "a.zip - Writing\nb.zip - Writing");
  assert(parallel.displayedCurrent == 0);
  parallel.operation_.progress = Progress{
      .fraction = 0.3, .message = "a.zip - Writing more", .current = 0, .total = 4,
      .indexing = false, .activeArchives = {"a.zip - Writing more", "b.zip - Writing"}};
  parallel.update();
  assert(parallel.displayedProgress == "a.zip - Writing more\nb.zip - Writing");
  parallel.operation_.progress = Progress{
      .fraction = 0.4, .current = 1, .total = 4, .indexing = false,
      .activeArchives = {"b.zip - Writing"}};
  parallel.update();
  assert(parallel.displayedProgress == "b.zip - Writing");
  assert(parallel.displayedCurrent == 1);
  ArchiveUnzipModal modal;
  modal.cancelOrClose();
  assert(modal.cancelling_ && modal.operation_.cancelRequests == 1);
  modal.operation_.progress = Progress{};
  modal.update();
  assert(modal.displayedProgress == "Indexing extracted charts");
  assert(!modal.cancel.enabled && modal.cancelText.text == "Indexing...");
  modal.cancelOrClose();
  assert(!modal.hidden && modal.operation_.cancelRequests == 1);
  int changes = 0, finishes = 0;
  modal.callbacks_.libraryChanged = [&]() { ++changes; };
  modal.callbacks_.finished = [&](const Result &) { ++finishes; };
  modal.operation_.result = Result{};
  modal.operation_.changed = true;
  modal.update();
  assert(modal.cancel.enabled && modal.cancelText.text == "Close");
  assert(changes == 1 && finishes == 1);
  modal.update();
  assert(changes == 1 && finishes == 1);
  modal.cancelOrClose();
  assert(modal.hidden);
  ArchiveUnzipModal single;
  single.batchMode_ = false;
  single.cancelOrClose();
  single.operation_.progress = Progress{};
  single.update();
  assert(single.displayedProgress == "Cancelling...");
  assert(single.cancel.enabled);
}
