#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/ContextMenuView.h"
#include "../src/view/OverlayPortal.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "../src/rendering/UniformCache.h"

#include <cassert>
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

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));

  OverlayPortal portal(0, 0, 800, 600);
  std::vector<bool> openChanges;
  std::vector<std::string> selections;
  auto *identity = static_cast<ContextMenuView *>(nullptr);
  {
    ContextMenuView menu(
        &portal,
        {.onOpenChanged =
             [&](bool open) { openChanges.push_back(open); },
         .onActionSelected = [&](const std::string &id) {
           selections.push_back(id);
         }});
    identity = &menu;
    menu.setViewportSize(800, 600);
    menu.show({.x = 700, .y = 520, .width = 90, .height = 58},
              {{.id = "folder", .label = "Show Same Folder"},
               {.id = "reveal", .label = "Reveal File"},
               {.id = "disabled", .label = "Disabled", .enabled = false}},
              210);
    assert(menu.isOpen());
    assert(portal.isPresented(&menu));
    assert(openChanges == std::vector<bool>{true});
    assert(menu.panel->getX() + menu.panel->getWidth() <= 790);
    assert(menu.panel->getY() < 520);

    menu.dispatchAction("disabled");
    assert(selections.empty());
    assert(menu.isOpen());

    menu.dispatchAction("folder");
    assert(selections == std::vector<std::string>{"folder"});
    assert(!menu.isOpen());
    assert(!portal.isPresented(&menu));

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    SDL_Event escape{};
    escape.type = SDL_KEYDOWN;
    escape.key.repeat = 0;
    escape.key.keysym.sym = SDLK_ESCAPE;
    assert(!menu.handleEventsImpl(escape));
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    SDL_Event back{};
    back.type = SDL_KEYDOWN;
    back.key.repeat = 0;
    back.key.keysym.sym = SDLK_AC_BACK;
    assert(!menu.handleEventsImpl(back));
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    menu.handlePointerDown(120.0F, 120.0F);
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    menu.handlePointerDown(20.0F, 20.0F);
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    assert(portal.isPresented(&menu));
    menu.dismiss();

    selections.clear();
    menu.setViewportSize(240, 180);
    menu.show({.x = 80, .y = 70, .width = 80, .height = 30},
              {{.id = "action-0", .label = "Action 0"},
               {.id = "action-1", .label = "Action 1"},
               {.id = "action-2", .label = "Action 2"},
               {.id = "action-3", .label = "Action 3"},
               {.id = "action-4", .label = "Action 4"},
               {.id = "action-5", .label = "Action 5"}},
              180);
    assert(menu.panel->getY() + menu.panel->getHeight() <= 170);

    const auto sendFinger = [&](Uint32 type, SDL_FingerID fingerId, float x,
                                float y) {
      SDL_Event event{};
      event.type = type;
      event.tfinger.fingerId = fingerId;
      event.tfinger.x = x / static_cast<float>(rendering::window_width);
      event.tfinger.y = y / static_cast<float>(rendering::window_height);
      assert(!menu.handleEvents(event));
    };
    for (SDL_FingerID fingerId = 1; fingerId <= 5; ++fingerId) {
      sendFinger(SDL_FINGERDOWN, fingerId, 100.0F, 160.0F);
      sendFinger(SDL_FINGERMOTION, fingerId, 100.0F, 110.0F);
      sendFinger(SDL_FINGERUP, fingerId, 100.0F, 110.0F);
    }
    sendFinger(SDL_FINGERDOWN, 6, 100.0F, 144.0F);
    sendFinger(SDL_FINGERUP, 6, 100.0F, 144.0F);
    assert(selections == std::vector<std::string>{"action-5"});
    assert(!menu.isOpen());
  }
  assert(!portal.isPresented(identity));
  assert(openChanges.back() == false);
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}
