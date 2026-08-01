#include "ProfileExportStaging.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (condition) {
    return;
  }
  ++failures;
  std::cerr << "FAIL: " << message << '\n';
}

std::filesystem::path makeRoot(const std::string &name) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("asobmashow-profile-export-staging-test-" + name);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root / "temporary", ignored);
  std::filesystem::create_directories(root / "managed", ignored);
  return std::filesystem::canonical(root);
}

profile_export_staging::Request
requestFor(const std::filesystem::path &root,
           const profile_export_staging::WarningReporter &reporter = {}) {
  return {.temporaryRoot = root / "temporary",
          .managedApplicationRoot = root / "managed",
          .now = std::chrono::system_clock::now(),
          .staleAfter = std::chrono::hours(24),
          .reportWarning = reporter};
}

std::filesystem::file_time_type
fileTimeFromSystem(std::chrono::system_clock::time_point value) {
  return std::filesystem::file_time_type::clock::now() +
         std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
             value - std::chrono::system_clock::now());
}

void testAllocationIsPrivateExactAndLifetimeBound() {
  const auto root = makeRoot("lifetime");
  auto result = profile_export_staging::Create(requestFor(root));
  expect(result.ok(), "secure staging allocation succeeds");
  expect(result.archivePath.filename() == profile_export_staging::kArchiveName,
         "archive uses the exact safe profile filename");
  expect(result.archivePath.parent_path().parent_path() ==
             profile_export_staging::RootUnder(root / "temporary"),
         "issued directory is under the dedicated staging root");
  expect(profile_export_staging::IsIssuedDirectoryName(
             result.archivePath.parent_path().filename().string()),
         "issued directory uses exact lowercase 32-hex grammar");

  const auto issued = result.archivePath.parent_path();
  {
    std::ofstream archive(result.archivePath);
    archive << "profile";
  }
  result.sourceLifetime.reset();
  expect(!std::filesystem::exists(issued),
         "source lifetime removes only its issued directory");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testManagedRootOverlapIsRejected() {
  const auto root = makeRoot("overlap");
  auto request = requestFor(root);
  request.managedApplicationRoot = root / "temporary";
  const auto result = profile_export_staging::Create(request);
  expect(!result.ok() &&
             result.errorMessage.find("overlaps") != std::string::npos,
         "staging cannot overlap managed application data");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testSymlinkParentTraversalUsesCanonicalTemporaryRoot() {
  const auto root = makeRoot("canonical-temporary-root");
  const auto canonicalTemporary = root / "target" / "temporary";
  std::filesystem::create_directories(root / "target" / "nested");
  std::filesystem::create_directories(canonicalTemporary);
  std::error_code linkError;
  std::filesystem::create_directory_symlink(root / "target" / "nested",
                                            root / "redirect", linkError);
  if (!linkError) {
    auto request = requestFor(root);
    request.temporaryRoot = root / "redirect" / ".." / "temporary";
    auto result = profile_export_staging::Create(request);
    expect(
        result.ok(),
        "symlink-parent temporary root resolves to one canonical identity: " +
            result.errorMessage);
    if (result.ok()) {
      expect(result.archivePath.parent_path().parent_path() ==
                 profile_export_staging::RootUnder(
                     std::filesystem::canonical(canonicalTemporary)),
             "returned archive path uses the same canonical temporary root");
      result.sourceLifetime.reset();
    }
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testNonPositiveSweepAgeIsRejected() {
  const auto root = makeRoot("age");
  auto request = requestFor(root);
  request.staleAfter = std::chrono::system_clock::duration::zero();
  const auto result = profile_export_staging::Sweep(request);
  expect(!result.ok() &&
             result.errorMessage.find("positive") != std::string::npos,
         "stale sweep requires a positive age bound");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testSweepNeverRemovesAnActiveLifetime() {
  const auto root = makeRoot("active");
  auto request = requestFor(root);
  auto active = profile_export_staging::Create(request);
  expect(active.ok(), "active staging lifetime is allocated");
  const auto issued = active.archivePath.parent_path();
  const auto now = std::chrono::system_clock::now();
  std::filesystem::last_write_time(
      issued, fileTimeFromSystem(now - std::chrono::hours(48)));
  request.now = now;
  const auto swept = profile_export_staging::Sweep(request);
  expect(swept.ok() && swept.staleDirectoriesRemoved == 0 &&
             std::filesystem::exists(issued),
         "age-bounded sweep skips a directory held by a live source token");
  active.sourceLifetime.reset();
  expect(!std::filesystem::exists(issued),
         "active source token still performs its own cleanup");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testSourceLifetimeNeverRetainsTheWarningReporter() {
  const auto root = makeRoot("reporter-lifetime");
  auto reporterOwner = std::make_shared<int>(42);
  std::weak_ptr<int> weakReporterOwner = reporterOwner;
  auto request = requestFor(root, [reporterOwner](const std::string &) {});
  auto result = profile_export_staging::Create(request);
  expect(result.ok(), "staging exists for reporter lifetime test");
  request.reportWarning = {};
  reporterOwner.reset();
  expect(weakReporterOwner.expired(),
         "detached source lifetime never retains a UI warning callback");
  result.sourceLifetime.reset();

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testStaleSweepIsAgeBoundedAndExact() {
  const auto root = makeRoot("sweep");
  auto initialRequest = requestFor(root);
  auto initial = profile_export_staging::Create(initialRequest);
  expect(initial.ok(), "initial staging root is created");
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  initial.sourceLifetime.reset();

  const auto stale = stagingRoot / std::string(32, 'a');
  const auto recent = stagingRoot / std::string(32, 'b');
  const auto unknown = stagingRoot / "keep-me";
  std::filesystem::create_directory(stale);
  std::filesystem::create_directory(recent);
  std::filesystem::create_directory(unknown);
  std::filesystem::permissions(stale, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(recent, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  std::filesystem::create_directory(stale / "workspace");
  {
    std::ofstream abandoned(stale / "workspace" / "large.tmp");
    abandoned << "abandoned";
  }
  const auto now = std::chrono::system_clock::now();
  std::filesystem::last_write_time(
      stale, fileTimeFromSystem(now - std::chrono::hours(48)));
  std::filesystem::last_write_time(
      recent, fileTimeFromSystem(now - std::chrono::hours(1)));

  auto sweepRequest = requestFor(root);
  sweepRequest.now = now;
  const auto swept = profile_export_staging::Sweep(sweepRequest);
  expect(swept.ok() && swept.staleDirectoriesRemoved == 1,
         "only one exact stale issued directory is swept (removed=" +
             std::to_string(swept.staleDirectoriesRemoved) +
             ", error=" + swept.errorMessage + ")");
  expect(!std::filesystem::exists(stale), "stale issued directory is removed");
  expect(std::filesystem::exists(recent),
         "recent issued directory is retained");
  expect(std::filesystem::exists(unknown), "unknown entry is retained");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testSymlinksAreNeverSweptOrFollowed() {
  const auto root = makeRoot("symlink");
  auto initial = profile_export_staging::Create(requestFor(root));
  expect(initial.ok(), "staging root exists before symlink test");
  initial.sourceLifetime.reset();
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  const auto outside = root / "outside";
  std::filesystem::create_directories(outside);
  {
    std::ofstream marker(outside / "keep.txt");
    marker << "keep";
  }
  const auto link = stagingRoot / std::string(32, 'c');
  std::error_code linkError;
  std::filesystem::create_directory_symlink(outside, link, linkError);
  if (!linkError) {
    std::vector<std::string> warnings;
    auto request = requestFor(
        root, [&](const std::string &warning) { warnings.push_back(warning); });
    request.now += std::chrono::hours(48);
    auto result = profile_export_staging::Sweep(request);
    expect(result.ok(), "unsafe stale entry does not block new allocation: " +
                            result.errorMessage);
    expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
           "exact-name symlink is not swept");
    expect(std::filesystem::exists(outside / "keep.txt"),
           "stale sweep never follows the symlink");
    expect(!warnings.empty(), "unsafe issued entry is reported");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testLifetimeRefusesAReplacedSymlink() {
  const auto root = makeRoot("lease-symlink");
  auto result = profile_export_staging::Create(requestFor(root));
  expect(result.ok(), "staging lease is allocated");
  const auto issued = result.archivePath.parent_path();
  const auto moved = root / "moved-issued";
  const auto outside = root / "outside";
  std::filesystem::create_directories(outside);
  std::error_code error;
  std::filesystem::rename(issued, moved, error);
  if (!error) {
    std::filesystem::create_directory_symlink(outside, issued, error);
  }
  if (!error) {
    result.sourceLifetime.reset();
    expect(std::filesystem::is_symlink(std::filesystem::symlink_status(issued)),
           "lease cleanup refuses a replaced symlink");
    expect(
        std::filesystem::exists(moved),
        "replaced issued path leaves the moved directory for stale recovery");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testPreplantedRootSymlinkIsRejected() {
  const auto root = makeRoot("root-symlink");
  const auto outside = root / "outside";
  std::filesystem::create_directories(outside);
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  std::error_code error;
  std::filesystem::create_directory_symlink(outside, stagingRoot, error);
  if (!error) {
    const auto result = profile_export_staging::Create(requestFor(root));
    expect(!result.ok(), "preplanted staging-root symlink is rejected");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testLifetimeRefusesIntermediateRootReplacement() {
  const auto root = makeRoot("root-replacement");
  auto result = profile_export_staging::Create(requestFor(root));
  expect(result.ok(), "staging lease exists before root replacement");
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  const auto movedRoot = root / "moved-staging-root";
  const auto outside = root / "outside";
  std::filesystem::create_directories(
      outside / result.archivePath.parent_path().filename());
  {
    std::ofstream marker(outside / result.archivePath.parent_path().filename() /
                         "keep.txt");
    marker << "keep";
  }
  std::error_code error;
  std::filesystem::rename(stagingRoot, movedRoot, error);
  if (!error) {
    std::filesystem::create_directory_symlink(outside, stagingRoot, error);
  }
  if (!error) {
    result.sourceLifetime.reset();
    expect(std::filesystem::exists(outside /
                                   result.archivePath.parent_path().filename() /
                                   "keep.txt"),
           "lifetime cleanup never follows a replaced intermediate root");
    expect(!std::filesystem::exists(
               movedRoot / result.archivePath.parent_path().filename()),
           "handle-relative cleanup removes the originally issued directory");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testLifetimeDoesNotDeleteARecreatedNormalRoot() {
  const auto root = makeRoot("normal-root-replacement");
  auto result = profile_export_staging::Create(requestFor(root));
  expect(result.ok(), "staging lease exists before normal root replacement");
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  const auto movedRoot = root / "moved-normal-root";
  const auto issuedName = result.archivePath.parent_path().filename();
  std::error_code error;
  std::filesystem::rename(stagingRoot, movedRoot, error);
  if (!error) {
    std::filesystem::create_directory(stagingRoot, error);
    std::filesystem::permissions(stagingRoot, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    std::filesystem::create_directory(stagingRoot / issuedName, error);
    std::filesystem::permissions(stagingRoot / issuedName,
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    std::ofstream replacement(stagingRoot / issuedName / "keep.txt");
    replacement << "keep";
  }
  if (!error) {
    result.sourceLifetime.reset();
    expect(std::filesystem::exists(stagingRoot / issuedName / "keep.txt"),
           "handle cleanup never deletes a same-path replacement directory");
    expect(!std::filesystem::exists(movedRoot / issuedName),
           "handle cleanup removes the originally issued directory");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

#if !defined(_WIN32)
void testStaleSweepRejectsHardLinkedLeaseWithoutMutation() {
  const auto root = makeRoot("hard-linked-lease");
  auto initial = profile_export_staging::Create(requestFor(root));
  expect(initial.ok(), "staging root exists before hard-link lease test");
  initial.sourceLifetime.reset();
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  const auto issued = stagingRoot / std::string(32, 'e');
  std::filesystem::create_directory(issued);
  std::filesystem::permissions(issued, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  const auto unrelated = root / "unrelated-owner-file";
  {
    std::ofstream file(unrelated);
    file << "keep";
  }
  expect(::chmod(unrelated.c_str(), 0644) == 0,
         "unrelated hard-link target starts with known permissions");
  std::error_code hardLinkError;
  std::filesystem::create_hard_link(unrelated, issued / ".lease",
                                    hardLinkError);
  expect(!hardLinkError, "test creates a hard-linked lease inode");
  if (!hardLinkError) {
    const auto now = std::chrono::system_clock::now();
    std::filesystem::last_write_time(
        issued, fileTimeFromSystem(now - std::chrono::hours(48)));
    std::vector<std::string> warnings;
    auto request = requestFor(
        root, [&](const std::string &warning) { warnings.push_back(warning); });
    request.now = now;
    const auto swept = profile_export_staging::Sweep(request);
    struct stat unrelatedStatus{};
    const bool statSucceeded = ::stat(unrelated.c_str(), &unrelatedStatus) == 0;
    expect(swept.ok() && swept.staleDirectoriesRemoved == 0 &&
               std::filesystem::exists(issued),
           "stale sweep refuses a hard-linked lease");
    expect(statSucceeded && (unrelatedStatus.st_mode & 0777) == 0644,
           "lease validation never changes the unrelated inode permissions");
    expect(!warnings.empty(), "unsafe hard-linked lease is reported");
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

void testStaleSweepHonorsCrossProcessLeaseFile() {
  const auto root = makeRoot("lease-lock");
  auto initial = profile_export_staging::Create(requestFor(root));
  expect(initial.ok(), "staging root exists before lease-lock test");
  initial.sourceLifetime.reset();
  const auto stagingRoot =
      profile_export_staging::RootUnder(root / "temporary");
  const auto issued = stagingRoot / std::string(32, 'd');
  std::filesystem::create_directory(issued);
  std::filesystem::permissions(issued, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  const auto leasePath = issued / ".lease";
  const int lease =
      ::open(leasePath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  expect(lease >= 0 && ::flock(lease, LOCK_EX | LOCK_NB) == 0,
         "test holds an external exclusive staging lease");
  const auto now = std::chrono::system_clock::now();
  std::filesystem::last_write_time(
      issued, fileTimeFromSystem(now - std::chrono::hours(48)));
  auto request = requestFor(root);
  request.now = now;
  const auto whileLocked = profile_export_staging::Sweep(request);
  expect(whileLocked.ok() && whileLocked.staleDirectoriesRemoved == 0 &&
             std::filesystem::exists(issued),
         "stale sweep skips an issued directory locked by another owner");
  if (lease >= 0) {
    ::flock(lease, LOCK_UN);
    ::close(lease);
  }
  const auto afterUnlock = profile_export_staging::Sweep(request);
  expect(afterUnlock.ok() && afterUnlock.staleDirectoriesRemoved == 1 &&
             !std::filesystem::exists(issued),
         "stale sweep removes the directory after its lease is released");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}
#endif
} // namespace

int main() {
  testAllocationIsPrivateExactAndLifetimeBound();
  testManagedRootOverlapIsRejected();
  testSymlinkParentTraversalUsesCanonicalTemporaryRoot();
  testNonPositiveSweepAgeIsRejected();
  testSweepNeverRemovesAnActiveLifetime();
  testSourceLifetimeNeverRetainsTheWarningReporter();
  testStaleSweepIsAgeBoundedAndExact();
  testSymlinksAreNeverSweptOrFollowed();
  testLifetimeRefusesAReplacedSymlink();
  testPreplantedRootSymlinkIsRejected();
  testLifetimeRefusesIntermediateRootReplacement();
  testLifetimeDoesNotDeleteARecreatedNormalRoot();
#if !defined(_WIN32)
  testStaleSweepRejectsHardLinkedLeaseWithoutMutation();
  testStaleSweepHonorsCrossProcessLeaseFile();
#endif
  if (failures != 0) {
    std::cerr << failures << " profile export staging test(s) failed\n";
    return 1;
  }
  std::cout << "profile export staging tests passed\n";
  return 0;
}
