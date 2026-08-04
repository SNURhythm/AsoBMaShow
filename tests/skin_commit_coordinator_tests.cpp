#include "skin/SkinCommitCoordinator.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "support/SkinActivationCommitStoreFake.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-skin-commit-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add,
                    ignored);
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class FakeProfileOwner final : public ISkinProfileSettingsOwner,
                               public ISkinProfileSnapshotProvider {
public:
  explicit FakeProfileOwner(std::vector<SkinProfileId> profiles) {
    for (auto &profile : profiles) {
      snapshots_.emplace(
          profile.opaque,
          VersionedSkinProfileSettings{.profileId = profile, .generation = 1});
    }
  }

  VersionedSkinProfileSettings
  snapshot(const SkinProfileId &profile) const override {
    return snapshots_.at(profile.opaque);
  }

  SkinProfileCommitResult beginCommit(const SkinProfileId &profile,
                                      std::uint64_t expectedGeneration,
                                      SkinProfileSettings candidate) override {
    auto found = snapshots_.find(profile.opaque);
    if (found == snapshots_.end()) {
      return retryable("skin.test.profile_missing", "Profile is missing");
    }
    if (found->second.generation != expectedGeneration) {
      return {.status = SkinProfileCommitResult::Status::GenerationChanged,
              .generationChanged = true,
              .snapshot = found->second};
    }
    if (unresolved_.contains(profile.opaque)) {
      return retryable("skin.test.profile_busy", "Profile is busy");
    }

    const auto ticket = ++nextTicket_;
    ++found->second.generation;
    found->second.settings = std::move(candidate);
    unresolved_[profile.opaque] = ticket;
    commits_.emplace(ticket,
                     Commit{.profile = profile,
                            .terminalAfterPolls = defaultTerminalAfterPolls,
                            .terminalStatus = defaultTerminalStatus});
    events.push_back("begin:" + std::to_string(ticket));
    return {.status = SkinProfileCommitResult::Status::Pending,
            .ticket = ticket,
            .snapshot = found->second};
  }

  SkinProfileCommitResult pollCommit(std::uint64_t ticket) override {
    auto found = commits_.find(ticket);
    if (found == commits_.end()) {
      return retryable("skin.test.ticket_missing", "Ticket is missing", ticket);
    }
    auto &commit = found->second;
    if (commit.polls++ < commit.terminalAfterPolls) {
      events.push_back("pending:" + std::to_string(ticket));
      return {.status = SkinProfileCommitResult::Status::Pending,
              .ticket = ticket,
              .snapshot = snapshots_.at(commit.profile.opaque)};
    }
    events.push_back("terminal:" + std::to_string(ticket));
    SkinProfileCommitResult result{.status = commit.terminalStatus,
                                   .ticket = ticket,
                                   .snapshot =
                                       snapshots_.at(commit.profile.opaque)};
    if (commit.terminalStatus ==
        SkinProfileCommitResult::Status::RetryableFailure) {
      result.failure = SkinDiagnostic{.code = "skin.test.save_failed",
                                      .message = "Synthetic save failure"};
    }
    return result;
  }

  void acknowledgeCommit(std::uint64_t ticket) noexcept override {
    if (acknowledgementAttempts++ == 0) {
      firstAcknowledgementAttempt = ticket;
    }
    const auto found = commits_.find(ticket);
    if (found == commits_.end()) {
      return;
    }
    events.push_back("ack:" + std::to_string(ticket));
    unresolved_.erase(found->second.profile.opaque);
    commits_.erase(found);
    ++acknowledgements;
  }

  std::uint64_t beginSnapshotAllProfiles() override { return 1; }
  std::optional<AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t) override {
    return AllSkinProfileSnapshotsResult{.complete = true};
  }
  void cancelSnapshotAllProfiles(std::uint64_t) noexcept override {}
  std::optional<ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) override {
    return std::nullopt;
  }
  ProfileInventoryMutationBarrier beginInventoryMutation() override {
    throw std::logic_error("not used by coordinator tests");
  }
  void finishInventoryMutation(
      ProfileInventoryMutationBarrier &&) noexcept override {}

  void setTerminal(std::uint64_t ticket,
                   SkinProfileCommitResult::Status status) {
    commits_.at(ticket).terminalStatus = status;
    commits_.at(ticket).terminalAfterPolls = 0;
  }

  bool hasUnresolved(const SkinProfileId &profile) const {
    return unresolved_.contains(profile.opaque);
  }

  void setNextTicket(std::uint64_t ticket) noexcept { nextTicket_ = ticket; }

  std::size_t acknowledgements = 0;
  std::size_t acknowledgementAttempts = 0;
  std::uint64_t firstAcknowledgementAttempt = 0;
  int defaultTerminalAfterPolls = 0;
  SkinProfileCommitResult::Status defaultTerminalStatus =
      SkinProfileCommitResult::Status::Persisted;
  std::vector<std::string> events;

private:
  struct Commit {
    SkinProfileId profile;
    int terminalAfterPolls = 0;
    int polls = 0;
    SkinProfileCommitResult::Status terminalStatus =
        SkinProfileCommitResult::Status::Persisted;
  };

  static SkinProfileCommitResult
  retryable(std::string code, std::string message, std::uint64_t ticket = 0) {
    return {.status = SkinProfileCommitResult::Status::RetryableFailure,
            .ticket = ticket,
            .failure = SkinDiagnostic{.code = std::move(code),
                                      .message = std::move(message)}};
  }

  std::uint64_t nextTicket_ = 0;
  std::map<std::string, VersionedSkinProfileSettings> snapshots_;
  std::map<std::string, std::uint64_t> unresolved_;
  std::map<std::uint64_t, Commit> commits_;
};

struct Fixture {
  explicit Fixture(std::vector<SkinProfileId> profiles = {{"A"}, {"B"}})
      : roots(rootsBelow(temp.root())), catalog(roots.privateCatalog),
        owner(std::move(profiles)), coordinator(store, owner) {}

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageCatalog catalog;
  NoAliases aliases;
  FakeProfileOwner owner;
  test_support::SkinActivationCommitStoreFake store;
  SkinCommitCoordinator coordinator;
};

SkinProfileSettings changedSettings(int marker) {
  SkinProfileSettings settings;
  settings.gameplayCompatibilityEnabled = marker != 0;
  return settings;
}

PreparedSkinActivation makePreparedActivation(Fixture &fixture,
                                              const SkinProfileId &profile) {
  const auto package = normalizePackageId("FixtureSkin").package;
  const auto entry = normalizeEntryPath(*package, "play/play7.luaskin").entry;
  const auto source = fixture.temp.root() / "activation-source";
  fs::create_directories(source / "play");
  std::ofstream(source / "play/play7.luaskin") << "return { type = 0 }";
  SkinTreeSnapshotter snapshotter(fixture.roots, fixture.aliases);
  auto snapshot = snapshotter.snapshot(source, *package, {}, {});
  std::string error;
  auto lease = std::move(*snapshot.prepared).publish(error);
  expect(lease.has_value(), "activation fixture publishes an immutable lease");

  auto base = fixture.owner.snapshot(profile);
  return {.sourceGeneration = 1,
          .catalogGeneration = 1,
          .expectedProfileGeneration = base.generation,
          .profileId = profile,
          .activation = {.revision = std::move(*lease),
                         .entry = *entry,
                         .reconciledSettings = {},
                         .configurationDigest = "fixture-digest"},
          .candidateProfileSettings = changedSettings(1)};
}

void testIdsTicketsAndProfileCompletionAreNeverReused() {
  Fixture fixture;
  const auto firstClient = fixture.coordinator.createClient();
  const auto secondClient = fixture.coordinator.createClient();
  expect(firstClient != 0 && secondClient > firstClient,
         "client IDs are nonzero and monotonic");

  const auto base = fixture.owner.snapshot({"A"});
  const auto submitted = fixture.coordinator.submitProfileSettings(
      firstClient, base, changedSettings(1));
  expect(submitted.accepted && submitted.ticket != 0,
         "a matching profile-only candidate receives a coordinator ticket");
  fixture.coordinator.poll();
  auto completions = fixture.coordinator.takeProfileCompletions(firstClient);
  expect(
      completions.size() == 1 &&
          completions.front().ticket == submitted.ticket &&
          completions.front().result.status ==
              SkinProfileCommitResult::Status::Persisted,
      "the profile-only completion is delivered under its coordinator ticket");
  expect(fixture.owner.acknowledgements == 1,
         "the terminal owner ticket is acknowledged exactly once");

  Fixture later;
  const auto laterClient = later.coordinator.createClient();
  expect(laterClient > secondClient,
         "client IDs remain monotonic across coordinator instances");
  const auto laterSubmission = later.coordinator.submitProfileSettings(
      laterClient, later.owner.snapshot({"A"}), changedSettings(1));
  expect(laterSubmission.ticket > submitted.ticket,
         "coordinator tickets remain monotonic across instances");
}

void testStaleGenerationAndSaveFailureRetainTypedOutcomes() {
  Fixture fixture;
  const auto client = fixture.coordinator.createClient();
  auto stale = fixture.owner.snapshot({"A"});
  --stale.generation;
  const auto rejected = fixture.coordinator.submitProfileSettings(
      client, stale, changedSettings(1));
  expect(!rejected.accepted && !rejected.diagnostics.empty(),
         "a stale profile generation is rejected with a diagnostic");

  fixture.owner.defaultTerminalStatus =
      SkinProfileCommitResult::Status::RetryableFailure;
  const auto accepted = fixture.coordinator.submitProfileSettings(
      client, fixture.owner.snapshot({"A"}), changedSettings(1));
  expect(accepted.accepted, "a save failure occurs after durable admission");
  fixture.coordinator.poll();
  auto completion = fixture.coordinator.takeProfileCompletions(client);
  expect(completion.size() == 1 &&
             completion.front().result.status ==
                 SkinProfileCommitResult::Status::RetryableFailure &&
             completion.front().result.failure.has_value(),
         "a profile-save failure is delivered as its typed terminal result");
}

void testDetachCannotRetargetLateProfileCompletion() {
  Fixture fixture;
  fixture.owner.defaultTerminalAfterPolls = 1;
  const auto oldClient = fixture.coordinator.createClient();
  const auto accepted = fixture.coordinator.submitProfileSettings(
      oldClient, fixture.owner.snapshot({"A"}), changedSettings(1));
  fixture.coordinator.detachClient(oldClient);
  const auto newClient = fixture.coordinator.createClient();
  fixture.coordinator.poll();
  fixture.coordinator.poll();
  expect(accepted.accepted && fixture.owner.acknowledgements == 1,
         "detaching delivery does not abandon the accepted transaction");
  expect(fixture.coordinator.takeProfileCompletions(oldClient).empty() &&
             fixture.coordinator.takeProfileCompletions(newClient).empty(),
         "a late old completion is neither retained nor retargeted");
}

void testProfileMutationBarrierDrainsAndResumesByOutcome() {
  Fixture fixture;
  fixture.owner.defaultTerminalAfterPolls = 2;
  const auto client = fixture.coordinator.createClient();
  expect(fixture.coordinator
             .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                    changedSettings(1))
             .accepted,
         "profile work is accepted before the mutation gate");
  auto begin = fixture.coordinator.beginProfileMutation({"A"});
  expect(begin.barrier.has_value() && !fixture.owner.hasUnresolved({"A"}),
         "beginProfileMutation waits until accepted owner work is terminal");
  expect(!fixture.coordinator
              .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                     changedSettings(0))
              .accepted,
         "the barrier blocks new submissions for its profile");

  fixture.coordinator.finishProfileMutation(std::move(*begin.barrier), false,
                                            true);
  expect(fixture.coordinator
             .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                    changedSettings(0))
             .accepted,
         "a failed profile mutation resumes submissions without deletion");
  fixture.coordinator.poll();

  auto abandoned = fixture.coordinator.beginProfileMutation({"A"});
  abandoned.barrier.reset();
  expect(
      fixture.coordinator
          .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                 changedSettings(1))
          .accepted,
      "an abandoned barrier resumes submissions without deleting activation");
  fixture.coordinator.poll();

  auto overwrite = fixture.coordinator.beginProfileMutation({"A"});
  fixture.coordinator.finishProfileMutation(std::move(*overwrite.barrier), true,
                                            true);
  expect(fixture.store.removedProfileCount() == 1 &&
             fixture.coordinator
                 .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                        changedSettings(0))
                 .accepted,
         "successful overwrite removes old activation keys then resumes");
  fixture.coordinator.poll();

  auto deletion = fixture.coordinator.beginProfileMutation({"A"});
  fixture.coordinator.finishProfileMutation(std::move(*deletion.barrier), true,
                                            false);
  expect(
      fixture.store.removedProfileCount() == 2 &&
          !fixture.coordinator
               .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                      changedSettings(1))
               .accepted,
      "successful deletion removes activation keys and keeps the ID blocked");
}

void testProfileMutationBarrierCannotBeFinishedByAnotherCoordinator() {
  Fixture first;
  Fixture second;
  const auto firstClient = first.coordinator.createClient();
  auto barrier = first.coordinator.beginProfileMutation({"A"});
  expect(barrier.barrier.has_value(),
         "the first coordinator issues its profile barrier");

  second.coordinator.finishProfileMutation(std::move(*barrier.barrier), true,
                                           false);
  expect(second.store.removedProfileCount() == 0,
         "a foreign barrier cannot remove another store's activation keys");
  barrier.barrier.reset();
  expect(
      first.coordinator
          .submitProfileSettings(firstClient, first.owner.snapshot({"A"}),
                                 changedSettings(1))
          .accepted,
      "rejecting a foreign finish leaves barrier abandonment able to resume");
}

void testFailedActivationRemovalResumesProfileGate() {
  Fixture fixture;
  const auto client = fixture.coordinator.createClient();
  auto barrier = fixture.coordinator.beginProfileMutation({"A"});
  fixture.store.throwOnNextProfileRemoval();
  fixture.coordinator.finishProfileMutation(std::move(*barrier.barrier), true,
                                            false);
  expect(fixture.store.removedProfileCount() == 0 &&
             fixture.coordinator
                 .submitProfileSettings(client, fixture.owner.snapshot({"A"}),
                                        changedSettings(1))
                 .accepted,
         "failed activation-key removal preserves activation and resumes the "
         "gate");
}

void testActivationPollingRevalidationDetachAndLeaseRelease() {
  Fixture fixture;
  const auto client = fixture.coordinator.createClient();
  auto prepared = makePreparedActivation(fixture, {"A"});
  auto weakPin = prepared.activation.revision.weakPin();
  fixture.store.setNextActivationDisposition(
      ActivationCommitDisposition::ProfileCommittedNeedsRevalidation);
  const auto submitted =
      fixture.coordinator.submitActivation(client, std::move(prepared));
  expect(
      submitted.accepted && weakPin.hasLiveLease(),
      "accepted activation retains its revision lease while save is pending");
  fixture.coordinator.poll();
  auto completions = fixture.coordinator.takeCompletions(client);
  auto revalidation = fixture.coordinator.takeRevalidationRequests();
  expect(
      completions.size() == 1 &&
          completions.front().result.disposition ==
              ActivationCommitDisposition::ProfileCommittedNeedsRevalidation &&
          revalidation.size() == 1 &&
          revalidation.front().profileId.opaque == "A",
      "a post-save source race yields one completion and owner-snapshot "
      "revalidation");
  expect(
      fixture.owner.acknowledgements == 1,
      "activation owner work is acknowledged only after terminal CAS polling");
  completions.clear();
  expect(!weakPin.hasLiveLease(), "terminal revalidation releases the "
                                  "coordinator's retained revision lease");

  auto detachedPrepared = makePreparedActivation(fixture, {"B"});
  auto detachedPin = detachedPrepared.activation.revision.weakPin();
  const auto detachedClient = fixture.coordinator.createClient();
  expect(fixture.coordinator
             .submitActivation(detachedClient, std::move(detachedPrepared))
             .accepted,
         "a second activation is accepted");
  fixture.coordinator.detachClient(detachedClient);
  fixture.coordinator.poll();
  expect(fixture.coordinator.takeCompletions(detachedClient).empty() &&
             !detachedPin.hasLiveLease(),
         "detached delivery is discarded and its retained result lease is "
         "released");
}

void testRevalidationRequestsCoalesceLatestSnapshotPerProfile() {
  Fixture fixture;
  const auto client = fixture.coordinator.createClient();
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto prepared = makePreparedActivation(fixture, {"A"});
    fixture.store.setNextActivationDisposition(
        ActivationCommitDisposition::ProfileCommittedNeedsRevalidation);
    expect(fixture.coordinator.submitActivation(client, std::move(prepared))
               .accepted,
           "revalidation fixture activation is accepted");
    fixture.coordinator.poll();
    fixture.coordinator.takeCompletions(client);
  }
  auto otherProfile = makePreparedActivation(fixture, {"B"});
  fixture.store.setNextActivationDisposition(
      ActivationCommitDisposition::ProfileCommittedNeedsRevalidation);
  expect(fixture.coordinator.submitActivation(client, std::move(otherProfile))
             .accepted,
         "a second profile can retain independent revalidation work");
  fixture.coordinator.poll();
  fixture.coordinator.takeCompletions(client);

  const auto expectedGeneration = fixture.owner.snapshot({"A"}).generation;
  const auto requests = fixture.coordinator.takeRevalidationRequests();
  expect(
      requests.size() == 2 && requests.front().profileId.opaque == "A" &&
          requests.front().generation == expectedGeneration &&
          requests.back().profileId.opaque == "B",
      "revalidation delivery coalesces the latest snapshot independently per "
      "profile");
}

void testPausedPollingAndShutdownResolveAcceptedOwnerWorkExactlyOnce() {
  Fixture fixture;
  fixture.owner.defaultTerminalAfterPolls = 1;
  const auto client = fixture.coordinator.createClient();
  auto prepared = makePreparedActivation(fixture, {"A"});
  auto pin = prepared.activation.revision.weakPin();
  const auto submitted =
      fixture.coordinator.submitActivation(client, std::move(prepared));
  expect(submitted.accepted,
         "activation admission succeeds before application polling pauses");

  // Simulate unrelated full-save/profile-switch activity: the owner terminal
  // remains idempotently available because only this coordinator acknowledges.
  fixture.owner.setTerminal(1, SkinProfileCommitResult::Status::Persisted);
  const auto externallyObserved = fixture.owner.pollCommit(1);
  expect(externallyObserved.status ==
                 SkinProfileCommitResult::Status::Persisted &&
             fixture.owner.acknowledgements == 0,
         "ordinary owner polling observes but does not consume terminal save "
         "state");
  fixture.coordinator.poll();
  fixture.coordinator.poll();
  auto completions = fixture.coordinator.takeCompletions(client);
  expect(completions.size() == 1 && fixture.owner.acknowledgements == 1,
         "resumed polling performs exactly one activation CAS and "
         "acknowledgement");
  completions.clear();
  expect(!pin.hasLiveLease(),
         "taking and releasing completion bounds lease lifetime");

  fixture.owner.defaultTerminalAfterPolls = 2;
  const auto pending = fixture.coordinator.submitProfileSettings(
      client, fixture.owner.snapshot({"B"}), changedSettings(1));
  fixture.coordinator.shutdown();
  fixture.coordinator.shutdown();
  expect(
      pending.accepted && !fixture.owner.hasUnresolved({"B"}) &&
          fixture.owner.acknowledgements == 2,
      "idempotent shutdown drains and acknowledges accepted coordinator work");
  expect(!fixture.coordinator
              .submitProfileSettings(client, fixture.owner.snapshot({"B"}),
                                     changedSettings(0))
              .accepted,
         "shutdown permanently stops new submissions");

  Fixture longPending;
  longPending.owner.defaultTerminalAfterPolls = 96;
  const auto longClient = longPending.coordinator.createClient();
  auto longPrepared = makePreparedActivation(longPending, {"A"});
  auto longPin = longPrepared.activation.revision.weakPin();
  expect(longPending.coordinator
             .submitActivation(longClient, std::move(longPrepared))
             .accepted,
         "long-pending activation is admitted before shutdown");
  longPending.coordinator.shutdown();
  expect(longPending.owner.acknowledgements == 1 && !longPin.hasLiveLease(),
         "bounded shutdown backoff still waits for the liveness-guaranteed "
         "terminal result");
}

void testActivationStoreOwnsItsDistinctOwnerAcknowledgement() {
  Fixture fixture;
  fixture.owner.setNextTicket(41);
  const auto client = fixture.coordinator.createClient();
  auto prepared = makePreparedActivation(fixture, {"A"});
  const auto submitted =
      fixture.coordinator.submitActivation(client, std::move(prepared));
  fixture.coordinator.poll();

  expect(submitted.accepted && fixture.owner.acknowledgementAttempts == 1 &&
             fixture.owner.firstAcknowledgementAttempt == 42 &&
             fixture.owner.acknowledgements == 1,
         "the Store uses a distinct ticket namespace and alone acknowledges "
         "its terminal owner commit");
}

void testOffMainShutdownRetainsOwnershipForMainThreadDrain() {
  Fixture fixture;
  fixture.owner.defaultTerminalAfterPolls = 1;
  const auto client = fixture.coordinator.createClient();
  const auto submitted = fixture.coordinator.submitProfileSettings(
      client, fixture.owner.snapshot({"A"}), changedSettings(1));
  std::thread wrongThread([&] { fixture.coordinator.shutdown(); });
  wrongThread.join();

  expect(submitted.accepted && fixture.owner.hasUnresolved({"A"}) &&
             fixture.owner.acknowledgementAttempts == 0,
         "off-main shutdown rejects the call without abandoning accepted "
         "owner work");
  fixture.coordinator.shutdown();
  expect(!fixture.owner.hasUnresolved({"A"}) &&
             fixture.owner.acknowledgementAttempts == 1 &&
             fixture.owner.acknowledgements == 1,
         "a later owning-thread shutdown drains and acknowledges exactly "
         "once");
}

void testOffMainShutdownRetainsTerminalActivationDelivery() {
  Fixture fixture;
  const auto client = fixture.coordinator.createClient();
  auto prepared = makePreparedActivation(fixture, {"A"});
  auto pin = prepared.activation.revision.weakPin();
  const auto submitted =
      fixture.coordinator.submitActivation(client, std::move(prepared));
  fixture.coordinator.poll();
  std::thread wrongThread([&] { fixture.coordinator.shutdown(); });
  wrongThread.join();

  auto completions = fixture.coordinator.takeCompletions(client);
  expect(submitted.accepted && completions.size() == 1 &&
             fixture.owner.acknowledgements == 1 && pin.hasLiveLease(),
         "off-main shutdown retains a terminal activation completion and its "
         "revision lease for owning-thread delivery");
  completions.clear();
  fixture.coordinator.shutdown();
  expect(!pin.hasLiveLease(),
         "consuming the retained completion releases its activation lease");
}

#if defined(__APPLE__) || defined(__unix__)
void testOffMainDestructionWithAcceptedWorkTerminates() {
  constexpr int terminatedExit = 86;
  constexpr int returnedExit = 87;
  auto fixture = std::make_unique<Fixture>();
  fixture->owner.defaultTerminalAfterPolls = 1;
  const auto client = fixture->coordinator.createClient();
  const auto submitted = fixture->coordinator.submitProfileSettings(
      client, fixture->owner.snapshot({"A"}), changedSettings(1));
  expect(submitted.accepted,
         "death-test fixture owns an accepted transaction before fork");

  const auto child = ::fork();
  if (child == 0) {
    std::set_terminate([] { std::_Exit(terminatedExit); });
    std::thread wrongThread([&] { fixture.reset(); });
    wrongThread.join();
    std::_Exit(returnedExit);
  }
  if (child < 0) {
    expect(false, "off-main destruction death test forks successfully");
    fixture->coordinator.shutdown();
    return;
  }

  int status = 0;
  const auto waited = ::waitpid(child, &status, 0);
  expect(waited == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == terminatedExit,
         "off-main destruction with accepted work fails fast instead of "
         "dropping ownership");
  fixture->coordinator.shutdown();
}

void testOffMainDestructionWithTerminalActivationDeliveryTerminates() {
  constexpr int terminatedExit = 88;
  constexpr int returnedExit = 89;
  auto fixture = std::make_unique<Fixture>();
  const auto client = fixture->coordinator.createClient();
  auto prepared = makePreparedActivation(*fixture, {"A"});
  const auto submitted =
      fixture->coordinator.submitActivation(client, std::move(prepared));
  fixture->coordinator.poll();
  expect(submitted.accepted && fixture->owner.acknowledgements == 1,
         "terminal-delivery death fixture drains Store ownership before fork");

  const auto child = ::fork();
  if (child == 0) {
    std::set_terminate([] { std::_Exit(terminatedExit); });
    std::thread wrongThread([&] { fixture.reset(); });
    wrongThread.join();
    std::_Exit(returnedExit);
  }
  if (child < 0) {
    expect(false, "terminal-delivery off-main destruction death test forks");
    fixture->coordinator.shutdown();
    return;
  }

  int status = 0;
  const auto waited = ::waitpid(child, &status, 0);
  expect(waited == child && WIFEXITED(status) &&
             WEXITSTATUS(status) == terminatedExit,
         "off-main destruction fails fast while an accepted terminal "
         "activation delivery is unconsumed");
  fixture->coordinator.shutdown();
}
#endif

void testCompletionDeliveryIsBounded() {
  std::vector<SkinProfileId> profiles;
  for (int index = 0; index < 200; ++index) {
    profiles.push_back({"profile-" + std::to_string(index)});
  }
  Fixture fixture(std::move(profiles));
  const auto client = fixture.coordinator.createClient();
  std::set<std::uint64_t> acceptedTickets;
  std::vector<SkinProfileId> rejectedProfiles;
  for (int index = 0; index < 200; ++index) {
    const SkinProfileId profile{"profile-" + std::to_string(index)};
    const auto submission = fixture.coordinator.submitProfileSettings(
        client, fixture.owner.snapshot(profile), changedSettings(1));
    if (submission.accepted) {
      acceptedTickets.insert(submission.ticket);
    } else {
      rejectedProfiles.push_back(profile);
    }
  }
  expect(!acceptedTickets.empty() && !rejectedProfiles.empty() &&
             acceptedTickets.size() + rejectedProfiles.size() == 200,
         "bounded delivery admission explicitly accounts for all 200 requests");
  fixture.coordinator.poll();
  const auto firstCompletions =
      fixture.coordinator.takeProfileCompletions(client);
  std::set<std::uint64_t> completedTickets;
  for (const auto &completion : firstCompletions) {
    completedTickets.insert(completion.ticket);
  }
  expect(completedTickets == acceptedTickets,
         "every accepted non-detached ticket has exactly one observable "
         "completion");

  std::set<std::uint64_t> retriedTickets;
  for (const auto &profile : rejectedProfiles) {
    const auto retry = fixture.coordinator.submitProfileSettings(
        client, fixture.owner.snapshot(profile), changedSettings(1));
    expect(retry.accepted,
           "taking completions releases delivery admission capacity");
    if (retry.accepted) {
      retriedTickets.insert(retry.ticket);
    }
  }
  fixture.coordinator.poll();
  const auto retriedCompletions =
      fixture.coordinator.takeProfileCompletions(client);
  completedTickets.clear();
  for (const auto &completion : retriedCompletions) {
    completedTickets.insert(completion.ticket);
  }
  expect(completedTickets == retriedTickets &&
             acceptedTickets.size() + retriedTickets.size() == 200,
         "explicitly rejected requests can retry without any completion loss");
  expect(fixture.owner.acknowledgements == 200,
         "bounding delivery never abandons accepted transactions");
}

} // namespace

int main() {
  testIdsTicketsAndProfileCompletionAreNeverReused();
  testStaleGenerationAndSaveFailureRetainTypedOutcomes();
  testDetachCannotRetargetLateProfileCompletion();
  testProfileMutationBarrierDrainsAndResumesByOutcome();
  testProfileMutationBarrierCannotBeFinishedByAnotherCoordinator();
  testFailedActivationRemovalResumesProfileGate();
  testActivationPollingRevalidationDetachAndLeaseRelease();
  testRevalidationRequestsCoalesceLatestSnapshotPerProfile();
  testPausedPollingAndShutdownResolveAcceptedOwnerWorkExactlyOnce();
  testActivationStoreOwnsItsDistinctOwnerAcknowledgement();
  testOffMainShutdownRetainsOwnershipForMainThreadDrain();
  testOffMainShutdownRetainsTerminalActivationDelivery();
#if defined(__APPLE__) || defined(__unix__)
  testOffMainDestructionWithAcceptedWorkTerminates();
  testOffMainDestructionWithTerminalActivationDeliveryTerminates();
#endif
  testCompletionDeliveryIsBounded();

  if (failures != 0) {
    std::cerr << failures << " skin commit coordinator assertion(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
