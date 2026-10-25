# Source syntax performance — 2026-10-24

The BMS source viewer has an application-local scanner in `src/view/BmsSyntax.cpp`.
`bms-parser-cpp`, its amalgamation and normal chart parse/scan calls are unchanged.
The only production call to `bms_syntax::tokenize` is in
`SelectableDocumentView::open`, after a native surface has been created.

The scanner emits compact UTF-16 spans without copied token strings or regex.
A channel payload is one span, independent of its number of two-character cells.
Scrolling does not tokenize again: native controls binary-search cached spans
and retain at most 16,384 styled UTF-16 units and 4,096 foreground runs. Retired
foreground attributes are removed/reset, preserving native selection and
practice background attributes. Android transfers span triples once and releases
the native copy; Windows maps CRLF offsets once on the worker. Windows paints
disjoint visible row slices under a shared budget, so a long unwrapped row does
not consume the budget with its hidden tail and leave shorter visible rows plain.
Empty `00` data cells are faint during viewport painting (dark RGB 55/63/77;
light RGB 218/222/228). Adjacent empty cells
and adjacent populated cells each coalesce into one run. This scans only the
bounded visible data (plus one character at each clipped edge), keeps the global
payload token intact, and shares the 4,096-run limit with other syntax colors.
Cell alignment prevents accidentally dimming the `00` across a `10`/`01` boundary.

## Local measurements

Apple M1 Pro. These are individual local samples, not timing assertions or
cross-platform performance guarantees. Scanner code was compiled with `-O2`;
the AppKit test used the existing debug build and a hidden SDL window.

| Scanner fixture | Bytes | Cached spans | Span vector capacity | Tokenize |
| --- | ---: | ---: | ---: | ---: |
| One 64 MiB channel payload | 67,108,871 | 3 | 49,152 bytes | 58.97 ms |
| 16 MiB mixed headers/resources/data, including Korean/emoji/CRLF | 16,777,282 | 1,636,808 | 25,165,824 bytes | 38.52 ms |

Ten thousand scattered viewport lookups took 0.06 ms for the long payload and
10.62 ms for mixed text. The mixed fixture averaged 426.8 spans per paint window.
Storage scales with syntax runs/lines, rather than note-cell count; dense
short-line input intentionally exercises the less favorable storage case.

The native AppKit fixture contained 4,194,375 bytes and 447,400 cached spans.
Attaching syntax initially took 47.85 ms including main-thread dispatch and initial visible
layout, retaining **200 foreground runs over 1,700 UTF-16 units**. With grouped
empty-cell dimming added, another run took 49.96 ms and retained **563 runs over
the same 1,700 units** (control creation: 34.54 ms). It did not
install hundreds of thousands of native attributes.

The same native benchmark exposed repeated layout during initial text setup.
Installing source only after the final frame and font reduced control creation
from 5,628.49 ms to 34.43 ms in the observed runs. This avoids loading a large
document into a zero-width text control and then reflowing it for frame/font
changes. Whole-document native text storage and layout still have their own
costs; the viewport bound applies to added syntax formatting.

## Reproduce

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Werror -I src \
  tests/bms_syntax_tests.cpp src/view/BmsSyntax.cpp \
  -o /tmp/asobmashow-bms-syntax-benchmark
/tmp/asobmashow-bms-syntax-benchmark --benchmark

cmake --build cmake-build-debug --target native_document_apple_tests -j 6
cmake-build-debug/native_document_apple_tests --benchmark
```

Normal tests use small boundary fixtures; the larger timing fixtures are opt-in.
The AppKit test also checks that distant scrolling retires old colors, returning
restores them, theme/resize changes work, practice backgrounds remain independent,
native copying preserves Unicode, and a wrapped range jumps directly into view.

Android JNI and Java compilation plus Java/C++ paint-window equivalence checks
passed locally. UIKit device and simulator compile checks passed; the native
simulator regression below also passed. Physical iOS gestures and rendering
performance still need device runtime checks. Windows native tests
require the Windows SDK/runtime unavailable on this machine; they are registered
but were not compiled or run locally. Isolated Windows row-planning checks passed
with sanitizers; native tests also cover mixed long/short rows and horizontal
scrolling, but those assertions still need execution on Windows.

## Android device validation

A signed restricted-file release build was installed over the existing app on
a Galaxy S20 FE (SM-G781N), Android 13, preserving its library. Startup selected
the Vulkan renderer. The existing 81,382-byte, 3,246-line chart exercised the
complete viewer/JNI path: syntax colors, wrapped data, touch scrolling,
long-press selection and copy menu, practice highlighting with a direct jump,
Close/reopen, and Back dismissing selection before closing the source. Sending
the app to Home and bringing it back preserved the viewer, scroll and selection.
No crash, ANR or native text error appeared in the app's runtime log.

The device run caught an initial-position bug: Android's movement method moved
the caret to EOF when focus arrived before layout, causing the source to open at
the bottom. Explicitly placing the initial caret at zero after requesting focus
fixes this without affecting later selection or practice navigation.

The `native-document` instrumentation mode reproduced the bug before the fix
(`Source opened at y=811481`) and passed afterwards. Its in-memory fixture has
1,048,599 UTF-16 units and 29,538 cached spans. Open, initial layout and coloring
took 909 ms in one device run; this includes native text layout and dispatch,
not just syntax painting. It also verified Unicode clipboard contents, immediate
distant practice focus, preserved selection/backgrounds, retired foreground
colors, the 16,384-character/4,096-span limits, theme changes and resizing.
The fixture adds no library files. See `android/README.md` for the command.

After adding empty-cell dimming, the device fixture was expanded to include
grouped data cells on separate short lines (1,048,616 UTF-16 units, 34,194 cached
spans). Native assertions verify a six-zero stretch shares one foreground span
and neighboring populated cells retain their color. This expanded fixture took
3,431–3,598 ms to open/layout/color; the identical fixture with data splitting
disabled took 3,526 ms. These samples show no measurable startup increase from
empty-cell coloring, while illustrating the native layout cost of more short
lines. They are not directly comparable with the earlier header-only fixture.

## Highlight focus after scrolling and early layout

The UIKit simulator probe reproduced a missed jump after manually changing the
native scroll position: the viewport was at y=133,788 while the target line was
at y=105,520. UIKit's noncontiguous layout estimates changed as text was laid out.
The backend now initializes TextKit 1 explicitly and uses contiguous geometry for
absolute jumps. Syntax updates from `layoutSubviews` are coalesced onto the main
queue after layout instead of mutating text storage inside that callback. Focus
requested with a zero-size viewport is retained until the view can lay it out.
A weak view reference and detached callback allow safe close before queued work.

`scripts/ios_native_document_test.sh` exercises the production backend on an
already booted simulator. Its wrapped-data fixture verifies early focus, manual
and animated scrolling across display frames, resize, independent selection and
backgrounds, bounded foreground runs, and queued close. It passed on the iOS 27.1
simulator. The exact reported early-load crash was **not** reproduced locally;
these checks validate the corrected layout paths, not a captured crash signature.
Contiguous native layout has document-size-dependent costs; the existing bounds
still apply to syntax attributes, not UIKit's own text layout.

The Android device regression separately reproduced a live fling overriding an
explicit jump (`1543 != 981027`). A zero-velocity fling plus consuming its final
frame retires that animation before positioning. Early focus also waits until
after the initial traversal so TextView's caret reveal cannot override it.
The instrumentation passed on the connected Galaxy S20 FE with these cases
in addition to its existing 1 MiB fixture (open/layout/color: 3,448 ms). AppKit's manual-scroll/refocus regression passes; the equivalent Windows
assertion is added but remains unexecuted without a Windows SDK/runtime.
