// Keep the real SDL host and dispatcher, with a barrier at the viewport read
// so event/snapshot publication ordering is deterministic under test.
#include <SDL3/SDL.h>
bool runtimeTestGetWindowSize(SDL_Window *, int *, int *);
#define SDL_GetWindowSize runtimeTestGetWindowSize
#include "../src/platform/SDLApplicationRuntime.cpp"
