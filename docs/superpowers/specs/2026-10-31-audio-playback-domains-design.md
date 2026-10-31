# Independent gameplay and system playback

The user selected the common one-output, multiple-submix approach and authorized implementation on a new branch after merging quick retry into develop.

## Intent and constraints

Chart transitions must not stop UI, menu, preview-file or skin sounds, invalidate their handles, change their speed, or compete for their command/voice capacity. Keep chart BGM and keysounds together on the existing sample-accurate timeline. Retain one native output for these two domains, the existing backend implementations, settings and rollback behavior. No new audio dependency, output-device setting, or distribution action. The user additionally requires no added playback delay or meaningful gameplay CPU regression: keep one native callback, no intermediate queue, and benchmark small output periods and high voice counts.

## Approaches

A shared voice store with bus tags is the current design; its global stop/unload and rate operations couple unrelated owners. Two native streams would isolate ownership but duplicate output configuration and hardware lifecycle. The selected design uses independently owned submixes feeding one application-owned output. Reuse the existing voice mixer, scheduling and decode implementation in each submix rather than introducing a second audio library.

## Ownership and rendering

`ApplicationAudio` owns the shared native output and two `AudioWrapper` submix processors. `Jukebox` borrows the gameplay processor in the application. System-sound consumers receive the system processor explicitly; gameplay scheduling continues to use the gameplay processor. The shared output exposes software backend adapters to the processors, so a processor stop disables and drains only its callback, never the native output. Native output ownership and settings are exposed through `ApplicationAudio`, not through a scene's jukebox.

A submix gate uses one atomic state for admission and active render ownership. The audio callback never waits; control-thread stop closes admission and waits for a prior render to exit before PCM can be released. Native callbacks render gameplay directly into bounded float chunks and add system output only when audible, pass correctly offset native presentation timestamps to gameplay, sum submixes, and clip/convert once. No callback allocation, decoding, locks or ownership destruction.

## Lifecycle and configuration

Chart pause, seek, retry, rate changes and unload affect only gameplay. System playback always uses normal speed and a separate voice/command/asset store. Scene-owned stop/dispose remains responsible for stopping menu sounds when leaving a scene. Device changes serialize both processors, suspend them without destroying system voices, stop the native output, prepare both PCM formats before output resumes, and retain rollback on failure. Application suspension applies to both processors. Existing volume conventions and per-source gains remain unchanged; do not double-apply Lua's system-volume gain.

The existing independent paged-preview stream and native background music player remain outside this refactor. The standalone AudioWrapper constructor remains useful for renderer tests and isolated clients; the application uses the shared graph.

## Verification

Regression coverage must prove SFX survives chart stop/prune/unload, stays at normal speed during altered-rate gameplay, has independent command capacity, and does not share handles or PCM ownership with charts. Cover in-flight callback retirement, native timestamp offsets across oversized blocks, float headroom, one physical stream, application suspension, successful device reconfiguration and rollback/start failure. Run audio and adjacent gameplay/menu/skin/settings tests and compile the desktop application. Commit and push task-related changes to the new branch.
