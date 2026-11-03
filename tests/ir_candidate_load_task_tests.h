namespace {
void finishCandidateLoad(IrUploadsScene &scene) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (scene.candidateLoadTask.hasWorker() && std::chrono::steady_clock::now() < deadline) {
    scene.applyCandidateLoad();
    std::this_thread::yield();
  }
  assert(!scene.candidateLoadTask.hasWorker());
}

void testBackgroundCandidatesAndStaleScope() {
  PreparationContext application;
  IrUploadsScene scene(application);
  PreparationCandidateList list;
  scene.candidateList = &list;
  PreparationGate gate;
  const auto uiThread = std::this_thread::get_id();
  application.replayRepository.reader = [&](auto, std::size_t limit) {
    assert(std::this_thread::get_id() != uiThread && limit <= 64);
    gate.block();
    return ir::IrUploadCandidateSummaryReadOutcome{.status = ir::IrUploadCandidateReadStatus::Loaded,
        .candidates = {candidate(7)}};
  };
  scene.reloadCandidates();
  gate.wait(); // The application thread is free while the database is blocked.
  assert(scene.profileSwitchBlocker() && !scene.candidatesLoaded);
  scene.reloadRequested = true;
  gate.release.set_value();
  finishCandidateLoad(scene);
  assert(scene.controller.candidates().empty());
  application.replayRepository.reader = [](auto before, auto) {
    return ir::IrUploadCandidateSummaryReadOutcome{.status = ir::IrUploadCandidateReadStatus::Loaded,
        .candidates = {candidate(before ? 6 : 8)},
        .nextBeforeModernChartResultId = before ? std::nullopt : std::optional(8)};
  };
  scene.reloadCandidates();
  finishCandidateLoad(scene);
  assert(scene.candidatesLoaded && list.candidates.size() == 2 && list.scrollOffset == 25);
  assert(list.candidates.front().attemptId() == attemptId(8));
  assert(!scene.profileSwitchBlocker());
  // A failing refresh keeps the published controller state intact.
  application.replayRepository.reader = [](auto, auto) -> ir::IrUploadCandidateSummaryReadOutcome {
    throw std::runtime_error("read failed");
  };
  scene.reloadCandidates();
  finishCandidateLoad(scene);
  assert(!scene.loadError.empty() && scene.controller.candidates().size() == 2);
  scene.onProfileWillChange();
  assert(scene.controller.candidates().empty() && !scene.candidatesLoaded);
}

void testCandidateCancellationAndPagination() {
  PreparationContext application;
  IrUploadsScene scene(application);
  PreparationGate gate;
  int reads = 0;
  application.replayRepository.reader = [&](auto, auto) {
    ++reads;
    gate.block();
    return ir::IrUploadCandidateSummaryReadOutcome{.status = ir::IrUploadCandidateReadStatus::Loaded,
        .nextBeforeModernChartResultId = 9};
  };
  scene.reloadCandidates();
  gate.wait();
  scene.candidateLoadTask.requestStop();
  gate.release.set_value();
  scene.onProfileWillChange();
  assert(reads == 1 && !scene.candidateLoadTask.hasWorker() && !scene.candidateLoadTask.takeResult());
  application.replayRepository.reader = [](auto, auto) {
    return ir::IrUploadCandidateSummaryReadOutcome{.status = ir::IrUploadCandidateReadStatus::Loaded,
        .nextBeforeModernChartResultId = 9};
  };
  scene.reloadCandidates();
  finishCandidateLoad(scene);
  assert(!scene.loadError.empty() && !scene.candidatesLoaded);
}
} // namespace
