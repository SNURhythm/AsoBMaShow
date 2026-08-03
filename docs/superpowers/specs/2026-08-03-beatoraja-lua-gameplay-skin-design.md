# Beatoraja Lua Gameplay Skin Compatibility Design

**Date:** August 3, 2026
**Status:** Design approved; awaiting written-spec review

## Objective

Add compatibility-first gameplay skin support to AsoBMaShow. The first
milestone loads an original Beatoraja Lua gameplay skin, uses the skin's full
2D playfield geometry instead of AsoBMaShow's perspective presentation, and is
accepted on iPad. Portable C++ remains the implementation center so most work
can be built and tested quickly on desktop.

The initial external acceptance target is the current official
ModernChic/SCURO 7-key gameplay distribution. Its exact version and SHA-256
digest must be recorded in local acceptance-test metadata before compatibility
implementation begins. The third-party package and assets remain external and
must not be committed or redistributed by AsoBMaShow.

The Beatoraja source baseline studied for this design is
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. Beatoraja selects JSON, Lua, or LR2
loaders by entry extension, while its Lua loader produces the same skin data
model used by the JSON path. The initial AsoBMaShow milestone follows the Lua
distribution because ModernChic ships with the current official Beatoraja
package and prominent current suites use Lua customization.

## Product Decisions

- Compatibility with existing skin packages takes priority over defining a new
  AsoBMaShow skin format.
- Beatoraja Lua is the first source format. JSON and LR2 are later adapters.
- The first screen is 7-key gameplay.
- A compatibility skin owns the complete visual play surface. The built-in 3D
  lane renderer and HUD are not composited with it.
- AsoBMaShow remains authoritative for input, chart time, judgment, gauge,
  score, replay, audio, failure, and stored results.
- iPad is the acceptance platform. Desktop builds and tests provide the fast
  development loop.
- The default iPad layout preserves the authored aspect ratio. Stretch and
  explicit layout adjustment are user-selectable alternatives.
- Installed skins are visible and editable in the Files app under
  `Documents/Skins`.
- Skin Lua has package-scoped file access and no network access.

## Scope

The first milestone supports the Lua language and skin features exercised by
the pinned ModernChic/SCURO 7-key entry, plus the reusable gameplay primitives
needed to support it cleanly:

- two-stage `.luaskin` header and configured-main execution;
- package-local modules and `require`;
- custom properties, selectable file paths, offsets, and persisted choices;
- images, sprite sheets, fonts, text, numeric values, sliders, graphs, gauges,
  judges, BGA, lane covers, notes, and long-note variants used by the target;
- destination conditions, timers, loops, interpolation, color, angle,
  clipping, filtering, blending, center, offset, and stretch behavior used by
  the target; and
- retained Lua callbacks for dynamic properties, writers, timers, and events.

The compatibility matrix is evidence-driven. Every object type, property ID,
timer ID, event ID, Lua module, and file API used by the pinned target is
recorded before implementation, together with whether each dependency is
critical or optional. Unsupported behavior is reported explicitly; it is never
silently treated as successful compatibility.

## Non-Goals

The first milestone does not include:

- music-select, decide, result, course-result, key-config, or skin-select
  screens;
- 5-key, 9-key, double-play, 24-key, or battle play skins;
- Beatoraja JSON or LR2 `.lr2skin` loading;
- a new skin-authoring format, live editor, or automatic object reflow;
- arbitrary Java or native-library access from Lua;
- skin-initiated HTTP, sockets, update checks, or asset downloads;
- hot reload during an active chart; or
- bundling or redistributing ModernChic/SCURO assets.

## Architecture

Compatibility mode is a parallel presentation path, not a second gameplay
engine.

### `SkinPackageStore`

Owns installed-package discovery, staging, validation, content revision
fingerprints, atomic replacement, and immutable candidate and activated
revisions. Its canonical user-editable iOS root is
`Utils::GetDocumentsPath("Skins")`. Content is copied into an app-private,
revision-addressed runtime store before an entry can be activated; active
sessions never stream from a tree that the Files app can mutate underneath
them.

Snapshot copying uses no-follow regular-file reads, rejects every link and
non-regular object in v1, and verifies that the source remained stable. A
revision fingerprint is SHA-256 over normalized relative paths and file bytes.
Lua, modules, resources, and streamed media are opened only from that snapshot.

### `LuaSkinRuntime`

Owns one sandboxed Lua state per active skin session. AsoBMaShow already links
LuaJIT and includes sol2; the implementation reuses the existing LuaJIT
dependency with JIT disabled for skin sessions on every platform. This keeps
desktop and iOS execution behavior aligned and avoids adding a second Lua
dependency.

A compatibility layer exposes the LuaJ-facing globals, modules, and standard
library behavior exercised by the pinned target. LuaJIT-only APIs remain
hidden. Differential fixtures cover language and library differences used by
the target, such as `bit32`, so successful execution does not accidentally
depend on desktop-only LuaJIT behavior.

The runtime reproduces Beatoraja's two load phases:

1. execute the `.luaskin` without `skin_config` to obtain and validate its
   header; and
2. execute it with the selected `skin_config` to obtain the complete skin.

Catalog inspection uses a fresh disposable, read-only Lua state with a tighter
budget and no event or overlay-write APIs. It is destroyed after typed metadata
conversion. Activation starts another fresh gameplay-session state and runs
both phases in that same state: first with nil `skin_config`, then again after
installing the selected configuration. Globals and `package.loaded` therefore
persist between the two activation executions as they do in Beatoraja, while no
state, callbacks, or side effects leak from catalog inspection.
Install-time default/configured validation mirrors those two executions in its
own disposable state; validation states are never reused for gameplay.

### `BeatorajaSkinModel`

Provides typed C++ representations for sources, resources, destinations,
animations, play-lane data, notes, gauges, judges, conditions, timers, events,
and retained Lua callback references. It is source-format neutral so later JSON
and LR2 loaders can target the same model.

### `PlaySkinStateBridge`

Maps an immutable AsoBMaShow gameplay snapshot to Beatoraja-compatible
properties, timers, events, and play-lane data. The bridge cannot mutate the
gameplay ruleset. Missing IDs follow Beatoraja's type-specific missing-property
semantics when those semantics are defined. Otherwise validation disables the
affected optional object or rejects a critical object instead of inventing a
value. Every such case adds one deduplicated compatibility diagnostic.

### `PlayfieldVisualState`

Extracts the render-neutral chart projection currently embedded in
`BMSRenderer`, including visible notes, long-note phase, lane/key state,
scroll/BPM/stop-derived positions, judgments, combo, gauge, timing, and BGA
state. Both the built-in renderer and compatibility renderer consume this
service so note lifecycle and chart geometry do not fork.

### `Skin2DRenderer`

Prepares the model in authored draw order and renders BGA, playfield, notes,
and HUD through bgfx. It supports the destination transforms and resource
types in the compatibility matrix. It may batch only adjacent compatible
commands; it must never reorder authored destinations for batching.

### `PlaySkinSession`

Owns the Lua state, prepared model, resources, caches, diagnostics, and frame
budget for one gameplay scene. No Lua object is shared across sessions or
threads. A session's Lua state is evaluated on one designated owner thread;
other threads receive only immutable snapshots or prepared command data.

## Data Flow

At installation time:

1. acquire a ZIP or folder, or discover a manually placed direct-child folder;
2. copy a stable candidate into private staging and validate the complete
   package tree;
3. discover `.luaskin` entries recursively;
4. execute only the header phase in the sandbox;
5. fingerprint and publish an immutable candidate revision;
6. run configured-main, typed-model, and critical-resource validation for
   currently selected entries and default-config validation for new 7-key
   entries;
7. publish app-imported source content under `Documents/Skins`; and
8. commit discovery metadata, diagnostics, and active-revision pointers only
   for entries that pass.

For app-driven imports, the visible source and candidate revision become
eligible in the app catalog through one journaled metadata commit only after
both publications succeed; crash recovery cleans orphaned staging or revision
trees. An entry is selectable only when its main model and critical resources
validate. For manual changes, the scanner verifies that the source did not
change while it was copied; an unstable tree is retried or reported and never
activated.

Tree safety is package-level, while compatibility and activation are
entry-level. Unsupported screens or key modes are catalogued as unavailable and
do not invalidate a structurally safe 7-key entry in the same distribution.

At chart loading time:

1. resolve the selected entry, saved configuration, and last validated
   immutable revision;
2. fully validate a changed visible source or configuration before atomically
   replacing that profile's entry/config activation record;
3. create a fresh session, execute the nil-`skin_config` header phase from the
   active revision, install the selected configuration, and execute the entry
   again in that same Lua state;
4. convert returned tables into `BeatorajaSkinModel`;
5. resolve resources, static references, and static conditions;
6. validate critical play objects; and
7. retain the Lua state for dynamic callbacks.

Each rendered frame then:

1. captures one immutable `PlayfieldVisualState` timestamp and state snapshot;
2. resolves snapshot-backed properties and updates custom timers at their
   Beatoraja-compatible cadence;
3. evaluates required Lua callbacks within the instruction budget;
4. applies conditions, animation, offsets, clipping, and the viewport
   transform;
5. produces an ordered draw-command list; and
6. submits contiguous compatible batches to bgfx.

Touch or pointer coordinates are transformed through the inverse of the same
viewport matrix before evaluating skin interaction regions.

## Lua Compatibility and Sandbox

The skin environment exposes only the standard Lua facilities required by the
target and explicit host modules. `ffi`, `jit`, `debug`, unrestricted `os`,
native package loaders, `package.loadlib`, and general process APIs are not
available. The host installs controlled equivalents for package-local module
loading and Beatoraja state modules. Binary Lua chunks are rejected; all
executable input is text from the immutable revision. The accepted syntax,
numeric conversions, module behavior, and standard-library subset are frozen by
differential tests against the pinned Beatoraja/LuaJ target rather than by
exposing LuaJIT extensions.

### File access

Each Lua state is rooted at one activated package revision, not at
`Documents/Skins` and never at the broader app `Documents` directory. A
package is one direct-child directory of `Documents/Skins`; all recursively
discovered entries in that directory share that package boundary. Loose skin
files at the `Skins` root are diagnosed rather than granting access to sibling
packages. Within that security ceiling, the `.luaskin` entry's immediate parent
is the virtual working directory and default module/resource base, matching
Beatoraja. A normalized relative reference may reach a shared parent only while
remaining inside the package snapshot.

- Reads may resolve only package-local regular files.
- Absolute paths, `..` escapes, symlink escapes, and access to sibling skins
  are rejected after canonicalization.
- Write modes use a quota-limited private data overlay keyed by opaque profile
  ID and normalized skin-entry identity. Generic data reads check the overlay
  before the read-only package, so an unmodified script can persist a relative
  data file without altering distributed skin assets. Lua/module loading and
  image, font, audio, or video resource resolution never consult the overlay,
  so overlay data cannot replace validated executable content.
- Package file APIs are enabled while loading and preparing a session, then
  switch to a render phase in which synchronous reads, directory scans, and
  writes are denied. Every host invocation rechecks the phase, including calls
  through a closure captured during loading. A target that needs runtime file
  access is an explicit compatibility gap unless that media type has a bounded,
  pre-opened streaming path.
- Process execution, native libraries, and unrestricted temporary-file APIs
  are unavailable.

### Networking

The initial runtime exposes no HTTP or socket API. If a future compatibility
target needs networking, it requires a separate design with visible user
permission, domain disclosure, response limits, and failure behavior. It is
not enabled implicitly by AsoBMaShow's own networking capabilities.
Beatoraja's restricted legacy `luajava`/HTTP facade is an intentional v1
divergence: any use by the pinned target is a diagnosed compatibility failure,
not a silently stubbed success.

### Execution limits

The host uses Lua instruction hooks to interrupt unbounded header, main, or
callback execution. Load work and per-frame callback work have separate
budgets. Lua states also use quota-enforcing allocators and bounded stack depth,
string size, returned-table depth and entries, converted-model objects,
resource count, file count, path length, decoded dimensions, and decoded bytes.
Host functions enforce their own byte and time limits because instruction hooks
cannot interrupt expensive C++ work. Exceeding a limit follows the same
critical-versus-optional failure policy as any other Lua or resource error.

## Installation and Files-App Editing

The Gameplay Skins settings page exposes both **Import Skin Archive** and
**Import Skin Folder**.

### Archive import

The existing iOS document-picker handoff copies the selected ZIP into staging.
The complete accepted archive becomes one package rooted at its chosen
destination directory; its top-level trees are never merged into another
package.
Extraction is streamed, reports progress, supports cancellation, and rejects:

- absolute and parent-traversal paths;
- all symbolic links, hard links, aliases, and other non-regular objects;
- duplicate or case-colliding destination paths;
- entries exceeding configured per-file or total expanded-size limits; and
- excessive path length, nesting depth, or file count.

### Folder import

The picked folder itself becomes one package root. The folder picker obtains
security-scoped access only for the import. The app recursively copies the
source to staging, applies the same validation as ZIP import, and releases
scoped access. The external source remains untouched and is not kept as a live
bookmark.

### Canonical visible store

Successful app-driven imports are atomically moved into
`Documents/Skins/<package-directory>`. A package-name collision never merges
trees: the user must choose whole-package replacement or a different name. The
repository already enables iOS file sharing and
document-browser/open-in-place behavior, so this directory appears under
**Files > On My iPad > AsoBMaShow > Skins**.

Users may also copy an unpacked skin directly into that folder or edit an
installed skin there. The app rescans:

- at startup;
- whenever Gameplay Skins settings opens; and
- when the user selects **Rescan/Reload**.

There is no active-session hot reload. A `PlaySkinSession` pins an immutable
validated revision, Lua state, and resources for the chart. Filesystem changes
become eligible after the session ends and the package is rescanned. A manual
partial or invalid edit creates diagnostics while the last valid revision
remains available; it cannot replace a running session or corrupt profile
configuration. Old private revisions are garbage-collected only when no
session or currently activated entry references them and storage policy
permits.

Deleting an entire direct-child package in Files is treated as removal on the
next rescan, not as an invalid edit: active sessions may finish on their pinned
revision, but new sessions fall back to the built-in renderer and the removed
revision becomes collectible when its final session reference closes.

Skin identity is the slash-normalized `.luaskin` entry path relative to
`Documents/Skins`. Separators and dot segments are normalized, Unicode is NFC,
and collisions are checked with platform-independent case folding while
authored spelling is preserved. Its content fingerprint is a revision marker,
not its identity. Reimporting or editing the same path preserves matching
options. Moving or renaming an entry creates a new identity. Option migration
matches declared option/file/offset keys and resets removed or incompatible
values to their declared defaults.

Installed packages are shared across profiles. Selected entries, Beatoraja
configuration values, layout mode, and custom transforms are persisted per
profile outside the skin directory.

## Configuration and Layout

The configured phase receives Beatoraja-compatible `skin_config.file_path`,
`skin_config.get_path`, `skin_config.option`,
`skin_config.enabled_options`, and
`skin_config.offset[name] = {x, y, w, h, r, a}` values. In addition to offsets
declared by the skin, 7-key gameplay exposes Beatoraja's synthesized **All
offset(%)**, **Notes offset**, **Judge offset**, and **Judge Detail offset** with
their upstream component permissions. These are compatibility controls inside
the authored canvas and remain distinct from the Fit/Stretch/Custom viewport
transform.

For every discovered 7-key entry, settings display:

- declared name, author, entry path, and source resolution;
- custom options, selectable file paths, and offsets;
- current content revision and validation state;
- layout mode and custom transform controls;
- compatibility diagnostics; and
- select, replace, remove, revalidate, and rescan actions.

The renderer uses the skin's declared width and height as its logical canvas.
The per-profile layout modes are:

1. **Fit** (default): use one uniform scale, center the canvas in the drawable
   viewport, and leave bars where the aspect ratios differ. A 16:9 skin on a
   landscape 4:3 iPad therefore has top and bottom bars.
2. **Stretch**: scale X and Y independently to fill the viewport.
3. **Custom**: start from Fit or Stretch, then apply user-controlled X/Y scale
   and X/Y translation in addition to the skin's declared offsets.

The final matrix applies consistently to images, text, notes, BGA, clipping,
and interaction regions. Custom layout is a viewport transform; it does not
infer semantics or automatically rearrange absolute-positioned skin objects.
Custom scales must be finite, positive, and clamped away from zero;
translations are bounded. Invalid persisted values reset to Fit, and a native
**Reset Layout** control remains reachable outside the skin-rendered surface.
Interaction is disabled with a diagnostic if matrix inversion ever fails.

## Gameplay Authority

Compatibility mode changes presentation only. AsoBMaShow remains the sole
authority for:

- audio and chart clock;
- input submission and assist/autoplay behavior;
- judgment windows and results;
- gauge rules, failure, and course state;
- score, combo, BP, and persistence;
- replay capture and playback; and
- scene transitions.

The compatibility corpus defines an event-mutation table listing every
supported event, writable presentation fields, value ranges, persistence, and
criticality. Unlisted events are rejected. V1 events may change only
session-local presentation state or explicitly declared skin configuration at
frame boundaries; they cannot write gameplay values, scores, general profile
fields, filesystem paths, or scene transitions.

When compatibility mode is disabled, the built-in renderer remains behaviorally
and visually regression-equivalent. Its implementation may consume the
extracted `PlayfieldVisualState`; golden and gameplay regression tests protect
existing output and timing.

## Resource and Performance Policy

Resource preparation happens during chart loading. Static file references,
sprite regions, bindings, and static conditions are resolved once. Image work
uses the existing decode coordinator and mobile cache budgets. No ordinary
skin object may perform synchronous file access, allocate a texture, or resolve
a path during active rendering. Explicitly supported streamed media is opened
from the immutable revision and pinned for the session.

Snapshot-backed host properties proven pure are memoized by ID, arguments, and
frame. Custom timers are updated and cached once per frame before custom events,
matching Beatoraja. Other Lua callbacks retain the call order and frequency
observed in pinned Beatoraja traces and are not coalesced merely for
performance; only explicitly classified pure callbacks may be once-per-frame.
Draw-command storage and temporary evaluation buffers are reused. Every Lua
callback remains constrained by the session instruction and resource budgets.

Acceptance is relative to the app's configured refresh rate on the target
iPad. The compatibility path must sustain that rate on representative charts
without recurring filesystem work, unbounded Lua execution, or resource growth
across repeated chart entries.

## Failure Handling

- Import and header failures leave the prior installation and selection
  unchanged.
- A content or configuration change is not activated until header, model,
  critical-object, and critical-resource validation pass.
- Main-load or critical-resource failure selects the initialized built-in
  renderer before chart start.
- An optional object or callback failure is reported once and disables only
  that object for the session.
- Failure of a critical play object, including the note presentation path,
  switches to the initialized built-in renderer at a frame boundary.
- Callback dependencies carry object criticality. Error or budget exhaustion in
  a critical callback schedules that same frame-boundary fallback and discards
  pending skin events or writes, then closes the failed skin session; an
  optional callback disables only its dependent optional objects.
- Lua errors and events cannot change authoritative gameplay state or stored
  records.
- The compatibility report includes entry path, Lua file and line where
  available, unsupported API/property identifiers, missing assets, disabled
  objects, and the chosen fallback.

## Verification Strategy

### Portable tests

Desktop tests cover:

- ZIP and folder manifests producing the same installed package;
- traversal, symlink, collision, quota, and partial-import rejection;
- stable-copy detection, immutable-revision activation, retention, and cleanup;
- header/main Lua phases and package-local module behavior;
- sandbox denial for sibling files, app data, native APIs, and networking;
- overlay write/read behavior and quotas;
- instruction-budget interruption for load and callback loops;
- typed-model validation and source/destination reference resolution;
- Beatoraja property, timer, condition, loop, and interpolation traces,
  including function/number/string dispatch, timer-string/OFF behavior, custom
  timer/event ordering, and zero/one/two-argument events;
- draw-order preservation and contiguous batching;
- note and long-note projection across BPM, stop, and scroll changes;
- Fit, Stretch, Custom, clipping, and inverse input transforms;
- manual edit detection, configuration preservation, and invalid-edit fallback;
  and
- built-in-renderer behavior when compatibility mode is disabled.

Small synthetic, redistributable fixtures are committed. Numeric reference
traces captured from the pinned Beatoraja source baseline are committed without
third-party skin assets. ModernChic/SCURO acceptance runs locally against the
external package. Deterministic offscreen screenshots cover 16:9 and iPad 4:3
Fit, Stretch, and Custom layouts.

Slice 1 also commits an acceptance manifest before renderer implementation. It
pins the iPad model, iPadOS version, display mode and target Hz, measurement
build, chart hashes, fixed autoplay scenarios and screenshot timestamps,
warm-up duration, run duration, repetition count, frame-time percentile and
missed-presentation limits, and allowed post-warm-up memory/resource deltas.
The chart scenarios cover normal notes, every supported long-note variant, BPM,
stop and scroll changes, chords, judgment grades, combo breaks, gauge thresholds
and failure, lane cover, BGA transitions, and song end.

The fast loop builds and runs focused skin test targets. Desktop integration
checkpoints then build all configured tests, run CTest, and confirm the app
target explicitly:

```sh
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
```

### iOS verification

`scripts/ios_release_verify.sh` is mandatory whenever a change affects
Objective-C++, the Xcode project, LuaJIT packaging, bundled resources, Files or
document-picker integration, or another iOS-specific contract. It also runs at
each integration checkpoint and before milestone completion. The script runs
release-critical native tests and an unsigned build; it does not upload a
distribution artifact.

Physical-iPad acceptance verifies:

- manual installation through `Documents/Skins`;
- ZIP and folder-picker installation of the same external skin;
- rescan after editing a skin in Files;
- option persistence across reimport and content changes;
- the manifest-pinned normal-note and long-note 7-key chart scenarios;
- BGA, notes, judgments, gauge, combo, and HUD behavior;
- Fit, Stretch, and Custom layout behavior;
- fallback from malformed and runtime-failing skins; and
- stable configured refresh rate and resource use across repeated sessions.

## Delivery Slices

1. **Compatibility corpus:** pin the external package digest, enumerate its
   required surface, add synthetic fixtures, and capture Beatoraja traces.
2. **Portable package and Lua core:** package store, ZIP/folder validation,
   Files-visible discovery, sandbox, two-phase loading, configuration model,
   and diagnostics.
3. **Portable model and renderer:** typed objects, resources, destinations,
   animation, layout transforms, draw commands, and golden tests.
4. **Gameplay integration:** shared visual-state extraction, property bridge,
   7-key notes, BGA/HUD objects, callbacks, and built-in fallback.
5. **iPad integration and closure:** picker/settings UI, iOS package audit,
   physical-device import/edit/play flows, performance, and compatibility
   report closure against the pinned external skin.

Each slice must leave the built-in renderer usable and independently verified.
JSON, LR2, other screens, and other key modes begin only after this milestone's
acceptance contract passes.

## Completion Criteria

The milestone is complete when an unmodified pinned ModernChic/SCURO package
can be installed from ZIP, imported from a folder, or placed manually in
`Documents/Skins/<package-directory>`; discovered and configured per profile;
and used to complete the manifest-pinned 7-key charts on iPad with the approved
layout modes, compatibility diagnostics, sandbox, fallback behavior, and
performance policy. All portable tests, the desktop target,
`scripts/ios_release_verify.sh`, and the physical-iPad acceptance pass.
