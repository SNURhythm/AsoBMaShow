#include "ThreadCompat.h"
#include "RAII.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#undef TARGET_OS_IOS
#undef TARGET_OS_SIMULATOR
#undef TARGET_OS_ANDROID
#define TARGET_OS_IOS ASOBMASHOW_PICKER_TEST_IOS
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_ANDROID (!ASOBMASHOW_PICKER_TEST_IOS)

using namespace std::chrono_literals;
thread_local bool failNextAllocation = false;
void *operator new(std::size_t size) {
  if (failNextAllocation) {
    failNextAllocation = false;
    throw std::bad_alloc();
  }
  if (void *allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

// Pause only the caller's active exchange. Worker publication still uses the
// real atomic store through the production std::atomic_bool reference.
std::function<void()> beforeActiveExchange;
struct ObservedActive : std::atomic_bool {
  ObservedActive(bool value) : std::atomic_bool(value) {}
  bool exchange(bool value, std::memory_order order = std::memory_order_seq_cst) {
    if (beforeActiveExchange) beforeActiveExchange();
    return std::atomic_bool::exchange(value, order);
  }
};

struct NativeState {
  std::atomic_int calls = 0;
  bool blocked = false;
  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
};
NativeState *native = nullptr;
bool allFilesAccess = true;
std::string pick() {
  const int call = ++native->calls;
  if (native->blocked && call == 1) {
    native->entered.set_value();
    native->released.wait();
  }
  return "folder-" + std::to_string(call);
}
bool PickIOSFolder(std::string &path, std::string &bookmark, std::string &) {
  path = pick(); bookmark = "bookmark"; return true;
}
bool PickAndroidChartFolder(std::filesystem::path &path, std::string &bookmark,
                           std::string &, const std::stop_token &) {
  path = pick(); bookmark = "tree-uri"; return true;
}
bool PickAndroidFolderForImport(std::filesystem::path &path, std::string &) {
  path = pick(); return true;
}
bool PickAndroidArchiveForImport(std::filesystem::path &path, std::string &) {
  path = pick(); return true;
}
bool AndroidBuildHasManageExternalStorage() { return allFilesAccess; }
void UnregisterAndroidImportTasks(int &) {}
void SDL_Log(const char *, ...) {}
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }

namespace chart_library_platform {
SOUND_PICKER_DECLARATIONS
SOUND_PICKER_METHODS

// Database insertion/task dispatch are observed effects. Request methods and
// worker lambdas are copied in full from production, including platform gates.
class FolderActionService {
  struct Impl {
    std::jthread pickerThread;
    std::atomic_bool pickerActive = false;
    int *tasks = nullptr;
    std::atomic_int enqueued = 0;
    void enqueueFolder(const std::filesystem::path &, const std::string &) { ++enqueued; }
#if TARGET_OS_ANDROID
    IMPORT_METHOD
#endif
  };
  std::unique_ptr<Impl> impl_ = std::make_unique<Impl>();
public:
  ~FolderActionService();
  void requestAddFolder();
  void requestImportArchive();
  bool active() const noexcept;
  int enqueued() const { return impl_->enqueued.load(); }
};
FOLDER_METHODS
} // namespace chart_library_platform

template <typename Picker>
void waitForIdle(Picker &picker) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (picker.active() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  require(!picker.active(), "native picker should finish");
}
template <typename Picker, typename Request>
void checkFailedLaunchCanRetry(Picker &picker, Request request) {
  failNextAllocation = true;
  bool failed = false;
  try { request(); } catch (const std::bad_alloc &) { failed = true; }
  failNextAllocation = false;
  require(failed, "thread launch allocation should fail");
  require(!picker.active(), "failed picker launch must release active admission");
  require(native->calls == 0, "failed launch cannot call the native picker");
  request();
  waitForIdle(picker);
  require(native->calls == 1, "a later request should reach the native picker");
}
void testSoundStartup() {
  NativeState state;
  native = &state;
  chart_library_platform::SoundSetFolderPicker picker;
  checkFailedLaunchCanRetry(picker, [&] { picker.request(); });
  const auto result = picker.consume();
  require(result && result->succeed && result->path == "folder-1", "retry keeps the selected result");
  require(!picker.consume(), "result is consumed once");
}
void testFolderStartup() {
#if TARGET_OS_ANDROID
  constexpr int routes = 3;
#else
  constexpr int routes = 1;
#endif
  for (int route = 0; route < routes; ++route) {
    NativeState state;
    native = &state;
    allFilesAccess = route == 0;
    chart_library_platform::FolderActionService picker;
    checkFailedLaunchCanRetry(picker, [&] {
      if (route == 2) picker.requestImportArchive();
      else picker.requestAddFolder();
    });
    require(picker.enqueued() == (route == 0 ? 1 : 0), "direct and import routes retain enqueue policy");
  }
}
void testSoundAdmissionKeepsPendingResult() {
  NativeState state;
  state.blocked = true;
  native = &state;
  chart_library_platform::SoundSetFolderPicker picker;
  auto entered = state.entered.get_future();
  picker.request();
  require(entered.wait_for(3s) == std::future_status::ready, "first picker should begin");
  std::promise<void> gap, releaseGap;
  auto reachedGap = gap.get_future();
  auto releasedGap = releaseGap.get_future().share();
  beforeActiveExchange = [&] { gap.set_value(); releasedGap.wait(); };
  auto second = std::async(std::launch::async, [&] { picker.request(); });
  require(reachedGap.wait_for(3s) == std::future_status::ready, "request should reach active admission");
  state.release.set_value();
  waitForIdle(picker); // First result is published before active is released.
  releaseGap.set_value();
  require(second.wait_for(3s) == std::future_status::ready, "racing request should return");
  second.get();
  beforeActiveExchange = {};
  require(!picker.active() && state.calls == 1, "pending result must prevent a second native pick");
  const auto result = picker.consume();
  require(result && result->path == "folder-1", "admission preserves the first pending result");
  require(!picker.consume(), "pending result is consumed once");
  picker.request();
  waitForIdle(picker);
  const auto next = picker.consume();
  require(state.calls == 2 && next && next->path == "folder-2", "consuming result permits a new pick");
}
int main(int argc, char **argv) {
  require(argc == 2, "expected picker lifecycle test mode");
  const std::string_view mode(argv[1]);
  if (mode == "sound-startup") testSoundStartup();
  else if (mode == "folder-startup") testFolderStartup();
  else if (mode == "sound-admission") testSoundAdmissionKeepsPendingResult();
  else require(false, "unknown picker lifecycle test mode");
}
