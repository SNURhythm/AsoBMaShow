# Shared Chart Library Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Execute
> inline in the current checkout; the user prohibited worktrees and subagents.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move chart scanning and library-task execution out of
`MainMenuScene` into one application-owned service without changing current
queue, progress, checkpoint, import, or gameplay-only pause behavior.

**Architecture:** A thread-safe `ChartLibraryTaskService` owns queue state and
the worker. `ChartLibraryOperations` performs the existing repository/scanner
operations and publishes value-owned completions. `ApplicationContext` owns
both for the whole app lifetime; MainMenu becomes a snapshot consumer.

**Tech Stack:** C++23, SDL2, `std::jthread`, `ChartRepository`,
`ChartLibraryScanner`, CMake/CTest.

**Spec:**
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`

## Global Constraints

- Preserve the exact current task statuses, 24-entry history, progress stages,
  pause checkpoints, automatic refresh, and platform import behavior.
- `GamePlayScene` is the only scene that pauses library work.
- Do not add a second repository, task queue, or worker.
- Keep picker UI, native list layout, filters, focus, and modals out of the
  shared service.
- Make each commit a coherent production slice with its focused tests; fold
  trivial corrections into that slice and split a slice before it becomes
  broad enough to obscure review.
- Do not modify `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not run a whole-file formatter.

---

### Task 1: Value-owned library task contract

**Files:**
- Create: `src/library/ChartLibraryTaskTypes.h`
- Test: `tests/chart_library_task_service_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ChartScanProgressStage` and
  `main_menu_library::FindBmsChartIdentity`.
- Produces:
  `chart_library_tasks::TaskRequest`, `TaskInfo`, `ProgressSnapshot`,
  `Snapshot`, `DownloadedIndexCompletion`, and `TaskRunResult`.

- [ ] **Step 1: Write the failing value-contract test**

```cpp
void testSnapshotCarriesQueueAndProgressAsValues() {
  chart_library_tasks::Snapshot snapshot{
      .revision = 7,
      .activeCount = 1,
      .tasks = {{.id = 42,
                 .title = "Refresh Library",
                 .status = chart_library_tasks::TaskStatus::Running,
                 .fraction = 0.25,
                 .current = 3,
                 .total = 12,
                 .detail = "Scanning folders"}},
      .progress = {.valid = true,
                   .revision = 8,
                   .taskId = 42,
                   .current = 3,
                   .total = 12,
                   .basisPoints = 2500,
                   .stage = ChartScanProgressStage::ScanningRoots}};
  require(snapshot.tasks.front().title == "Refresh Library" &&
              snapshot.progress.basisPoints == 2500,
          "task snapshot owns the task and progress values");
}
```

- [ ] **Step 2: Run the focused target and observe the missing-header failure**

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests -j 6`

Expected: compilation fails because `library/ChartLibraryTaskTypes.h` does not
exist.

- [ ] **Step 3: Add the task types with the current MainMenu fields**

```cpp
namespace chart_library_tasks {
enum class TaskStatus { Queued, Running, Complete, Failed, Paused };
enum class TaskKind { RefreshLibrary, IndexDownloadedPath, AndroidImport };

struct TaskRequest {
  std::uint64_t id = 0;
  TaskKind kind = TaskKind::RefreshLibrary;
  std::string title;
  std::filesystem::path folderToAdd;
  std::string iosBookmark;
  std::filesystem::path downloadedPath;
  std::vector<std::filesystem::path> downloadedRemovedPaths;
  main_menu_library::FindBmsChartIdentity downloadedTargetIdentity;
  std::uint64_t downloadedSelectionGeneration = 0;
  std::filesystem::path androidImportPath;
  bool androidImportFolder = false;
  bool rebuildLibraryMetadata = false;
};

struct TaskInfo {
  std::uint64_t id = 0;
  std::string title;
  TaskStatus status = TaskStatus::Queued;
  double fraction = 0.0;
  int current = 0;
  int total = 0;
  std::string detail;
};

struct ProgressSnapshot {
  bool valid = false;
  std::uint64_t revision = 0;
  std::uint64_t taskId = 0;
  int current = 0;
  int total = 0;
  int basisPoints = 0;
  ChartScanProgressStage stage = ChartScanProgressStage::Preparing;
};

struct Snapshot {
  std::uint64_t revision = 0;
  int activeCount = 0;
  std::vector<TaskInfo> tasks;
  ProgressSnapshot progress;
};

struct DownloadedIndexCompletion {
  std::filesystem::path chartPath;
  main_menu_library::FindBmsChartIdentity targetIdentity;
  std::uint64_t selectionGeneration = 0;
};

enum class TaskRunDisposition { Complete, Paused, Failed };
struct TaskRunResult {
  TaskRunDisposition disposition = TaskRunDisposition::Complete;
  std::string detail = "Complete";
  std::optional<DownloadedIndexCompletion> downloadedIndex;
};
} // namespace chart_library_tasks
```

- [ ] **Step 4: Register, build, and run the focused test**

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests -j 6 && ./cmake-build-debug/chart_library_task_service_tests`

Expected: PASS.

- [ ] **Step 5: Keep the contract with its executable worker slice**

Do not create a header-only contract commit. Task 2 completes the first
reviewable production slice and commits this header, the worker, and their
focused tests together.

### Task 2: Queue, progress, and pause state machine

**Files:**
- Create: `src/library/ChartLibraryTaskService.h`
- Create: `src/library/ChartLibraryTaskService.cpp`
- Modify: `tests/chart_library_task_service_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TaskRequest` and a `TaskRunner` callback.
- Produces:

```cpp
using TaskProgressCallback =
    std::function<void(const ChartScanProgress &, std::string_view)>;
using TaskPauseCallback = std::function<bool()>;
using TaskRunner = std::function<TaskRunResult(
    const TaskRequest &, const std::stop_token &, TaskProgressCallback,
    TaskPauseCallback)>;

class ChartLibraryTaskService final {
public:
  explicit ChartLibraryTaskService(TaskRunner);
  ~ChartLibraryTaskService();
  void start();
  void shutdown() noexcept;
  void setGameplayPaused(bool paused);
  std::uint64_t enqueue(TaskRequest);
  Snapshot snapshot() const;
  std::vector<DownloadedIndexCompletion> takeDownloadedIndexCompletions();
  bool active() const noexcept;
};
```

- [ ] **Step 1: Add a failing real-worker test for queue ordering**

```cpp
void testWorkerRunsQueuedTasksOnceInOrder() {
  std::mutex mutex;
  std::condition_variable_any changed;
  std::vector<std::string> titles;
  chart_library_tasks::ChartLibraryTaskService service(
      [&](const auto &request, const auto &, auto, auto) {
        {
          std::lock_guard lock(mutex);
          titles.push_back(request.title);
        }
        changed.notify_all();
        return chart_library_tasks::TaskRunResult{};
      });
  service.start();
  service.enqueue({.title = "first"});
  service.enqueue({.title = "second"});
  std::unique_lock lock(mutex);
  changed.wait(lock, [&] { return titles.size() == 2; });
  require(titles == std::vector<std::string>{"first", "second"},
          "one worker runs tasks once in FIFO order");
}
```

- [ ] **Step 2: Run the test and observe the undefined-service failure**

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests -j 6`

Expected: compilation fails because `ChartLibraryTaskService` is undefined.

- [ ] **Step 3: Implement the worker with the current status transitions**

Implement one `std::jthread`, the current condition-variable wait, sequential
FIFO dequeue, 24-item terminal-history trimming, the seqlock-style progress
snapshot, and `Queued -> Running -> Complete/Failed` transitions. Convert a
runner exception to `Failed` using `error.what()` exactly as MainMenu does.

```cpp
std::uint64_t ChartLibraryTaskService::enqueue(TaskRequest request) {
  request.id = nextTaskId_.fetch_add(1);
  const auto id = request.id;
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(request);
    tasks_.push_back({.id = id, .title = request.title,
                      .status = TaskStatus::Queued, .detail = "Waiting"});
    trimTerminalHistoryLocked(24);
    publishRevisionLocked();
  }
  start();
  workAvailable_.notify_one();
  return id;
}
```

- [ ] **Step 4: Add and run pause/resume and snapshot mutation tests**

Add a runner that blocks inside the supplied pause callback. Assert that
`setGameplayPaused(true)` publishes `Paused`, no second task starts, and
`setGameplayPaused(false)` lets the same task complete before the next task.

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests -j 6 && ./cmake-build-debug/chart_library_task_service_tests`

Expected: PASS without sleeps; use condition variables for every rendezvous.

- [ ] **Step 5: Commit the state machine**

```bash
git add CMakeLists.txt src/library/ChartLibraryTaskTypes.h src/library/ChartLibraryTaskService.h src/library/ChartLibraryTaskService.cpp tests/chart_library_task_service_tests.cpp
git commit -m "refactor: add shared library task worker"
```

### Task 3: Production chart-library operations

**Files:**
- Create: `src/library/ChartLibraryOperations.h`
- Create: `src/library/ChartLibraryOperations.cpp`
- Create: `tests/chart_library_operations_tests.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ChartRepository`, active `AppSettings`, settings-save callback,
  task request, progress callback, pause callback, and stop token.
- Produces:

```cpp
struct ChartLibraryOperationsDependencies {
  ChartRepository &repository;
  AppSettings &settings;
  std::function<bool()> saveSettings;
};

class ChartLibraryOperations final {
public:
  explicit ChartLibraryOperations(ChartLibraryOperationsDependencies);
  chart_library_tasks::TaskRunResult
  run(const chart_library_tasks::TaskRequest &, const std::stop_token &,
      chart_library_tasks::TaskProgressCallback,
      chart_library_tasks::TaskPauseCallback);
};
```

- [ ] **Step 1: Write failing operation tests against a temporary real repository**

Create a temporary SQLite repository, enqueue a refresh request with an empty
root fixture, and assert the operation calls the supplied pause callback and
returns `Paused` when it returns false. Write a minimal BMS and its empty audio
file inside the test's temporary root, then assert a completed refresh
increments the repository library revision.

```cpp
const auto result = operations.run(
    {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
     .title = "Refresh Library",
     .folderToAdd = fixtureRoot},
    stop.get_token(),
    [&](const ChartScanProgress &value, std::string_view) {
      observedStage = value.stage;
    },
    [] { return true; });
require(result.disposition ==
            chart_library_tasks::TaskRunDisposition::Complete,
        "refresh completes through the real scanner");
```

- [ ] **Step 2: Run the target and observe the missing operations type**

Run: `cmake --build cmake-build-debug --target chart_library_operations_tests -j 6`

Expected: compilation fails because `ChartLibraryOperations` is undefined.

- [ ] **Step 3: Move refresh/rebuild execution out of MainMenu**

Move `seedDefaultDifficultyTablesIfNeeded`, folder-entry bootstrap,
platform-specific root resolution/registration, cache clearing,
`ChartLibraryScanner::ScanWithResult`, flush checkpoints, and progress text to
`ChartLibraryOperations.cpp`. Preserve the existing call order and messages.
Replace `MainMenuScene &scene` in `LoadCharts` with callbacks owned by the
operations object; do not leave a UI pointer in worker code.

- [ ] **Step 4: Move downloaded-index and Android-import execution**

Move repository deletion, `ScanAddedWithResult`, target-path resolution,
archive extraction, Android import scanning, and completion publication into
the operation switch. Return `DownloadedIndexCompletion` instead of writing
`pendingFindBmsSelectionHandoff` from the worker thread. Picker threads remain
in MainMenu and only enqueue value-owned requests.

- [ ] **Step 5: Run operation and existing scanner tests**

Run: `cmake --build cmake-build-debug --target chart_library_operations_tests chart_library_scanner_tests main_menu_library_tests -j 6 && ./cmake-build-debug/chart_library_operations_tests && ./cmake-build-debug/chart_library_scanner_tests && ./cmake-build-debug/main_menu_library_tests`

Expected: PASS.

- [ ] **Step 6: Commit the production operation boundary**

```bash
git add CMakeLists.txt src/CMakeLists.txt src/library/ChartLibraryOperations.h src/library/ChartLibraryOperations.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp tests/chart_library_operations_tests.cpp
git commit -m "refactor: extract chart library operations"
```

### Task 4: Application ownership and MainMenu snapshot consumption

**Files:**
- Modify: `src/context.h`
- Modify: `src/main.cpp`
- Modify: `src/scene/SceneManager.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/chart_library_task_service_tests.cpp`
- Modify: `tests/application_startup_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces: `ApplicationContext::chartLibraryTasks` and
  `ApplicationContext::chartLibraryOperations` with application lifetime.

- [ ] **Step 1: Add failing application-lifetime and foreground-policy tests**

Assert that start is idempotent, a MainMenu pause/resume does not stop the
worker, a false foreground pause flag does not publish `Paused`, and a true
flag does. Assert destruction joins the worker exactly once.

```cpp
service.start();
service.start();
service.setGameplayPaused(false);
require(service.active(), "selector foreground keeps the worker active");
service.setGameplayPaused(true);
require(service.snapshot().tasks.front().status ==
            chart_library_tasks::TaskStatus::Paused,
        "gameplay foreground publishes the paused task");
```

- [ ] **Step 2: Run the focused tests and observe missing context ownership**

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests application_startup_tests -j 6`

Expected: the new context ownership assertions fail.

- [ ] **Step 3: Construct and start the service once in application startup**

Add the two owning pointers after repository/settings members. Construct the
operations object only after successful profile initialization, bind the task
runner to `ChartLibraryOperations::run`, start it in
`runReadyApplicationAfterResultRecovery`, and enqueue exactly one automatic
`Refresh Library` request before the Intro work planned later.

```cpp
context.chartLibraryTasks->start();
context.chartLibraryTasks->enqueue(
    {.kind = chart_library_tasks::TaskKind::RefreshLibrary,
     .title = "Refresh Library"});
```

- [ ] **Step 4: Bind SceneManager's existing performance flag to the service**

Keep `SceneManager::updateBackgroundTaskPauseState()` as the only scene-policy
authority. Replace the MainMenu-owned notification callback with:

```cpp
if (context.chartLibraryTasks) {
  context.chartLibraryTasks->setGameplayPaused(shouldPause);
}
```

Do not override `pausesBackgroundTasksForPerformance()` in any selector scene.

- [ ] **Step 5: Make MainMenu consume service snapshots**

Delete MainMenu's worker/thread/queue/progress members and worker methods.
`refreshTasksButton()` and the Tasks modal read one service `Snapshot` per UI
refresh. Consume downloaded-index completions on the main thread and create the
existing `PendingFindBmsSelectionHandoff` there. Enqueue refresh, rebuild,
download, and Android-import requests through the service.

- [ ] **Step 6: Run focused regression tests**

Run: `cmake --build cmake-build-debug --target chart_library_task_service_tests chart_library_operations_tests application_startup_tests main -j 6 && ./cmake-build-debug/chart_library_task_service_tests && ./cmake-build-debug/chart_library_operations_tests && ./cmake-build-debug/application_startup_tests`

Expected: PASS; MainMenu compiles without task-worker ownership.

- [ ] **Step 7: Commit application ownership**

```bash
git add CMakeLists.txt src/context.h src/main.cpp src/scene/SceneManager.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp tests/chart_library_task_service_tests.cpp tests/application_startup_tests.cpp
git commit -m "refactor: share chart library work across scenes"
```

### Task 5: Full shared-service verification

**Files:**
- Modify only if a focused failure identifies a production defect in files
  already listed by Tasks 1-4.

**Interfaces:**
- Consumes: completed shared service.
- Produces: a clean foundation for both music selectors.

- [ ] **Step 1: Run the library and repository suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'chart_library|chart_scan|main_menu_library|application_startup' -j 6`

Expected: all matching tests pass.

- [ ] **Step 2: Run the desktop compile check**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: PASS.

- [ ] **Step 3: Inspect ownership and diff hygiene**

Run: `rg -n 'checkEntriesThread|libraryTaskQueue|libraryTaskWorkerPaused' src/scene/MainMenuScene.* && git diff --check`

Expected: `rg` has no matches and `git diff --check` has no output.

- [ ] **Step 4: Keep corrections in their owning feature slice**

If Steps 1-3 expose a defect, return to that behavior's failing focused test,
make the smallest production correction, rerun the owning task, and include it
in that task's coherent commit. Do not create a standalone cleanup commit and
do not stage broad directory paths.
