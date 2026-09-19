// The controller supplies real one-shot archive tasks; effects are gated here.
void testProfileArchiveWorkerCompletionAndAdmission() {
  FakeServices fake;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto dependencies = fake.dependencies();
  dependencies.exportProfile = [&](std::string_view id, const std::filesystem::path &path) {
    REQUIRE(id == "bravo");
    REQUIRE(path == "profile.zip");
    entered.set_value();
    released.wait();
    return archiveSuccess(profile("bravo", "Bravo"), "Prepared.");
  };
  ProfileSettingsController controller(std::move(dependencies));
  ProfileArchiveWorker worker;
  REQUIRE(!worker.hasWorker());
  REQUIRE(!worker.takeCompletion());
  auto task = controller.beginExport("bravo", "profile.zip");
  REQUIRE(task.has_value());
  const auto generation = task->generation();
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weakCapture = capture;
  bool cleaned = false;
  worker.start(std::move(*task), [&, capture](ProfileArchiveResult &result) {
    REQUIRE(result.ok());
    cleaned = true;
    result.message += " Cleanup completed.";
  });
  capture.reset();
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  REQUIRE(worker.hasWorker());
  REQUIRE(!worker.takeCompletion());
  FakeServices otherFake;
  ProfileSettingsController other(otherFake.dependencies());
  auto rejected = other.beginExport("bravo", "other.zip");
  REQUIRE(rejected.has_value());
  const auto rejectedGeneration = rejected->generation();
  bool rejectedBusy = false;
  try { worker.start(std::move(*rejected)); }
  catch (const std::logic_error &) { rejectedBusy = true; }
  REQUIRE(rejectedBusy);
  other.abandonArchive(rejectedGeneration);
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  // Capture disposal happens after publication. Completed but unconsumed work
  // still owns admission even though its effects have finished.
  while (!weakCapture.expired()) {
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  REQUIRE(worker.hasWorker());
  auto awaitingRejected = other.beginExport("bravo", "still-busy.zip");
  REQUIRE(awaitingRejected.has_value());
  const auto awaitingGeneration = awaitingRejected->generation();
  rejectedBusy = false;
  try { worker.start(std::move(*awaitingRejected)); }
  catch (const std::logic_error &) { rejectedBusy = true; }
  REQUIRE(rejectedBusy);
  other.abandonArchive(awaitingGeneration);
  std::optional<ProfileArchiveCompletion> completion;
  while (!(completion = worker.takeCompletion())) {
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  REQUIRE(cleaned);
  REQUIRE(weakCapture.expired());
  REQUIRE(!worker.hasWorker());
  REQUIRE(!worker.takeCompletion());
  REQUIRE(completion->kind == ProfileArchiveTaskKind::Export);
  REQUIRE(completion->generation == generation);
  REQUIRE(completion->result.message == "Prepared. Cleanup completed.");
  REQUIRE(controller.phase() == ProfileSettingsPhase::PreparingExport);
  REQUIRE(fake.archivePipelineActive);
  REQUIRE(controller.completeArchive(completion->kind, completion->generation, completion->result));
  REQUIRE(!fake.archivePipelineActive);
}

void testProfileArchiveWorkerStopWaitsForCleanup(bool destroy) {
  FakeServices fake;
  std::promise<void> entered, release, cleanupEntered, releaseCleanup;
  auto released = release.get_future().share();
  auto cleanupReleased = releaseCleanup.get_future().share();
  auto dependencies = fake.dependencies();
  dependencies.importProfile = [&](const auto &, const auto &) {
    entered.set_value();
    released.wait();
    return archiveSuccess(profile("charlie", "Charlie"));
  };
  ProfileSettingsController controller(std::move(dependencies));
  auto worker = std::make_unique<ProfileArchiveWorker>();
  auto task = controller.beginImport("private.zip");
  REQUIRE(task.has_value());
  const auto generation = task->generation();
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weakCapture = capture;
  worker->start(std::move(*task), [&, capture](ProfileArchiveResult &result) {
    REQUIRE(result.ok());
    cleanupEntered.set_value();
    cleanupReleased.wait();
  });
  capture.reset();
  REQUIRE(entered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  std::promise<void> stopping;
  auto stopped = std::async(std::launch::async, [&] {
    stopping.set_value();
    if (destroy) worker.reset();
    else worker->stopAndWait();
  });
  stopping.get_future().wait();
  REQUIRE(stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  release.set_value();
  REQUIRE(cleanupEntered.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  REQUIRE(stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  releaseCleanup.set_value();
  REQUIRE(stopped.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  stopped.get();
  REQUIRE(weakCapture.expired());
  REQUIRE(fake.archivePipelineActive);
  controller.abandonArchive(generation);
  REQUIRE(!fake.archivePipelineActive);
  if (!destroy) {
    REQUIRE(!worker->hasWorker());
    REQUIRE(!worker->takeCompletion());
    // A stopped owner can be reused; operation exceptions retain task policy.
    auto failureDependencies = fake.dependencies();
    failureDependencies.exportProfile = [](auto, const auto &) -> ProfileArchiveResult {
      throw std::runtime_error("archive unavailable");
    };
    ProfileSettingsController failureController(std::move(failureDependencies));
    auto retry = failureController.beginExport("bravo", "retry.zip");
    REQUIRE(retry.has_value());
    bool cleanupRan = false;
    worker->start(std::move(*retry), [&](auto &result) {
      REQUIRE(!result.ok());
      cleanupRan = true;
    });
    std::optional<ProfileArchiveCompletion> completion;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(completion = worker->takeCompletion())) {
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
    }
    REQUIRE(cleanupRan);
    REQUIRE(completion->result.message.find("archive unavailable") != std::string::npos);
    REQUIRE(failureController.completeArchive(completion->kind, completion->generation, completion->result));
    worker->stopAndWait();
    worker->stopAndWait();
  }
}
