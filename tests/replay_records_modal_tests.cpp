#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/ReplayRecordsModal.h"
#include "view/View.h"
#include "ReplayVideoExporter.h"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ResultRecordSummary modernChartRecord() {
  ModernChartResultRecord modern{
      .result = {.resultId = 19, .attemptId = "selector-modal-attempt"}};
  return {
      .identity = ModernChartRecordId{.attemptId = modern.result.attemptId},
      .capabilities = {.watch = true, .videoExport = true},
      .modern = std::move(modern),
  };
}

void testSelectedModernRecordDispatchesWatchAndExport() {
  ChartMetaRecord chart;
  chart.meta.Title = "Selected chart";
  const auto summary = modernChartRecord();
  std::string watched;
  std::string exported;
  const ReplayRecordsModalCallbacks callbacks{
      .watchModernChart = [&](const ChartMetaRecord &record,
                              const ModernChartResultRecord &modern) {
        watched = record.meta.Title + ":" + modern.result.attemptId;
      },
      .exportModernChart = [&](const ChartMetaRecord &record,
                               const ModernChartResultRecord &modern,
                               ReplayVideoExportOptions) {
        exported = record.meta.Title + ":" + modern.result.attemptId;
      },
  };

  expect(ReplayRecordsModal::dispatchAction(ReplayRecordsModalAction::Watch,
                                             chart, summary, callbacks) &&
             watched == "Selected chart:selector-modal-attempt",
         "selected modern record requests replay watch through its owner");
  expect(ReplayRecordsModal::dispatchAction(
             ReplayRecordsModalAction::VideoExport, chart, summary,
             callbacks) &&
             exported == "Selected chart:selector-modal-attempt",
         "selected modern record requests video export through its owner");
}

void testNonModernRecordCannotCrossTheActionBoundary() {
  auto summary = modernChartRecord();
  summary.identity = LegacyChartRecordId{.legacyReplayId = 19};
  ChartMetaRecord chart;
  bool called = false;
  const ReplayRecordsModalCallbacks callbacks{
      .watchModernChart = [&](const ChartMetaRecord &,
                              const ModernChartResultRecord &) {
        called = true;
      },
  };
  expect(!ReplayRecordsModal::dispatchAction(ReplayRecordsModalAction::Watch,
                                              chart, summary, callbacks) &&
             !called,
         "forged legacy identity cannot dispatch a modern selector replay");
}

void testRetainedModalActivatesSelectedRecordThroughOwner() {
  View parent;
  ChartMetaRecord chart;
  chart.meta.Title = "Retained chart";
  const auto summary = modernChartRecord();
  std::string watched;
  std::string exported;
  const ReplayRecordsModalCallbacks callbacks{
      .loadRecords =
          [&](const ChartMetaRecord &) {
            return std::vector<ResultRecordSummary>{summary};
          },
      .watchModernChart = [&](const ChartMetaRecord &record,
                              const ModernChartResultRecord &modern) {
        watched = record.meta.Title + ":" + modern.result.attemptId;
      },
      .exportModernChart = [&](const ChartMetaRecord &record,
                               const ModernChartResultRecord &modern,
                               ReplayVideoExportOptions) {
        exported = record.meta.Title + ":" + modern.result.attemptId;
      },
  };

  auto modal = ReplayRecordsModal::Create(&parent, callbacks);
  expect(modal != nullptr, "shared records modal is created in the parent view");
  modal->showChart(chart);
  expect(modal->isVisible(), "showChart presents the retained records modal");
  modal->selectRecord(summary);
  expect(modal->activate(ReplayRecordsModalAction::Watch) &&
             watched == "Retained chart:selector-modal-attempt",
         "selected modern record requests replay watch through its owner");
  expect(modal->activate(ReplayRecordsModalAction::VideoExport) &&
             exported == "Retained chart:selector-modal-attempt",
         "selected modern record requests video export through its owner");
  modal->hide();
  expect(!modal->isVisible(), "hide dismisses the retained records modal");
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless bgfx did not initialize\n";
    return 1;
  }
  testSelectedModernRecordDispatchesWatchAndExport();
  testNonModernRecordCannotCrossTheActionBoundary();
  testRetainedModalActivatesSelectedRecordThroughOwner();
  bgfx::shutdown();
  return failures == 0 ? 0 : 1;
}
