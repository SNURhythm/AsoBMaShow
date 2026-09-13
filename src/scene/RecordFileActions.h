#pragma once

#include "../PlatformDocumentHandoff.h"
#include "../replay/ReplayFileActionService.h"

#include <functional>
#include <optional>
#include <string>

class RecordFileActions {
public:
  struct Feedback {
    std::string message;
    bool reloadRecords = false;
    bool failed = false;
  };

  using DocumentExporter = std::function<
      platform_document_handoff::PlatformDocumentHandoffOperation(
          PlatformDocumentExportRequest)>;

  explicit RecordFileActions(
      ReplayRepository &repository,
      DocumentExporter exporter = platform_document_handoff::ExportDocumentAsync);

  [[nodiscard]] Feedback share(const replay::ReplayFileActionRequest &request);
  [[nodiscard]] Feedback remove(const replay::ReplayFileActionRequest &request);
  [[nodiscard]] std::optional<Feedback> poll();
  [[nodiscard]] bool active() const noexcept;
  void close() noexcept;

private:
  ReplayRepository &repository_;
  DocumentExporter exporter_;
  platform_document_handoff::PlatformDocumentHandoffOperation handoff_;
};
