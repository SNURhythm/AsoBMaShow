#include "bgfx_helper.h"

#include <cassert>
#include <type_traits>

namespace {
int nativeWindow, nativeView, nativeLayer;
int replacementView, replacementLayer;
int creates = 0, destroys = 0;
bool failCreate = false, failLayer = false;
bool replacement = false;
}

extern "C" SDL_MetalView SDLCALL SDL_Metal_CreateView(SDL_Window *window) {
  assert(window == reinterpret_cast<SDL_Window *>(&nativeWindow));
  if (failCreate) return nullptr;
  ++creates;
  return replacement ? &replacementView : &nativeView;
}

extern "C" void *SDLCALL SDL_Metal_GetLayer(SDL_MetalView view) {
  assert(view == &nativeView || view == &replacementView);
  return failLayer ? nullptr : view == &nativeView ? &nativeLayer : &replacementLayer;
}

extern "C" void SDLCALL SDL_Metal_DestroyView(SDL_MetalView view) {
  assert(view == &nativeView || view == &replacementView);
  ++destroys;
}

int main() {
  static_assert(!std::is_copy_constructible_v<SdlMetalViewOwner>);
  auto *window = reinterpret_cast<SDL_Window *>(&nativeWindow);
  bgfx::PlatformData data{};
  {
    SdlMetalViewOwner owner;
    assert(setup_bgfx_platform_data(data, window, owner));
    assert(data.nwh == &nativeLayer && owner.get() == &nativeView);
    assert(creates == 1 && destroys == 0);
    // The application resets ownership after bgfx shutdown, before the window.
    owner.reset();
    assert(destroys == 1);
  }
  assert(destroys == 1);
  {
    SdlMetalViewOwner owner;
    assert(setup_bgfx_platform_data(data, window, owner));
  }
  assert(creates == 2 && destroys == 2);
  {
    SdlMetalViewOwner owner;
    failLayer = true;
    assert(!setup_bgfx_platform_data(data, window, owner));
    assert(!owner && data.nwh == nullptr);
    assert(creates == 3 && destroys == 3);
    failLayer = false;
    failCreate = true;
    assert(!setup_bgfx_platform_data(data, window, owner));
    assert(!owner && data.nwh == nullptr);
    assert(creates == 3 && destroys == 3);
  }
  {
    failCreate = false;
    SdlMetalViewOwner owner, retired;
    assert(setup_bgfx_platform_data(data, window, owner));
    const auto before = destroys;
    replacement = true;
    assert(replace_bgfx_metal_view(data, window, owner, retired));
    assert(owner.get() == &replacementView && retired.get() == &nativeView);
    assert(data.nwh == &replacementLayer && destroys == before);
    // Renderer consumes the new handle before the old native view is released.
    retired.reset();
    assert(destroys == before + 1);
    failCreate = true;
    assert(!replace_bgfx_metal_view(data, window, owner, retired));
    assert(owner.get() == &replacementView && !retired);
    assert(data.nwh == &replacementLayer && destroys == before + 1);
  }
}
