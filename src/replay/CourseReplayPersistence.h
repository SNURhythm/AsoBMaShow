#pragma once

#include "CourseReplayCapture.h"
#include "ReplayFileAssociationCoordinator.h"

#include "../repositories/ReplayRepository.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

enum class CourseReplayPersistenceState {
  SavedWithReplay,
  SavedWithoutReplay,
  Retryable,
  InvalidAttempt,
  IntegrityConflict,
};

struct CourseReplayPersistenceOutcome {
  CourseReplayPersistenceState state = CourseReplayPersistenceState::Retryable;
  std::optional<ModernCourseStageReceipt> receipt;
  bool replayAttached = false;
  std::string diagnostic;

  [[nodiscard]] bool saved() const noexcept {
    return state == CourseReplayPersistenceState::SavedWithReplay ||
           state == CourseReplayPersistenceState::SavedWithoutReplay;
  }
  [[nodiscard]] bool durable() const noexcept { return receipt.has_value(); }
};

struct CourseReplayPersistenceDependencies {
  std::function<ModernCourseResultReadOutcome(std::string_view)> loadResult;
  std::function<std::optional<std::vector<std::byte>>(
      const ReplayCourseDocument &, std::int64_t, std::string &)>
      encode;
  ReplayFileAssociationCoordinatorDependencies fileAssociation;
  std::function<ModernCourseStageOutcome(
      const result_persistence::ModernCourseResult &,
      const std::optional<ModernReplayFileAttachment> &,
      const std::optional<CoursePathInput> &)>
      stage;
};

class CourseReplayPersistence {
public:
  explicit CourseReplayPersistence(ReplayRepository &repository);
  explicit CourseReplayPersistence(
      CourseReplayPersistenceDependencies dependencies);

  [[nodiscard]] CourseReplayPersistenceOutcome
  persist(const CapturedCourseReplayAttempt &attempt);

private:
  CourseReplayPersistenceDependencies dependencies_;
};

} // namespace replay
