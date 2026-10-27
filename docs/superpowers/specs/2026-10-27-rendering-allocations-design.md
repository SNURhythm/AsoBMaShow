# Rendering allocation reduction

The user requested rendering optimization, particularly allocations, and explicitly selected both built-in gameplay and custom skins. Implement focused CPU allocation reductions in the existing C++ renderer. Preserve output, ordering, media ownership, all-or-nothing preflight, and current pacing; no new telemetry or deployment.

## Approach

Use allocation-free heterogeneous shader/uniform cache lookup, retaining owned keys only on misses. Reuse per-renderer contiguous skin preflight storage, with a lightweight generation-checked submission token. Replace the built-in per-frame timeline hash set with lookup in the existing ordinal list (binary search when sorted, reusable sorted scratch otherwise).

These are implementation choices within the requested optimization. A general arena would add allocator/lifetime complexity; caching entire render outputs would require broader invalidation changes. Reuse scratch only: resources, scissor, filters and glyph pages are still resolved each frame. Existing UI upload buffers already reuse capacity.

Storage grows to the largest prepared frame for that renderer and is released with it. Flatten glyph handles so command/segment fluctuations cannot accumulate separate retained buffers. Failed preparation must remain uncommitted and recover next frame; stale plans must never read current storage. Built-in arbitrary ordinal order remains supported.

## Verification

Extend existing tests using the shared test-only allocation observer. Demonstrate cached lookups and warmed skin preflight allocate zero ordinary C++ objects, with output/ownership assertions. Compare built-in render allocation counts for small and large timeline lists after preparation, and preserve characterization coverage. No new test executables. Run affected rendering/UI/gameplay checks and desktop/unsigned iOS builds. Record measured allocation reductions without claiming GPU latency or physical frame-time benefit. Commit and push on the current PR branch as instructed.
