#!/usr/bin/env python3
"""Exercise the production bridge on macOS with a simulated UIKit status getter."""
import subprocess
import tempfile
from pathlib import Path

from gameplay_terminal_scene_extract import extract


ROOT = Path(__file__).resolve().parents[1]
PREAMBLE = r'''
#import <Foundation/Foundation.h>
#include <atomic>
#include <cassert>
#include <utility>
namespace platform {
inline bool isMainThread() { return true; }
template <typename F> decltype(auto) onMain(F &&operation) {
  return std::forward<F>(operation)();
}
}
static bool simulatedEnabled = false;
static int logCount = 0;
bool UIAccessibilityIsGuidedAccessEnabled() { return simulatedEnabled; }
NSNotificationName const UIAccessibilityGuidedAccessStatusDidChangeNotification = @"GuidedAccessTest";
void SDL_Log(const char *, ...) { ++logCount; }
'''
TEST = r'''
int main(int argc, char **) {
  @autoreleasepool {
    const bool initial = argc > 1;
    simulatedEnabled = initial;
    assert(IsIOSGuidedAccessEnabled() == initial);
    int previousLogs = logCount;
    assert(IsIOSGuidedAccessEnabled() == initial);
    assert(logCount == previousLogs); // Stable frame polling must not spam logs.

    // Neither transition delivers a notification: locking/unlocking must not
    // be necessary to refresh the session status in either direction.
    simulatedEnabled = !initial;
    assert(IsIOSGuidedAccessEnabled() == !initial);
    simulatedEnabled = initial;
    assert(IsIOSGuidedAccessEnabled() == initial);

    simulatedEnabled = !initial;
    previousLogs = logCount;
    [[NSNotificationCenter defaultCenter]
      postNotificationName:UIAccessibilityGuidedAccessStatusDidChangeNotification object:nil];
    assert(logCount == previousLogs + 1); // The observer still receives changes.
    assert(IsIOSGuidedAccessEnabled() == !initial);
    simulatedEnabled = initial;
    [[NSNotificationCenter defaultCenter]
      postNotificationName:UIAccessibilityGuidedAccessStatusDidChangeNotification object:nil];
    assert(IsIOSGuidedAccessEnabled() == initial);
  }
}
'''


def main():
    method = extract((ROOT / "src/iOSNatives.mm").read_text(),
                     "bool IsIOSGuidedAccessEnabled()")
    with tempfile.TemporaryDirectory(prefix="guided-access-test-") as temporary:
        source = Path(temporary) / "test.mm"
        binary = Path(temporary) / "test"
        source.write_text(PREAMBLE + method + TEST)
        subprocess.run(["xcrun", "--sdk", "macosx", "clang++", "-std=c++20",
                        "-fobjc-arc", "-fblocks", "-framework", "Foundation", "-I", str(ROOT / "src"),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run([str(binary), "initially-enabled"], check=True)
    print("Guided Access initial, polled, and notified status tests passed")


if __name__ == "__main__":
    main()
