# Play recap

User intent: browse and export improvements for session, day, week, or custom time ranges, grouped into lamp, score, and BP sections.

Use the local profile's score history; exclude imported IR records. Compare each chart/LN mode's best before the range with its best within it, not today's all-time cache. Repeated improvements collapse into one row per kind. A chart can appear in multiple sections. First records say New. Missing/deleted charts retain their stored title/hash. Courses are outside this initial chart recap.

Session means the latest stretch of recorded local plays, separated by at least 60 minutes without a result. Daily uses local midnight; weekly uses Monday; custom accepts inclusive local dates. SQL boundaries remain UTC and half-open. Day/week navigation and session navigation allow reviewing past periods. BP uses the saved bad_points value, which includes BAD + POOR + KPOOR and unpassed notes. The legacy judgement-only fallback requires completion evidence. Group lamp upgrades by achieved lamp, highest rank first, retaining alphabetical order within groups. Follow existing score/lamp eligibility and normalization policies.

Add a dedicated responsive scene reachable from both music selectors. Browse paginated sections; export all three sections into one receipt-style PNG using existing view rendering and mobile Photos support. The image is 960 pixels wide with content-driven height, independent of device resolution. No replay payload or chart archive reads. Errors must remain distinct from an empty recap. Profile changes reset results. Export renders bounded GPU strips into one streaming PNG and reports the output location only after completion.

Approaches: use historical queries (chosen) rather than recording new upgrade events (would miss old history) or differencing current caches (cannot reconstruct past boundaries). Reuse the result image export backend rather than introduce another raster/font stack.

Builder choices: 60-minute session gap, Monday week start, local-only baselines, chart-only scope, fixed-width, content-height receipt export (updated per user request).
