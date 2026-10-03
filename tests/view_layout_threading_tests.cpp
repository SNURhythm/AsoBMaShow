#include "../src/view/View.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <thread>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
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

class LayoutView : public View {
public:
  int layoutCalls = 0;
  bool wrongThread = false;

  void onLayout() override {
    ++layoutCalls;
    wrongThread |= std::this_thread::get_id() != owner;
  }

private:
  const std::thread::id owner = std::this_thread::get_id();
};

void testIndependentBatchesAndDestruction() {
  LayoutView uiRoot;
  uiRoot.setWidth(20);
  const int initialCalls = uiRoot.layoutCalls;
  int immediateExportWidth = 0;
  int batchedExportWidth = 0;
  bool uiStayedDeferred = false;
  {
    View::LayoutBatchScope uiBatch;
    uiRoot.setWidth(80);
    // The UI is idle while export runs: this exposes shared batching state
    // deterministically without depending on a hash-table race to crash.
    std::thread exporter([&] {
      LayoutView exportRoot;
      exportRoot.setWidth(40);
      immediateExportWidth = exportRoot.getWidth();
      {
        View::LayoutBatchScope exportBatch;
        exportRoot.setWidth(60);
        exportRoot.addView(new View());
        exportRoot.clearChildren();
      }
      batchedExportWidth = exportRoot.getWidth();
    });
    exporter.join();
    uiStayedDeferred = uiRoot.layoutCalls == initialCalls;
  }
  expect(immediateExportWidth == 40,
         "an active UI batch must not defer an export tree's layout");
  expect(batchedExportWidth == 60,
         "an export batch must flush before the UI batch finishes");
  expect(uiStayedDeferred && !uiRoot.wrongThread,
         "export flushing and destruction must not consume pending UI roots");
  expect(uiRoot.getWidth() == 80,
         "the UI must retain its pending layout until its own batch finishes");
}

void testIndependentLayoutCallbacks() {
  class ExportDuringLayout final : public View {
  public:
    bool exportLaidOut = false;
    void onLayout() override {
      std::thread exporter([&] {
        LayoutView exportRoot;
        exportRoot.setWidth(70);
        exportLaidOut = exportRoot.getWidth() == 70 &&
                        exportRoot.layoutCalls > 0;
      });
      exporter.join();
    }
  } uiRoot;
  uiRoot.setWidth(100);
  expect(uiRoot.exportLaidOut,
         "a UI layout callback must not suppress export layout callbacks");
}

void testConcurrentExportCleanupAndUiLayout() {
  std::barrier start(2);
  std::atomic<bool> correct = true;
  auto exercise = [&] {
    start.arrive_and_wait();
    for (int iteration = 0; iteration < 1000; ++iteration) {
      LayoutView root;
      {
        View::LayoutBatchScope batch;
        root.setWidth(100);
        root.setHeight(50);
        root.addView(new View(0, 0, 10, 10));
        root.addView(new View(0, 0, 20, 20));
        root.clearChildren();
        root.addView(new View(0, 0, 30, 30));
      }
      if (root.getWidth() != 100 || root.getHeight() != 50 ||
          root.wrongThread) {
        correct = false;
      }
    }
  };
  std::thread exporter(exercise);
  exercise();
  exporter.join();
  expect(correct, "concurrent independent trees must lay out and clean up safely");
}
} // namespace

int main() {
  testIndependentBatchesAndDestruction();
  testIndependentLayoutCallbacks();
  if (failures == 0) testConcurrentExportCleanupAndUiLayout();
  return failures == 0 ? 0 : 1;
}
