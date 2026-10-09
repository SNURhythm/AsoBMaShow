// Same source compiled against both application revisions; CPU-only surfaces.
#include "view/DropdownView.h"
#include "view/TextView.h"
#include "view/FontCacheSession.h"
#include "rendering/UniformCache.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width, window_height = design_height;
int render_width = design_width, render_height = design_height;
float widthScale = 1, heightScale = 1, ui_scale_x = 1, ui_scale_y = 1;
int ui_offset_x = 0, ui_offset_y = 0, ui_view_width = design_width, ui_view_height = design_height;
}

using Clock = std::chrono::steady_clock;
std::uint64_t checksum = 0;
struct Measurement { std::string name; int operations; double ns; };
std::vector<Measurement> measurements;
template<class F> void measure(std::string name, int operations, F work) {
  work();
  const auto start = Clock::now();
  work();
  const auto ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  measurements.push_back({std::move(name), operations, ns / operations});
}

class WrapProbe : public TextView {
public:
  using TextView::TextView;
  std::uint64_t layoutChecksum(int width) {
    // Extends the old return value's lifetime or borrows the new cached vector,
    // matching production consumers without adding a copy to the candidate.
    const auto &lines = wrappedTextLines(width);
    std::uint64_t result = lines.size();
    for (const auto &line : lines) result += line.size();
    return result;
  }
  SDL_Surface *raster(int width) {
    int w = 0, h = 0;
    return renderFallbackTextSurface(width, w, h);
  }
};

int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("expected bundled font path");
    if (!SDL_Init(SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 1; init.resolution.height = 1;
    if (!bgfx::init(init)) throw std::runtime_error("bgfx noop initialization failed");
    {
      text_runtime::FontCacheSession session;
      for (const int count : {4, 16, 128}) {
        DropdownView dropdown({});
        DropdownView::State state;
        state.label = "Display settings";
        state.selectedId = "0";
        for (int n = 0; n < count; ++n) {
          state.options.push_back({std::to_string(n), "Output resolution 選択 #" + std::to_string(n)});
        }
        dropdown.refresh(state);
        constexpr int updates = 256;
        measure("dropdown_resize_" + std::to_string(count), updates, [&] {
          for (int i = 0; i < updates; ++i) {
            dropdown.setTriggerWidth(i % 2 ? 300 : 0);
            checksum += dropdown.getWidth();
          }
        });
        measure("dropdown_refresh_" + std::to_string(count), updates, [&] {
          for (int i = 0; i < updates; ++i) {
            dropdown.refresh(state);
            checksum += dropdown.getWidth();
          }
        });
        // Cache-miss control: every option-set change must still be measured.
        measure("dropdown_changed_label_" + std::to_string(count), updates, [&] {
          for (int i = 0; i < updates; ++i) {
            state.options[0].label = i % 2 ? "Changed long label A" : "Changed long label B";
            dropdown.refresh(state);
            checksum += dropdown.getWidth();
          }
        });
      }
      for (const bool mixed : {false, true}) {
        const std::string label = mixed ? "mixed" : "primary";
        WrapProbe view(mixed ? "assets/fonts/fa-solid-900.ttf" : argv[1], 22);
        view.setDeferredTextureMaterialization(true);
        view.setColor({255, 255, 255, 255});
        view.setText(mixed ? "MあM가 MあM가 MあM가 MあM가 MあM가 MあM가"
                           : "AVATAR office gjpqy wrapped text with multiple words");
        constexpr int layouts = 1000, rasters = 100;
        measure("wrap_reused_" + label, layouts, [&] {
          for (int i = 0; i < layouts; ++i) {
            checksum += view.layoutChecksum(240);
          }
        });
        measure("wrap_resized_" + label, layouts, [&] {
          for (int i = 0; i < layouts; ++i) {
            checksum += view.layoutChecksum(i % 2 ? 240 : 280);
          }
        });
        measure("composed_raster_reused_" + label, rasters, [&] {
          for (int i = 0; i < rasters; ++i) {
            auto *surface = view.raster(240);
            if (!surface) throw std::runtime_error(SDL_GetError());
            checksum += surface->w * surface->h;
            SDL_DestroySurface(surface);
          }
        });
      }
    }
    rendering::UniformCache::getInstance().destroyAll();
    bgfx::shutdown();
    SDL_Quit();
    std::cout << "{\"sdl_major\":3,\"checksum\":" << checksum << ",\"metrics\":[";
    for (std::size_t i = 0; i < measurements.size(); ++i) {
      if (i) std::cout << ',';
      const auto &m = measurements[i];
      std::cout << "{\"name\":\"" << m.name << "\",\"operations\":" << m.operations
                << ",\"ns_per_op\":" << m.ns << '}';
    }
    std::cout << "]}\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
