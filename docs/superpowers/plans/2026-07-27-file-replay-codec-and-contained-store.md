# File Replay Codec and Contained Store Implementation Plan

> Slice 2 of the contract-first file replay restart. The umbrella design and
> this focused design are pre-approved when consistent; execute inline with
> test-driven development and continue directly to Slice 3.

**Goal:** Add pinned Beatoraja BRD interoperability, canonical replay paths,
and a recoverable contained file lifecycle without activating runtime replay
persistence.

**Architecture:** Reuse the Slice 1 setup, playback, validation, and limits as
the only domain authority. Keep byte encoding, path grammar, and filesystem
durability separate. Model ownership transitions explicitly so later SQLite
coordination can acknowledge the exact verified installation rather than
inferring ownership from a filename.

## Constraints

- No SQLite schema or runtime call-site change in this slice.
- No gameplay capture, result, provenance, IR, or capability activation.
- Do not copy code or fixtures from PR #82 until the corresponding new test
  fails for their absence.
- No result facts or persistence identifiers in replay bytes or file metadata.
- Use `replay::kReplayLimits`; do not add a second limit structure.
- An occupied path is not replaced without a database-aware owner
  reconciliation, which is outside this slice.
- Every task begins red, ends green, and is committed independently.

## Task 1: Path grammar and exact setup projection

**Files:**

- Add `src/replay/BeatorajaLongNoteMode.h`
- Add `src/replay/BeatorajaReplayPath.{h,cpp}`
- Modify `src/replay/ReplaySetup.{h,cpp}`
- Add `tests/beatoraja_replay_path_tests.cpp`
- Extend `tests/replay_setup_tests.cpp`

**Red contract:** Exercise chart/course stems, parsed undefined-LN agreement,
all supported constraint IDs, 256 structural stages versus the smaller
filename-eligible boundary, maximum history suffix, unsafe components, exact
stock lane-shuffle patterns, manual-assignment fallback, and DP FLIP.

**Green implementation:** Add pure path builders using `ReplayLimits` and add
lane-shuffle representation/validation to the canonical setup. No filesystem
access is allowed from path code.

## Task 2: Bounded byte primitives and independent fixtures

**Files:**

- Add `src/replay/Base64Url.{h,cpp}`
- Add `src/replay/GzipCodec.{h,cpp}`
- Add `tests/replay_codec_primitive_tests.cpp`
- Add `tests/fixtures/replay/BeatorajaFixtureGenerator.java`
- Add the three Java-generated golden fixture files

**Red contract:** Bound Base64URL output before allocation; reject invalid
padding/tail bits; reject malformed/trailing/bad-CRC gzip streams; accept exact
limit-sized streams; and verify the documented fixture hashes before codec
code sees them.

**Green implementation:** Introduce small bounded primitives backed by the
existing miniz compilation unit. Copy the independent fixtures only after the
fixture-presence/hash test fails.

## Task 3: BRD codec closure

**Files:**

- Add `src/replay/BeatorajaReplayCodec.{h,cpp}`
- Add `tests/beatoraja_replay_codec_tests.cpp`

**Red contract:** Decode independent stock chart/course fixtures; map signed
key records for 5/7/9/10/14/24/48 key modes; allow empty completed input;
preserve pre-roll, both scratch directions, exact lane patterns, manual
assignment, DP FLIP, setup extension facts, touch/lane-cover events, and
bounded course rest; reject count/size/depth/type violations and every
stock/extension identity disagreement.

**Green implementation:** Encode stock gzip JSON plus versioned playback-only
extension. Decode with caller-supplied stage key modes and time bounds, then
run the Slice 1 setup/playback validator. Keep unsupported extensions
distinguishable from corrupt files.

## Task 4: Explicit lifecycle and contained file store

**Files:**

- Add `src/replay/ReplayFileLifecycle.{h,cpp}`
- Add `src/replay/ReplayFileStore.{h,cpp}`
- Add `tests/replay_file_lifecycle_tests.cpp`
- Add `tests/replay_file_store_tests.cpp`

**Red contract:** Cover legal/illegal lifecycle transitions; canonical
containment; symlink and traversal rejection; private temporary naming;
write/flush/rename/directory-sync/validation faults; ambiguous success
reconciliation; exact-attempt retry; occupied-path refusal; metadata-bound
inspection/removal; and cleanup restricted to proven private artifacts.

**Green implementation:** Use private file writes, durable rename, explicit
parent sync, SHA-256/size validation, and the pure lifecycle transition model.
The store returns a verified installation receipt but cannot manufacture a
database association.

## Task 5: Slice closure and review

**Files:**

- Add `tests/replay_codec_store_contract_tests.cpp`
- Extend `tests/replay_contract_boundary_tests.cpp`
- Update CMake registrations

**Red contract:** Prove local chart/course documents accepted by Slice 1 encode
and decode under the same limits, then install/inspect/remove without adding
result facts or repository dependencies. Source-audit the codec/store layer for
duplicate limits and forbidden persistence vocabulary.

**Green implementation:** Repair only shared boundaries found by closure.
Run all Slice 1/2 focused tests, full CTest, and desktop `main`; review the
complete diff against `origin/develop` for duplicate setup, limit, path, and
ownership authorities; commit review fixes with regression tests.
