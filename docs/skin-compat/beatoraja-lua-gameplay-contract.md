# Beatoraja Lua gameplay source contract

This contract freezes the source semantics used by AsoBMaShow's first Lua
gameplay-skin compatibility milestone. The only Beatoraja baseline is commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. The default build and CTest suite
consume the committed manifest and never inspect a Beatoraja clone or the
external SCURO package.

## Pinned loading and conversion behavior

| Behavior | Pinned source |
| --- | --- |
| Loader selection is extension-based: `.json` uses `JSONSkinLoader`, `.luaskin` uses `LuaSkinLoader`, and other entries use the LR2 path. | `src/bms/player/beatoraja/skin/SkinLoader.java`, `SkinLoader.load` |
| Header loading sets the entry directory, executes the `.luaskin` while `skin_config` is absent, and converts its return value to `JsonSkin.Skin`. | `src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java`, `LuaSkinLoader.loadHeader`; selected entry symbol `play7_hw.luaskin` |
| Configured loading first performs header loading, reconciles the saved property, exports `skin_config`, and executes the entry again in the same `SkinLuaAccessor`/Lua `Globals`. Globals and `package.loaded` therefore survive between the two executions. | `src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java`, `LuaSkinLoader.load`; `src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java`, `SkinLuaAccessor.exportSkinProperty` |
| `skin_config` contains `file_path`, `get_path`, `option`, `enabled_options`, and `offset`; offset records contain `x,y,w,h,r,a`. | `src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java`, `SkinLuaAccessor.exportSkinPropertyToTable` |
| Lua callback/model fields accept functions, numeric built-in IDs, recognized string names, or Lua script strings. An unrecognized string is retained as a script, not converted to ID zero. | `src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java`, `LuaSkinLoader.serializeLuaScript` |
| Table conversion recursively maps exact public field names. Tables become arrays in the order returned by `LuaTable.keys()`; a non-table array becomes empty, unmatched fields are ignored, and non-table object records retain Java field defaults. | `src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java`, `LuaSkinLoader.fromLuaValue`; `src/bms/player/beatoraja/skin/json/JsonSkin.java`, `JsonSkin.Skin` |
| Header conversion preserves declared properties, selectable file globs, custom offsets/categories, and adds the four play offsets: All, Notes, Judge, and Judge Detail. | `src/bms/player/beatoraja/skin/json/JSONSkinLoader.java`, `JSONSkinLoader.loadJsonSkinHeader`; `src/bms/player/beatoraja/skin/SkinHeader.java`, `SkinHeader.setSkinConfigProperty` |

## Properties, timers, and events

The audited surface records every resolved property, timer, event, module, file
API, and object with a `critical` or `optional` disposition and pinned source
provenance.

- The `MAIN` Boolean, Integer, Float, String, Timer, and Button bindings use
  their matching property factories. Unsupported mappings return `null`, and
  timer IDs below zero also map to `null`. The direct `main_state.option`,
  `number`, `float_number`, and `text` functions are distinct host APIs: their
  defining `MainStatePropertyLuaApiExporter` functions call the matching
  Boolean, Integer, Float, or String factory and immediately dereference its
  result. `main_state.event_index` instead calls
  `IntegerPropertyFactory.getImageIndexProperty` through
  `MainStatePropertyLuaApiExporter.EventIndexFunction`; it does not call
  `getIntegerProperty`. An unknown direct lookup is therefore an error, not a
  silent false, zero, or empty result.
- Direct `main_state.timer` reads the requested microtimer through
  `MainStatePropertyLuaApiExporter.TimerFunction`, without using
  `TimerPropertyFactory`. Direct `main_state.event_exec` validates and executes
  the requested event through
  `MainStatePropertyLuaApiExporter.EventExecFunction`, without using
  `EventFactory`. The audit retains these direct-host origins separately from
  model binding IDs even when their numeric IDs coincide.
- A timer is off at `Long.MIN_VALUE`. `TimerProperty.isOff` and
  `TimerPropertyFactory.getTimerProperty` preserve that distinction; an event
  at timestamp zero is therefore not confused with an off timer.
- `MainController.render` updates the main clock and state, then invokes
  `Skin.updateCustomObjects`, then `Skin.drawAllObjects`. Within
  `Skin.updateCustomObjects`, all `CustomTimer.update` calls occur before all
  `CustomEvent.update` calls. Within either phase, traversal follows libGDX
  `IntMap` backing-hash iteration; it is not an ascending-ID contract, and the
  pinned implementation's collision eviction consumes global RNG state. A
  custom timer caches one value for that frame. A conditional custom event
  observes the updated timers and enforces `minInterval` in
  `CustomEvent.update`. Task 1a records that the selected configured SCURO model
  has empty custom-timer and custom-event maps. V1 therefore preserves the
  timer-before-event phase rule and uses deterministic authored order for any
  future nonempty map with an explicit compatibility-divergence diagnostic;
  it does not claim to reproduce unknown upstream RNG state.
- Skin-triggered timer writes and event execution are constrained by
  `MainStatePropertyLuaApiExporter.SetTimerFunction` and
  `MainStatePropertyLuaApiExporter.EventExecFunction`; non-writable/non-runnable
  IDs raise rather than mutating unrestricted state.

## Destination and authored order

`JSONSkinLoader.setDestination` carries omitted animation fields forward from
the previous keyframe, defaults the first keyframe, attaches destination
conditions, timer, loop, clip, mouse rectangle, offsets, and stretch, and adds
objects through `JSONSkinLoader.loadJsonSkin` in the source destination order.

`SkinObject.prepareRegion` subtracts an active destination timer, applies the
loop contract (`-1` ends, otherwise wrap after the loop point), rejects time
before the first keyframe, and interpolates the surrounding keyframes.
`SkinObject.getRate` implements linear (`acc=0`), ease-in quadratic (`acc=1`),
ease-out quadratic (`acc=2`), and step (`acc=3`) behavior. Region, clip, color,
and angle use the same selected interval; configured offsets are applied after
the authored interpolation. `Skin.prepare` removes invalid or statically false
objects before loading, and `Skin.drawAllObjects` prepares and draws the
remaining object array in authored order.

## Gameplay note and BGA phases

- `SkinNote.prepare` samples normal, mine, hidden, processed, and all ten
  long-note image roles before `SkinNote.draw` delegates the geometry to
  `LaneRenderer.drawLane`. `LaneRenderer.drawLongNote` selects distinct LN,
  CN, and HCN start/end/body phases, including active, missed, damaged, and
  reactive HCN bodies according to the current judge state.
- `SkinBGA.prepare` advances `BGAProcessor.prepareBGA` with the play timer.
  `BGAProcessor.drawBGA` makes an active miss sequence exclusive; otherwise it
  draws the base BGA (or blank) and then the layer. Image/video renderer type
  and the configured BGA stretch are selected before submission.

The selected closure also uses package-local `dofile` and `io.open`. V1 does
not expose the LuaJIT standard filesystem implementations. It replaces them
with text-only activated-revision `dofile` and a virtual `io.open` supporting
only the audited default/`r`/`w`/`a`, `lines`, `write`, and `close` shapes.
Reads are bounded overlay-first data reads, writes commit atomically to the
quota-limited private overlay, and every captured function/handle is denied
after render phase begins. No wrapper returns or accepts an unrestricted host
path.

Before any configured-model or retained-operation claim is produced, the
auditor checks `SelectedLuaClosureContractV1` for the pinned SCURO 4.02 target.
The domain-separated digest covers every loaded closure virtual path identity
and the corresponding exact ZIP source bytes in canonical UTF-8 path order.
The manifest serializes only this digest and contract metadata, never the
closure paths or source. Any byte or identity change, loaded-file addition or
removal, or archive replacement fails closed and requires explicit review of
the source constant, manifest, and acceptance record. The subsequent scanner
is a characterization of that exact reviewed closure, not a sound or complete
general Lua verifier; arbitrary Lua-syntax support is outside this contract.

The selected-closure audit freezes 20 one-virtual-path `dofile` calls and 17
`io.open` calls: four default reads, three explicit `r` reads, seven `w`
writes, and three `a` appends. It also freezes six zero-argument `lines`
calls, sixteen `write` calls (one zero-argument and fifteen one-argument),
fourteen zero-argument `close` calls, and one configured-load `listFiles`
directory scan. `write` accepts zero or more string arguments and returns its
same handle so the selected chained call remains valid; `close` returns true
on success. Nested overlay writes create only validated, package-relative
overlay parent directories.

Configured loading may perform the audited reads, writes, and directory scan.
Two selected option guards can instead retain filesystem-reading/writing
callbacks into render phase. The audit reconstructs their effective runtime
option keys, choice labels, and numeric `op` values from the selected header,
then serializes only opaque option, choice, and guard IDs. Each guard records
the ordered operation kinds derived from its retained callback's scoped,
transitive call graph. The passing vector makes both callbacks unreachable.
The negative vector makes exactly one callback reachable, takes the denied
kind from that callback's first operation, and leaves the other callback
unreachable.

At the configured-load-to-render transition, read buffers are released, all
handles are invalidated, and unclosed write buffers are discarded. A dirty
handle at that boundary is a validation failure. Every later operation through
an invalidated handle or captured filesystem function is session-critical and
must be denied before effect. Performed filesystem/resource-upload counters and
denied-attempt counters are separate.

`auditedGuardConfigurationSha256` is domain-separated static audit evidence
over the selected revision, selected entry, and effective opaque runtime
option/choice selections. The public guard-vector digest is a second
domain-separated hash over that audit digest and the ordered opaque
guard/option/choice/outcome tuples. Neither digest is
`SkinConfigurationDigestV1`; neither attests to physical configuration bytes
or may populate the acceptance record's pending
`externalDigests.configurationSha256`.

Any post-transition filesystem or resource-upload attempt is a
session-critical sandbox-integrity violation even when its caller belongs to
an otherwise optional object. The operation is denied before I/O, that skin
frame is discarded, and the initialized built-in presentation takes over in
the same frame. Performed and denied counters remain distinct in acceptance
evidence.

For the negative overlay-integrity probe, the before digest is computed
asynchronously to completion before chart/session binding. The after digest is
computed only after session teardown, and the two digests must be equal.
Timed-path polling is memory-only: it may read precomputed status but may not
hash or enumerate overlay storage during rendering.

## Audited closed legacy facade and sandbox divergence

AsoBMaShow v1 exposes no network API, Java interoperability, reflection,
controller/input access, or native object. It resolves the selected target's
load-blocking evidence by installing a closed ordinary-Lua compatibility table
under the historical name `luajava`; this table is not a Java bridge. It
contains only the audited File/Gdx class-token behavior below. Every other
`LegacySkinLuaApi` class, constructor, member, URL/HTTP/reader branch, and
`newInstance` request is a compatibility error rather than a successful no-op.

The pinned SCURO 4.02 7-key closure has two unguarded, top-level
`require("luajava")` sites in two always-loaded opaque helpers. Opaque helper A
then performs one top-level `luajava.bindClass("java.io.File")`; its two
`luajava.new(File, path)` constructor sites back single `mkdir` and `listFiles`
facade sites. The selected configured-load path reaches `listFiles` through its
rotation wrapper; `mkdir` remains deferred with no selected-entry caller.
Opaque helper B
performs one top-level `luajava.bindClass("com.badlogic.gdx.Gdx")`. Its one
application-listener/audio-processor initialization site, one `play` site, and
one `dispose` site are all guarded by `pcall`, so loss of that audio behavior
is optional. Pinned `LegacySkinLuaApi.BindClassFunction`,
`LegacySkinLuaApi.NewFunction`, `LegacySkinLuaApi.fileFacade`, and
`LegacySkinLuaApi.gdxFacade` define the corresponding Beatoraja facade; the GDX
facade exposes `graphics` and `input`, not `app`.

The guarded audio calls do not make the module optional: either unguarded
`require` would fail immediately without the closed table. The committed
`legacyLuaApiSurface` records the site counts, load-time reachability, deferred
file reachability, and guarded audio disposition without storing either helper
path. The reviewed design decision maps File construction/listing to the
package virtual filesystem, maps latent `mkdir` to the private overlay, and
returns a GDX table with no `app`, matching the pinned optional-audio failure.
It exposes neither a Java value nor a host path and it adds no network access.
Physical acceptance remains `pending` until that exact facade and every other
runtime/renderer criterion are implemented and measured.

## Evidence and redistribution boundary

`scripts/audit_beatoraja_skin.py` is opt-in and read-only. It requires explicit
Beatoraja, archive, wrapper-prefix, and extracted-root arguments. It validates
the pinned clean clone, safely inventories the ZIP, computes the archive hash,
computes `SkinTreeDigestV1` independently over the archive payload and the
extracted tree, and refuses to emit a manifest unless the two tree digests are
identical. It also requires the exact reviewed archive hash and selected Lua
closure digest before emitting the configured evidence described above.

The manifest retains the selected entry path and official provenance URLs.
Other external module/resource names are replaced by stable opaque IDs. No
SCURO source, image, font, audio, video, archive, absolute local path, account
identifier, device name, or physical screenshot is stored in this repository.
