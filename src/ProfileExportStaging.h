#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace profile_export_staging {

inline constexpr std::string_view kRootName =
    "AsoBMaShow-profile-export-staging";
inline constexpr std::string_view kArchiveName =
    "AsoBMaShow-profile.asobprofile";

using WarningReporter = std::function<void(const std::string &)>;

struct Request {
  std::filesystem::path temporaryRoot;
  std::filesystem::path managedApplicationRoot;
  std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
  std::chrono::system_clock::duration staleAfter = std::chrono::hours(24);
  // Invoked synchronously by Create/Sweep only. It is never retained by the
  // sourceLifetime object or called from detached platform worker teardown.
  WarningReporter reportWarning;
};

struct Result {
  std::filesystem::path archivePath;
  std::shared_ptr<void> sourceLifetime;
  std::string errorMessage;
  std::size_t staleDirectoriesRemoved = 0;

  [[nodiscard]] bool ok() const noexcept {
    return !archivePath.empty() && sourceLifetime != nullptr &&
           errorMessage.empty();
  }
};

struct SweepResult {
  std::string errorMessage;
  std::size_t staleDirectoriesRemoved = 0;

  [[nodiscard]] bool ok() const noexcept { return errorMessage.empty(); }
};

[[nodiscard]] std::filesystem::path
RootUnder(const std::filesystem::path &temporaryRoot);
[[nodiscard]] bool IsIssuedDirectoryName(std::string_view name) noexcept;

// Performs the same fail-closed root validation as Create(), then removes
// only exact issued directories older than staleAfter. This is safe to call
// during application startup even when no staging root exists yet.
[[nodiscard]] SweepResult Sweep(const Request &request);

// Allocates an owner-private, exact-shaped staging directory outside managed
// application data. The returned opaque owner removes only that issued
// directory and may safely be retained by detached platform export work.
// Cleanup is identity-bound, but archivePath is necessarily path-based: its
// consumer is expected to be another cooperative application process. An
// untrusted process running as the same OS user could replace a path component
// before a path-only consumer opens it.
[[nodiscard]] Result Create(const Request &request);

} // namespace profile_export_staging
