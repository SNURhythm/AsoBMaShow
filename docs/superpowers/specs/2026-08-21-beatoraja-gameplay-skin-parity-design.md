# Beatoraja Gameplay-Skin Parity Design

## Goal

Support every gameplay-skin feature accepted by the pinned Beatoraja Lua,
JSON, and LR2 gameplay loaders, including features that are absent from the
current property-oriented `docs/todo.md` inventory.

The compatibility contract is Beatoraja commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. Behavior is derived from that
source rather than inferred from third-party skins or current AsoBMaShow
behavior.

## Scope

The work includes every skin type and loader path that Beatoraja routes to its
gameplay player, across:

- Lua `.luaskin` gameplay skins;
- JSON gameplay skins;
- LR2 `.lr2skin` gameplay skins, including MS932 input and includes;
- gameplay objects, destinations, conditions, offsets, timers, events,
  interactions, resources, host APIs, and runtime state used by those skins;
- live gameplay, replay playback, and replay video export;
- package discovery, validation, diagnostics, and resource lifecycle needed to
  make those gameplay skins usable.

Known missing visual families include judgement/note distribution graphs, BPM
graphs, timing visualizers, hit-error visualizers, gauge graphs, timing
distribution graphs, negative-type distribution graphs, complete PM-character
rendering, the practice object, bitmap fonts, LR2 fonts, LR2 source movies, and
editable text behavior. Timing-distribution declarations are also currently
rejected, although pinned `SkinTimingDistributionGraph.prepare()` makes them
an explicit no-op in gameplay and renders them only for `MusicResult`. This
list is illustrative; the source-derived ledger is authoritative.

Non-gameplay skin screens and unrelated Beatoraja application systems are out
of scope. A dependency needed to reproduce gameplay-skin behavior remains in
scope even if its implementation lives in a shared subsystem.

## Architectural choice

Use a source-neutral compatibility pipeline. Lua, JSON, and LR2 have separate
frontends, but they normalize into one expanded gameplay-skin model and share
resource planning, validation, runtime authority, interactions, and rendering.

Maintaining three independent renderer stacks would duplicate behavior and
make cross-format drift likely. Running Beatoraja or a JVM as the production
renderer is unsuitable for the mobile targets. The pinned Java implementation
may instead be used as a development-time differential oracle.

## Source-derived parity ledger

Create a machine-readable gameplay compatibility ledger from the pinned
source. It must enumerate, at minimum:

- Lua and JSON fields and object families;
- LR2 gameplay commands and their argument/default rules;
- destination fields, interpolation, stretching, blending, offsets, and
  conditions;
- properties, timers, events, writers, and interaction modes;
- Lua `main_state` functions and legacy facade operations;
- runtime data required by each object;
- source-defined null, default, ignored, fallback, or malformed-input cases.

Every entry maps to an implementation and focused tests or to an explicit
source-defined no-op/default classification. CI must reject an unclassified
valid surface discovered by the ledger checks. Valid constructs may not be
silently converted to unsupported blank objects.

`docs/todo.md` can remain a human-readable status summary, but it is no longer
the completeness authority.

## Format frontends

Package discovery must recognize Lua, JSON, and LR2 gameplay entry points
without exposing non-gameplay skins in the gameplay catalog.

Each frontend owns only format-specific behavior:

- Lua execution and returned-table decoding;
- JSON decoding, defaults, and field coercions;
- LR2 MS932 decoding, CSV command parsing, include ordering, legacy path
  resolution, coordinate conversion, and command defaults.

All frontends emit the same canonical model while retaining authored order,
source location, include provenance, and original identifiers. Diagnostics can
therefore point back to the exact file, include chain, object, field, or LR2
command that produced a model element.

Equivalent features authored in different formats must converge after
normalization. Format-specific semantics remain explicit model metadata where
normalization would otherwise erase observable Beatoraja behavior.

## Canonical gameplay-skin model

Expand the existing model rather than bypassing it with format-specific draw
paths. The model must represent every gameplay object and destination admitted
by the ledger, including specialized graph data, practice presentation,
PM-character animation, bitmap-font glyph sources, movie sources, text writers,
custom timers/events, and format-specific compatibility flags.

Object order is stable and reflects Beatoraja's authored destination order.
Conditions and timers are evaluated against a captured frame. Resource
references are resolved by a separate planning phase so decoding does not
perform GPU, movie, audio, or filesystem work during a frame.

The normalized representation is consumed by one renderer and one interaction
layer. Click regions, slider writers, text editing, custom events, and custom
timers therefore behave consistently across formats while retaining their
source-defined argument rules.

## Runtime authority

Gameplay publishes an immutable skin frame consumed by live play, replay, and
replay export. The frame and its retained histories must provide every value
required by the pinned gameplay objects, including:

- per-note judgement and timing-difference state;
- the source-sized recent-judgement histories used by timing and hit-error
  visualizers, including the exact 100-entry behavior where Beatoraja uses it;
- per-second note distribution and judgement/early-late buckets;
- BPM, scroll, stop, transition, minimum, maximum, and main-BPM series;
- gauge history and graph sampling state;
- practice menu and practice-object state;
- key, pointer, controller, audio, and custom timer/event state.

Histories update incrementally and use Beatoraja's capacities, ordering,
defaults, reveal timing, cursor behavior, and reset boundaries. The renderer
does not reconstruct authoritative gameplay history from draw calls.

Skin-originated mutations remain transactional: they are staged against the
captured frame and applied only after successful frame submission. Replay and
export paths receive the same immutable authority rather than approximating
missing state independently.

## Lua capability boundary

Follow the pinned Beatoraja rule exactly. This is a finite compatibility facade,
not arbitrary Java or unrestricted operating-system access.

The legacy facade mirrors the pinned `LegacySkinLuaApi` allowlist for the
specific supported operations involving `Gdx`, `Input`, `Controllers`,
`Controller`, and `File`, plus the source-supported construction/reading path
for URL, input-stream, and buffered readers. File listing and directory
creation, graphics dimensions, key state, and the first controller's name and
button state follow the source argument and return rules.

The `main_state` surface includes the pinned property APIs and the source
functions for file existence, directory creation, file listing, line reading,
write, append, clear, line counting, bounded HTTP GET/line retrieval, and audio
play, loop, preload, stop, and dispose.

HTTP remains limited to `http` and `https`, uses the pinned timeout behavior
(default one second and at most five seconds), and retains the pinned response
limits of 1,024 lines and 65,536 characters. The Lua environment keeps the
source restrictions on unsafe standard globals, native library loading,
package paths, and process spawning. No broader host or Java bridge is added.

## Resource and performance model

Parsing, normalization, validation, script compilation, and resource discovery
occur once during cancellable session preparation. A resource plan deduplicates
textures, fonts, movies, and audio across entry points and includes before
materialization.

Expensive decode and compile work stays off the gameplay frame path. Rendering
must avoid per-frame filesystem access, resource discovery, script compilation,
and avoidable allocations. Graph updates use bounded incremental histories.
GPU, movie, font, and audio resources are released deterministically with the
skin/session lifetime.

Cold and warm loading benchmarks use representative built-in, synthetic, and
locally available compatibility skins. Results are compared with `develop`;
median loading may not regress by more than 10 percent unless variance or a
specific compatibility cost is measured, documented, and approved.

## Compatibility and diagnostics

Observable defaults, coercions, selector evaluation, destination ordering,
coordinate conversion, missing-resource behavior, and malformed-object
boundaries follow the relevant pinned loader.

Invalid content produces structured diagnostics with format, source file,
include chain, object type, and field or command. Recovery or rejection occurs
at the same practical boundary as Beatoraja. Include-cycle protection and
resource safeguards must not alter valid skin behavior.

An explicit source-defined null, ignored, or default result is not treated as a
missing feature. Conversely, an implementation fallback is not labeled
compatible merely because rendering continues.

## Delivery milestones

1. Build and gate the source-derived gameplay parity ledger.
2. Add gameplay package discovery and canonical JSON and LR2 frontends.
3. Implement missing object and resource families individually.
4. Add the runtime authorities and exact Lua host capabilities those objects
   require.
5. Prove cross-path parity, resource lifetime, and loading performance.
6. Complete conformance fixtures, ModernChic acceptance, platform verification,
   GitHub review resolution, and repeated clean self-review.

Shared prerequisites receive narrow commits. Each loader, visual family, host
capability, runtime authority, and corrective finding receives its own
independently reviewable commit. The completed branch is pushed only after the
full goal and review cycle pass.

## Verification and definition of done

Committed redistributable fixtures cover every valid ledger entry. Overlapping
Lua, JSON, and LR2 fixtures assert equivalent normalized and draw behavior.
Focused model, draw-command, interaction, runtime-state, and script-sandbox
tests cover defaults and failure cases. Visual goldens cover the specialized
graphs, fonts, movies, practice UI, PM characters, notes, gauges, BGA, covers,
judges, text editing, blending, stretching, and animation.

Development-time differential tests compare source-shaped traces or results
with the pinned Beatoraja implementation where automation is practical. Pixel
goldens use redistributable synthetic assets; third-party skin assets are not
committed.

ModernChic is a local acceptance target. It must load without unsupported
gameplay objects, render its note, timing, BPM, and hit-error graphs correctly,
and select LN modes according to Beatoraja rather than a locally inferred shift.

Completion requires:

- no unimplemented or unclassified valid gameplay ledger entry;
- no unsupported placeholder for a valid committed fixture or ModernChic
  gameplay object;
- passing focused and full desktop tests;
- passing Android compilation and the unsigned iOS release verification;
- passing relevant GitHub checks;
- all valid PR review threads fixed and resolved;
- a complete self-review cycle with no actionable finding;
- feature-by-feature commit history, no accidental worktree content, and the
  completed branch pushed.
