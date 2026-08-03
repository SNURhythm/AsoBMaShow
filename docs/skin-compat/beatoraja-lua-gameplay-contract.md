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

- The Boolean, Integer, Float, String, Timer, and Event factory methods return
  the matching built-in binding or `null` for an unsupported mapping. Timer IDs
  below zero also map to `null`. The direct `main_state.option`, `number`,
  `float_number`, `text`, and `event_index` functions immediately dereference
  their factory result, so an unknown direct lookup is an error; it is not
  silently false, zero, or empty. These behaviors are defined by
  `BooleanPropertyFactory.getBooleanProperty`,
  `IntegerPropertyFactory.getIntegerProperty`,
  `FloatPropertyFactory.getRateProperty`,
  `StringPropertyFactory.getStringProperty`,
  `TimerPropertyFactory.getTimerProperty`, `EventFactory.getEvent`, and
  `MainStatePropertyLuaApiExporter.OptionFunction/NumberFunction/FloatNumberFunction/TextFunction/EventIndexFunction`.
- A timer is off at `Long.MIN_VALUE`. `TimerProperty.isOff` and
  `TimerPropertyFactory.getTimerProperty` preserve that distinction; an event
  at timestamp zero is therefore not confused with an off timer.
- `MainController.render` updates the main clock and state, then invokes
  `Skin.updateCustomObjects`, then `Skin.drawAllObjects`. Within
  `Skin.updateCustomObjects`, all `CustomTimer.update` calls occur before all
  `CustomEvent.update` calls. Within either phase, traversal follows libGDX
  `IntMap` backing-hash iteration; it is not an ascending-ID contract. A custom
  timer caches one value for that frame. A conditional custom event observes
  the updated timers and enforces `minInterval` in `CustomEvent.update`.
  Acceptance therefore records the observed order for selected timer/event IDs
  instead of assuming a sort that `Skin.updateCustomObjects` does not perform.
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

## Intentional sandbox divergence and current compatibility gap

AsoBMaShow v1 exposes no network API and no module named `luajava`. This is an
intentional security divergence from Beatoraja's restricted
`LegacySkinLuaApi.install` facade, which can provide bounded `java.io.File`,
GDX/audio, controller, and legacy HTTP access. A use is a compatibility error,
not a successful no-op.

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
`require` fails immediately when AsoBMaShow exposes no `luajava`. The committed
`legacyLuaApiSurface` records the site counts, load-time reachability, deferred
file reachability, and guarded audio disposition without storing either helper
path. Consequently physical acceptance remains `pending`: the unmodified entry
cannot pass while the approved no-`luajava` policy remains in force. Resolving
this requires an explicit reviewed design decision; this contract neither adds
network access nor hides the incompatibility.

## Evidence and redistribution boundary

`scripts/audit_beatoraja_skin.py` is opt-in and read-only. It requires explicit
Beatoraja, archive, wrapper-prefix, and extracted-root arguments. It validates
the pinned clean clone, safely inventories the ZIP, computes the archive hash,
computes `SkinTreeDigestV1` independently over the archive payload and the
extracted tree, and refuses to emit a manifest unless the two tree digests are
identical.

The manifest retains the selected entry path and official provenance URLs.
Other external module/resource names are replaced by stable opaque IDs. No
SCURO source, image, font, audio, video, archive, absolute local path, account
identifier, device name, or physical screenshot is stored in this repository.
