# Progressive music preparation

The music player starts from an initial half-second of rendered PCM and prepares
subsequent audio on a worker. Gameplay continues to prepare all of its audio;
its scheduler, callback, and timing remain unchanged. Full loading can consume decoded preview pages before filling the remainder.

Use a separate listening-only source with sequential, frame-exact block mixing.
Open keysounds at first use and decode bounded windows, including long backing
tracks. Reuse the integer resampler, chart event collection, and Club Beat plan.
Keep float overlap headroom and the existing PCM conversion semantics. Sparse
notes retain silence and sounds retain tails across block boundaries.

The source appends completed blocks to a private staging WAV and exposes only
published frames to readers. Prepare 0.5 seconds before handing it to the native
player; maintain up to ten seconds of read-ahead. Seeking into unpublished audio
waits for production; seeking backward reads existing blocks without re-rendering.
Only a fully completed render atomically replaces the persistent cache file.
Cancellation and errors never publish a partial cache. Readers use bounded waits.

Native PCM adapters use AVAudioEngine/AVAudioPlayerNode on iOS and AudioTrack on
Android. Existing file playback remains for cache hits. Preserve pause, resume,
stop, seek, speed and pitch modes, duration, completion, queue/metadata controls,
audio focus/interruption handling, and background services/audio sessions.
Pause/stop disables speculative rendering; play enables it. Replacing a stream
cancels its producer. Read/underrun waiting must not block a UI or audio callback.
The playback position follows consumed audio frames, not a wall-clock accumulator.

Opening assets from archives may still require extracting an individual encoded
file. Keep extraction cancellable, retain extracted assets privately for reuse,
and bound decoder memory and the number of open file handles. Missing or invalid
keysounds remain silent, matching existing rendering behavior; operational render
failures stop the stream and expose an error instead of masquerading as completion.

Validate sample equivalence against the full renderer, initial readiness before
later assets load, long-source bounded decoding, cross-block tails, seeks,
cancellation, cache atomicity, and platform transport behavior. No deployment.


Chart previews use the same block source through a separate portable audio
backend. The preview request explicitly supplies an absolute start frame. Begin
mixing at that frame, seek prior voices to their exact rational phase, and include
all overlapping tails without rendering the skipped prefix. A suffix preview
never publishes a complete-song music cache. The audio callback only consumes a
bounded ring; decoding, disk reads, waiting, and resampling stay on feeder workers.
Respect output-device preferences and master/BGM/keysound volume settings.

A preview-session DecodedAssetCache retains native-rate/channel PCM pages under a
64 MiB LRU bound. Cache keys identify resolved source assets; changed sources
invalidate their pages. Archive extraction belongs to the cache session and
survives preview source cancellation. Cancellation preserves completed pages.
Pressing Play stops preview output and production, keeps the parsed chart/random
choices and decoded cache, and runs full Jukebox preparation. Missing pages are
decoded before immutable full sounds are installed; gameplay starts only after
successful, uncancelled completion. Non-normal play options may reparse while
still reusing compatible assets. Existing standalone #PREVIEW selection audio
continues using its independent service.
