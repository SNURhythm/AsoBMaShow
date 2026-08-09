#include "scene/play/ReplayPlayfieldPresentation.h"

#include <iostream>
#include <string_view>

namespace rendering {
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
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
Camera *main_camera = nullptr;
Camera game_camera{main_view};
void updateUIScale(int renderWidth, int renderHeight) {
  render_width = renderWidth;
  render_height = renderHeight;
}
} // namespace rendering

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TestBga final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t, std::int64_t, const GameplayBgaMissState &) override {
    return {};
  }
  BgaPreflightResult preflight(const PreparedGameplayBgaFrame &,
                               std::span<const BgaDrawTarget>) override {
    return {.ready = true};
  }
  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {}
  void submitPrepared(const PreparedGameplayBgaFrame &,
                      const BgaDrawTarget &) noexcept override {}
  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {}
  void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept override {}
};

ReplayPlayfieldPresentationCreateInfo
createInfo(bms_parser::Chart &chart, const AppSettings &settings,
           const PlayfieldPresentationConfig &configuration, TestBga &bga) {
  return {.chart = chart,
          .timingWindows = {},
          .configuration = configuration,
          .settings = settings,
          .playback = {},
          .bga = bga,
          .skinServices = {.acquire = [](int) {
            return skin::GameplaySkinAcquisition{
                .disposition = skin::GameplaySkinAcquisitionDisposition::BuiltIn};
          }},
          .skinInput = {},
          .recordFailure = {}};
}

void testNoSelectionKeepsOneAdapter() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  const auto created = ReplayPlayfieldPresentation::create(
      createInfo(chart, settings, configuration, bga));
  expect(created.presentation != nullptr && !created.failure,
         "no selected skin creates one coordinator-backed replay adapter");
}

void testSelectedFailureRetainsFactoryDiagnostic() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  AppSettings settings;
  PlayfieldPresentationConfig configuration;
  TestBga bga;
  auto info = createInfo(chart, settings, configuration, bga);
  info.skinServices.acquire = [](int) {
    return skin::GameplaySkinAcquisition{
        .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
        .failure = skin::GameplaySkinAcquisitionFailure{
            .diagnostic = {.code = "skin.test.selected_failure",
                           .message = "selected skin failed",
                           .severity = skin::DiagnosticSeverity::Error}}};
  };
  const auto created = ReplayPlayfieldPresentation::create(std::move(info));
  expect(created.presentation == nullptr && created.failure &&
             created.failure->diagnostic.code == "skin.test.selected_failure",
         "selected-skin factory failure has no replay adapter and retains its diagnostic");
}

} // namespace

int main() {
  if (SDL_Init(SDL_INIT_TIMER) != 0) {
    std::cerr << "FAIL: SDL timer initialization failed: " << SDL_GetError()
              << '\n';
    return 1;
  }
  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless Metal initialization failed\n";
    SDL_Quit();
    return 1;
  }
  rendering::PosColorVertex::init();
  rendering::PosTexVertex::init();
  rendering::PosTexCoord0Vertex::init();
  testNoSelectionKeepsOneAdapter();
  testSelectedFailureRetainsFactoryDiagnostic();
  bgfx::shutdown();
  SDL_Quit();
  if (failures != 0) {
    std::cerr << failures << " replay playfield presentation test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playfield presentation tests passed\n";
  return 0;
}
