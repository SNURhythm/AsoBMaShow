# Rendering allocation reduction

The user requested allocation-focused optimization for both built-in gameplay
and custom skins. This pass changes CPU-side preparation and lookup only; frame
pacing, blur, BGA ownership, draw order and shaders are unchanged.

## Changes

- Shader cache hits look up a borrowed vertex/fragment name pair without
  constructing a combined string. Keys own both names on insertion, and pairs
  cannot alias through underscore concatenation. Uniform lookups likewise accept
  borrowed string views and only allocate owned names on a miss.
- Skin quad preflight retains flat segment, command, glyph-handle, sampler and
  distance-field scratch vectors on the renderer. Submission plans are move-only
  generation-checked tokens. Every frame resolves current resource/scissor state;
  failed preparation emits nothing and can recover on the next frame.
- Built-in rendering searches the existing sorted traversed-ordinal list instead
  of constructing a hash set. Unordered input uses reusable sorted scratch and
  retains the previous membership behavior, including duplicate/sentinel handling.

Scratch capacity grows to the largest frame handled by that renderer and is
released with it. Flattening glyph storage avoids accumulating a separate buffer
for every command position. This trades retained scratch capacity for reduced
allocator churn; it does not cache previous output or resource decisions.

## Measurements

These are ordinary C++ allocation counts observed on the calling thread in the
existing tests, not total process/GPU allocations or device frame timings.

| Fixture | Before | After |
| --- | --- | --- |
| 1,024 warmed shader/uniform lookups | 1,024 allocations, 35,072 bytes | 0 allocations, 0 bytes |
| Warm skin preflight: 64 eight-glyph commands, 2 segments | 68 allocations, 6,225 bytes | 0 allocations, 0 bytes |
| Built-in render after preparation: 10 traversed rows | 17 allocations | 4 allocations |
| Built-in render after preparation: 267 traversed rows | 279 allocations | 4 allocations |
| Same large list unordered, with duplicate ordinal | 279 allocations | 4 allocations |

The cache measurement includes two shader lookups and two long-name uniform
lookups per iteration. Skin counts exclude fake-backend capture allocations;
production preflight itself is measured. Built-in counts exclude frame capture
and prepare, so this does not claim allocation-free complete gameplay frames.
Each allocation regression failed before its production change and passed after.

## Verification

Focused Metal UI, skin batch and built-in characterization tests pass. Coverage
includes borrowed non-null-terminated names, owned keys after caller mutation,
release/reload, exact shader pairs, stale/moved/wrong-owner plans, changing glyph
pages and command state, empty frames, reserve/allocation failure recovery, and
built-in pixel equivalence across ordered/unordered timeline membership.

The desktop and unsigned iOS device builds pass. The rendering/UI/gameplay
group run passed 121 of 122 checks; the remaining `main_menu_tabs_tests` failed
to compile an outdated hidden-selector fixture. The missing notification/profile
boundary members were already used by unchanged production code. Updating only
the fixture made its focused rerun pass (3.21s), completing all 122 selected
checks. Independent review is pending. Device frame-time benefit remains unmeasured. The prior blur-on
pause/resume Present Delay observation has no established connection to these
CPU allocation reductions.
