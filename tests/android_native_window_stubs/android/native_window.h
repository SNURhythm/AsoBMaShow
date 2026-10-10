#pragma once

struct ANativeWindow;
extern "C" void ANativeWindow_acquire(ANativeWindow *window);
extern "C" void ANativeWindow_release(ANativeWindow *window);
