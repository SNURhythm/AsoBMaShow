> Historical note: the timing capture described here was removed after the performance investigation; these instructions no longer apply to current builds.

# Frame-correlated gameplay CPU timing

The iOS performance-log export includes **Metal presentation timing v2**. It uses the same frame token and CAMediaTime clock as the display-link, GPU and presentation timestamps. No diagnostic setting is required. Collection is active only while a managed Metal display-link frame owns a token; fallback, background and export work without a token are not silently assigned to another frame.

## Capture

1. Use a build containing the v2 recorder. Play normally with the skin/BGA settings you want to investigate.
2. After the slowdown or chart completion, return to the menu and export the existing performance log promptly. The fixed ring retains 8,192 callbacks, approximately 68 seconds at 120 callbacks/s (longer at lower rates); it is not a whole-session recording.
3. Share that log with the chart/skin and whether this was manual play, autoplay, replay or practice. Keep the original file intact. Confirm it contains `Metal presentation timing v2`.

The UI/export path is unchanged. The application does not write or print timing rows every frame. Export creates a best-effort snapshot and formats it on request; snapshot/export costs are not collection overhead.

## Choosing active gameplay frames

The old `gameplay` column is a sampled scene-level flag. Prefer the new observations:

- `gameplay_phase_first` / `gameplay_phase_last`: first and last observed phases within this frame.
- `gameplay_phase_mask`: all observed phases, including intermediate transitions. Bit values are preparing=2, active=4, paused=8, ending=16 and post_notes=32.
- `gameplay_mode`: manual, autoplay, replay, practice, or unknown. Replay takes precedence over its autoplay flag.
- `skin_selected`: whether the last observed gameplay presentation selects a skin. A recoverable submission fallback can still render built-in content; this field is not a GPU draw inventory.

`preparing` covers setup, preroll/count-in, practice menus and startup/failure UI. `active` means playing before or at the chart's last playable note. `post_notes` is after that note, before formal ending; the source gameplay lifecycle can retain an end-of-notes interval. `ending` follows the scene's ending state, including result preparation/finish animations. `paused` requires the playback clock to be paused. These are observations at update entry/exit, gameplay render entry, and result-transition scheduling, not a per-instruction state trace. `unknown` means no gameplay observation, including ordinary menu frames; it does not prove a scene name.

For a strict steady-active cohort, select `cpu_collected=1`, `gameplay_phase_mask=4`, and no open/invalid CPU stages. Select `gameplay_mode=manual` separately when analyzing normal manual play. Keep mixed phases as a separate transition cohort rather than calling the entire frame active or discarding it as an outlier. For practice, `post_notes` uses the chart's last playable note, not the practice section boundary; early section completion is captured by the ending state.

## Reading CPU stages

Each stage exports `cpu_<name>_seconds` and `cpu_<name>_count`. The duration is the total of completed invocations during that token. Count distinguishes an unexecuted stage from measured work that rounds to zero.

| Stage | Measured application-worker interval |
| --- | --- |
| `scene_update` | `sceneManager.update`, including scene/view update work |
| `gameplay_update` | `GamePlayScene::update`, including all its early-return/terminal branches |
| `capture` | Gameplay graph/authority/note-state construction and immutable frame capture |
| `projection` | Visible playfield projection/traversal after capture |
| `built_in_prepare` | Built-in presentation preparation, including snapshot consumption and HUD synchronization |
| `built_in_render` | Built-in presentation drawing, including note traversal and HUD; excludes the surrounding scene UI |
| `skin_prepare` | Selected skin frame preparation/evaluation |
| `skin_submit` | Selected skin render, including preflight, drawing command preparation and its post-commit work |
| `bga_prepare` | Coordinator's sole prepared BGA frame acquisition/update |
| `fullscreen_bga` | Fullscreen BGA cache/composition/blur/final-output CPU submission |
| `frame_submit` | iOS `bgfx::frame` call, including any wait inside it |
| `deferred_work` | Deferred scene callbacks, including synchronous scene changes/load/cleanup |
| `result_persistence` | `scheduleResultTransition`, including synchronous result/replay preparation and persistence before scheduling |
| `realtime_stop` | Stopping/transferring realtime gameplay authority |

**Durations are inclusive elapsed wall time, not CPU busy time.** They can include I/O, locks or scheduling waits. `scene_update` contains `gameplay_update`; gameplay update can contain persistence/authority stop. `scene_render` contains capture, projection, skin and BGA preparation. Built-in preparation and rendering are inside `scene_render` and are only recorded when that path runs. Deferred work can contain gameplay cleanup/authority stop. Do not sum parents with children or subtract an entire stage from a parent unless that call actually occurred inside it. Built-in rendering and other work remain in the scene totals without a dedicated child stage. These intervals do not cover every main-loop operation.

The existing GPU timestamps still measure whole command-buffer GPU work. `fullscreen_bga`, `skin_submit`, and `frame_submit` are CPU-side intervals, not GPU pass timers. The recorder does not measure input-to-sound or input-to-photon latency.

## Missing, interrupted and overwritten data

- The first 15 CSV columns preserve the v1 order. v2 appends CPU metadata and stage columns; consumers should parse the header by name.
- `cpu_collected=0` means unavailable, for example an in-flight snapshot or a dropped end-frame update. Never interpret the zero duration fields as a fast frame.
- `cpu_open_stages` marks scopes still open when the frame was interrupted/ended. Their completed earlier invocations may remain in totals; open invocations are not fabricated as complete durations.
- `cpu_invalid_stages` marks rejected nonfinite/backward-clock durations. Stage masks use the exported stage order starting at bit zero.
- Token replacement resets all scratch and metadata. Old scope destructors and late GPU/display updates cannot overwrite a newer token occupying the ring slot.
- Ring writes use a try-once flag, never wait, and count contention in `dropped_updates`. Snapshot can omit busy slots. Retained callback tokens are not proof that every intended display refresh was delivered.

Collection uses fixed worker-local storage, one clock pair per executed scope, and one bulk CPU publication at frame end. Other threads and untracked frames do not call the CPU diagnostic clock. The native tests check zero ordinary allocations during warmed collection, native end-frame publication, selected-presentation scope membership, and real extracted gameplay phase transitions. See the implementation plan for the current validation record.

## Local overhead check (2026-10-28)

An isolated warmed Apple M1 Pro probe at `-O2` used the real `CACurrentMediaTime` clock and 8,192-slot rings, with five batches of 100,000 iterations in each of two rounds. The old recorder's median was 0.136 µs in the noisier first round and 0.071 µs in the second. The v2 collector plus recorder measured 0.344–0.350 µs with ten scope pairs and 0.427–0.428 µs with thirteen. Thus the second paired round adds about 0.28–0.36 µs/frame for this synthetic scope workload. This excludes actual gameplay work, phase-state gathering and concurrent GPU/display updates; it is not a physical-device overhead or FPS claim.

The fixed ring grows from 1,048,592 to 3,014,672 bytes (about 2.88 MiB total). A full synthetic 8,192-row text export was 3,319,523 bytes; actual text size varies with values. Timed collection remains allocation-free; snapshot and text export allocate on demand. The fixture verified zero allocation requests for 1,000 complete warmed collection/publication cycles.
