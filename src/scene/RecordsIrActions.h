#pragma once

#include "../ir/IrOutboxModels.h"
#include "../ir/IrSubmissionService.h"

#include <optional>
#include <string>
#include <string_view>

class ApplicationContext;

namespace replay_records {

[[nodiscard]] ir::IrRecordActivity
recordActivity(ir::IrActiveRequestKind request) noexcept;
[[nodiscard]] std::string_view irStatusFeedback(ir::IrRecordState state) noexcept;
[[nodiscard]] std::optional<std::string>
irUploadUnavailable(const ApplicationContext &context);
[[nodiscard]] std::string uploadSavedResult(ApplicationContext &context,
                                           std::string_view attemptId);

} // namespace replay_records
