#include "ApplicationStartup.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace application_startup {
namespace {
constexpr std::string_view kProfileMessage =
    "AsoBMaShow could not initialize the active player profile. The "
    "application will close to protect your data. Check available storage, "
    "storage permissions, and the app version, then try again.";
constexpr std::string_view kDatabasePrefix =
    "AsoBMaShow could not initialize required data: ";
constexpr std::string_view kFailureSuffix =
    ". The application will close to protect your data. Check available "
    "storage, storage permissions, and the app version, then try again.";

std::string databaseFailureMessage(
    const app_database_initializer::DatabaseInitializationStatus &status) {
  const std::array<std::pair<bool, std::string_view>, 4> components{{
      {status.chart, "Chart Library"},
      {status.score, "Scores"},
      {status.replay, "Replays"},
      {status.music, "Music Library"},
  }};
  std::string message{kDatabasePrefix};
  bool first = true;
  for (const auto &[ready, label] : components) {
    if (ready) {
      continue;
    }
    if (!first) {
      message += ", ";
    }
    message += label;
    first = false;
  }
  message += kFailureSuffix;
  return message;
}
} // namespace

int execute(bool profileReady, const Dependencies &dependencies) {
  if (!profileReady) {
    const Result result{
        .failure = Failure::ProfileInitialization,
        .databaseStatus = std::nullopt,
        .userMessage = std::string{kProfileMessage},
    };
    dependencies.reportFatal(result);
    return EXIT_FAILURE;
  }

  const auto status = dependencies.initializeDatabases();
  if (!status.ok()) {
    const Result result{
        .failure = Failure::DatabaseInitialization,
        .databaseStatus = status,
        .userMessage = databaseFailureMessage(status),
    };
    dependencies.reportFatal(result);
    return EXIT_FAILURE;
  }

  if (dependencies.reconcileReplayFiles) {
    try {
      dependencies.reconcileReplayFiles();
    } catch (...) {
      // Replay files are optional playback data. Reconciliation failure must
      // not make independently stored results or IR work unavailable.
    }
  }

  dependencies.runReadyApplication();
  return EXIT_SUCCESS;
}

} // namespace application_startup
