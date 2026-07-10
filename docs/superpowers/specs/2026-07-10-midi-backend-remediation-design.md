# Portable MIDI Backend Remediation Design

## Goal

Close the Input Task 5 review findings without changing the public input-profile contract: native MIDI adapters must never lose an initial packet, callback teardown must be memory-safe without unbounded retired contexts, and every simultaneously live CoreMIDI source must have a distinct stable ID.

## Constraints

- Keep `feature/foundation-input-midi` isolated and do not merge it.
- Native callbacks may only copy work into the portable queue; registry publication remains on the main-thread `pump()` path.
- Stable IDs may be volatile for truly indistinguishable devices, but two live devices must never share an ID.
- Callback safety must not depend on undocumented synchronous callback quiescence from CoreMIDI or WinMM.
- CoreMIDI client lifetime must follow Apple's application-lifetime recommendation while backend ports and notification subscriptions remain restartable.
- Android MIDI stays optional and must fail without disabling SDL input.

## Architecture

### Opaque callback entry registry

`NativeCallbackLifetime` will register an owner under a monotonically increasing, never-reused integer token. Native APIs receive the token encoded as an opaque `void *`; callbacks convert it back to an integer key but never dereference it.

Callback acquisition first copies shared state from a process-lifetime registry under a mutex, then increments that state's active lease count. Closing removes the token from the registry, marks the state closed, clears the owner pointer, and waits for existing leases. This covers both shutdown races:

- a callback that entered the OS trampoline but has not looked up the token resumes, misses the removed token, and returns safely;
- a callback that already holds a lease keeps the context and backend alive until it releases the lease, and close waits for it.

CoreMIDI connections, the CoreMIDI notification subscription, and WinMM connections will use this helper. Disconnected contexts can then be reclaimed immediately after `closeAndWait()`; `retiredConnections_` is removed.

### Connect publication before activation

`QueuedMidiInputBackend` will expose an RAII activation publication. Construction queues a connected snapshot before the caller enables native reception. `commit()` keeps the connection; destruction without commit queues a compensating disconnected snapshot.

CoreMIDI will begin publication before `MIDIPortConnectSource`, and WinMM before `midiInStart`. Android will call `nativeMidiDevice(..., true)` before `outputPort.connect(receiver)` and call the matching disconnect if `connect` fails. Thus any packet callback can only enqueue after the connected boundary, while native activation failures roll the boundary back before the next pump.

### Live CoreMIDI ID allocation

`LiveMidiDeviceIdAllocator` will preserve an ID for an already-live endpoint and claim a collision-free ID for each new endpoint. It accepts the enumerator's preferred ID, but if that ID is already held it appends a deterministic session suffix until free. Removing an endpoint releases only its claim.

This intentionally does not promise persistence for indistinguishable duplicates. It does guarantee that add/remove/re-add sequences cannot merge two live sources into the same parser or registry entry.

### Shared CoreMIDI client

A process-lifetime `CoreMidiClientService` will lazily create one `MIDIClientRef` and intentionally leave its disposal to process termination, matching Apple's documented guidance. The service object itself also has explicit process lifetime.

Each backend still creates and disposes its own input port. The service holds only opaque notification tokens for currently active backends; dispatch acquires each through `NativeCallbackLifetime`. Backend stop first unsubscribes its token and closes the notification lifetime, so a late notification either holds a safe lease or misses the registry.

### Android reliability and text

Android device opens will use a three-attempt bounded retry with a short main-loop delay. Device add/start resets the budget; a status change may re-arm a failed still-present device; stop/removal cancels it by generation and clears state. Successful connection clears the retry budget.

JNI strings will be read as UTF-16 with `GetStringChars` and converted by a portable `utf16ToUtf8` helper. Valid surrogate pairs become four-byte UTF-8, while unpaired surrogates become U+FFFD, ensuring names published to C++ are valid UTF-8.

## Error and teardown behavior

- Failed native activation produces a queued connect/disconnect pair and no retained native connection.
- Disconnect first prevents new logical packets, unregisters callback entry, drains active leases, closes the OS handle/port, publishes disconnect, and releases the live ID.
- Queue overflow behavior remains unchanged: affected parsers reset through a synthetic disconnect/reconnect release boundary.
- Android retry never outlives its backend generation and never exceeds three automatic attempts per trigger.
- A failed shared CoreMIDI client creation remains an explicit MIDI-only backend startup failure.

## Test strategy

Portable C++ tests will be written first and observed failing for:

- a packet queued synchronously during activation being published only after the device connect;
- failed activation producing a connect/disconnect rollback;
- an active callback lease blocking close until release;
- a callback that delays lookup until after close safely failing acquisition;
- a stale token never acquiring a newly registered context;
- CoreMIDI-style A/B/remove-A/re-add-A allocation preserving distinct live IDs;
- UTF-16 BMP, supplementary-plane, and malformed-surrogate conversion.

Platform source changes follow those portable contracts. Verification includes focused repeated CTest, full desktop build and CTest, iOS build-only, Android Java plus arm64 native build-only, project/XML lint, and a clean feature worktree before the final commit/report.
