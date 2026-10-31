#include "platform/IOSDrawableSize.h"

#include <cstdlib>
#include <iostream>

using ios_drawable::Size;

static void expect(Size actual, Size expected) {
  if (actual.width != expected.width || actual.height != expected.height) {
    std::cerr << "Expected " << expected.width << 'x' << expected.height
              << ", got " << actual.width << 'x' << actual.height << '\n';
    std::exit(1);
  }
}

int main() {
  using ios_drawable::fullscreenNativeSize;
  // Recorded iPad More Space geometry: mode is larger than the physical panel.
  expect(fullscreenNativeSize({1940, 2816}, {970, 1408}, {970, 1408},
                             {1668, 2420}, true), {1668, 2420});
  expect(fullscreenNativeSize({2816, 1940}, {1408, 970}, {1408, 970},
                             {1668, 2420}, true), {2420, 1668});
  // Fractional nativeScale multiplication must not leave a one-pixel mismatch.
  expect(fullscreenNativeSize({1668, 2421}, {970, 1408}, {970, 1408},
                             {1668, 2420}, true), {1668, 2420});
  // The same policy also repairs a smaller drawable, without changing UI points.
  expect(fullscreenNativeSize({1000, 1451}, {970, 1408}, {970, 1408},
                             {1668, 2420}, true), {1668, 2420});
  expect(fullscreenNativeSize({1206, 2622}, {402, 874}, {402, 874},
                             {1206, 2622}, true), {1206, 2622});
  // A partial window or a stale resize event must retain SDL's chosen size.
  expect(fullscreenNativeSize({1000, 1600}, {500, 800}, {500, 800},
                             {1668, 2420}, false), {1000, 1600});
  expect(fullscreenNativeSize({1940, 2816}, {970, 1408}, {1408, 970},
                             {1668, 2420}, true), {1940, 2816});
  expect(fullscreenNativeSize({1000, 1000}, {500, 500}, {500, 500},
                             {1668, 2420}, true), {1000, 1000});
  expect(fullscreenNativeSize({1940, 2816}, {970, 1408}, {970, 1408},
                             {0, 2420}, true), {1940, 2816});
  std::cout << "iOS fullscreen native drawable sizing passed\n";
}
