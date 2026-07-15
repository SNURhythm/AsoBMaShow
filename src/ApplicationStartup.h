#pragma once

#include "AppDatabaseInitializer.h"

#include <functional>
#include <optional>
#include <string>

namespace application_startup {

enum class Failure {
  None,
  ProfileInitialization,
  DatabaseInitialization,
};

struct Result {
  Failure failure = Failure::None;
  std::optional<
      app_database_initializer::DatabaseInitializationStatus> databaseStatus;
  std::string userMessage;

  [[nodiscard]] bool ok() const noexcept {
    return failure == Failure::None;
  }
};

struct Dependencies {
  std::function<app_database_initializer::DatabaseInitializationStatus()>
      initializeDatabases;
  std::function<void(const Result &)> reportFatal;
  std::function<void()> runReadyApplication;
};

int execute(bool profileReady, const Dependencies &dependencies);

} // namespace application_startup
