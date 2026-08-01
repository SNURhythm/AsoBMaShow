# Judgement Timing Millisecond Display Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Display gameplay FAST/SLOW timing magnitudes with exactly two decimal places, ceiled to the next hundredth of a millisecond.

**Architecture:** Put the exact integer conversion and string formatting in a header-only gameplay HUD helper so it can be tested without constructing `BMSRenderer`. The renderer will retain all existing visibility, direction, color, playback-rate, and linger behavior and delegate only the millisecond label.

**Tech Stack:** C++23, CMake, the existing standalone gameplay timing test target.

## Global Constraints

- FAST and SLOW format the absolute timing magnitude identically.
- Exact hundredths remain unchanged; any smaller nonzero remainder rounds upward.
- The result always has exactly two fractional digits and the existing `ms` suffix.
- Every `long long` input, including the minimum signed value, must be handled without overflow.
- Zero formatting is `0.00ms`, while the renderer continues suppressing zero timing differences.
- FAST/SLOW direction rules, criteria, colors, lingering, timing calculations, replay precision, and result counters remain unchanged.

---

### Task 1: Exact Judgement Timing Label

**Files:**
- Create: `src/scene/play/JudgementTimingText.h`
- Modify: `tests/gameplay_practice_boundary_tests.cpp:1-68`
- Modify: `src/scene/play/BMSRenderer.cpp:5-7,2816-2824`

**Interfaces:**
- Consumes: signed real-time judgement difference in microseconds after playback-rate conversion.
- Produces: `gameplay_timing::formatJudgementTimingMilliseconds(long long diffMicros) -> std::string`.

- [ ] **Step 1: Write the failing formatter tests**

Add the formatter include and numeric-limit support to
`tests/gameplay_practice_boundary_tests.cpp`:

```cpp
#include "scene/play/GamePlayTiming.h"
#include "scene/play/JudgementTimingText.h"

#include <iostream>
#include <limits>
#include <vector>
```

Add this helper inside the anonymous namespace:

```cpp
bool expectTimingText(long long diffMicros, const std::string &expected,
                      const char *message) {
  return expect(gameplay_timing::formatJudgementTimingMilliseconds(diffMicros) ==
                    expected,
                message);
}
```

At the beginning of `main`, add exact-boundary, ceiling, direction, zero, and
signed-minimum checks:

```cpp
  if (!expectTimingText(12'340, "12.34ms",
                        "exact hundredths remain unchanged") ||
      !expectTimingText(12'341, "12.35ms",
                        "partial hundredths round upward") ||
      !expectTimingText(-12'341, "12.35ms",
                        "FAST timing uses the absolute magnitude") ||
      !expectTimingText(1, "0.01ms",
                        "sub-hundredth timing rounds upward") ||
      !expectTimingText(0, "0.00ms", "zero retains two decimal places") ||
      !expectTimingText(std::numeric_limits<long long>::min(),
                        "9223372036854775.81ms",
                        "minimum signed timing formats without overflow")) {
    return 1;
  }
```

- [ ] **Step 2: Run the target to verify the test fails**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_practice_boundary_tests -j 6
```

Expected: compilation fails because
`scene/play/JudgementTimingText.h` does not exist.

- [ ] **Step 3: Implement the integer-only formatter**

Create `src/scene/play/JudgementTimingText.h`:

```cpp
#pragma once

#include <string>

namespace gameplay_timing {

inline std::string formatJudgementTimingMilliseconds(long long diffMicros) {
  const auto unsignedDiff = static_cast<unsigned long long>(diffMicros);
  const unsigned long long magnitudeMicros =
      diffMicros < 0 ? 0ULL - unsignedDiff : unsignedDiff;
  const unsigned long long hundredths =
      magnitudeMicros / 10ULL + (magnitudeMicros % 10ULL != 0ULL ? 1ULL : 0ULL);
  const unsigned long long wholeMilliseconds = hundredths / 100ULL;
  const unsigned long long fractionalHundredths = hundredths % 100ULL;

  return std::to_string(wholeMilliseconds) + "." +
         (fractionalHundredths < 10ULL ? "0" : "") +
         std::to_string(fractionalHundredths) + "ms";
}

} // namespace gameplay_timing
```

This avoids signed absolute-value overflow and avoids the addition-before-
division overflow that `(magnitude + 9) / 10` would cause at the maximum
magnitude.

- [ ] **Step 4: Run the focused test to verify the formatter passes**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_practice_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_practice_boundary_tests$' --output-on-failure
```

Expected: the target builds and the selected test reports `100% tests passed`.

- [ ] **Step 5: Route renderer timing text through the formatter**

Add the focused helper include beside `GamePlayTiming.h` in
`src/scene/play/BMSRenderer.cpp`:

```cpp
#include "GamePlayTiming.h"
#include "JudgementTimingText.h"
#include "TouchVisualizationTiming.h"
```

Replace the integer-millisecond rounding block with:

```cpp
  if (judgementTimingMsText != nullptr) {
    if (refreshedTimingText || !keepLingeringTimingText) {
      judgementTimingMsText->setVisible(showTimingMs);
      judgementTimingMsText->setText(
          showTimingMs
              ? gameplay_timing::formatJudgementTimingMilliseconds(diffMicros)
              : "");
      judgementTimingMsText->setColor(ui_theme::sdl(timingColor));
    }
  }
```

- [ ] **Step 6: Verify the focused behavior and desktop build**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_practice_boundary_tests main -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_practice_boundary_tests$' --output-on-failure
git diff --check
```

Expected: both targets build, the selected test reports `100% tests passed`,
and `git diff --check` exits successfully without output.

- [ ] **Step 7: Commit the implementation**

```bash
git add src/scene/play/JudgementTimingText.h \
  src/scene/play/BMSRenderer.cpp \
  tests/gameplay_practice_boundary_tests.cpp
git commit -m "feat(gameplay): show ceiled timing hundredths"
```
