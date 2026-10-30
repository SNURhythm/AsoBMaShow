# Goal Challenge

## Intent
Rename chart goals' Select chart action to **Challenge!** and prepare the selected goal for play while retaining the existing folder-preserving navigation and LN-mode handling.

## Behavior
- Set pacemaker mode to GOAL and remember the exact challenged goal's ID in profile settings. Read its current target at play time; a lamp-only challenged goal falls back to BEST.
- When GOAL is chosen manually, clear the explicit ID and use the lowest unfinished score target for the selected chart and effective LN mode. Missing/deleted goals fall back to this lookup, then BEST. Achieved explicit targets remain usable.
- The play-option bar displays the resolved target (AAA, 87.77%, or BEST); the GOAL dropdown option displays GOAL: <target>.
- Raise the gauge to the minimum mapped lamp that satisfies a lamp goal only if the selected gauge and its auto-shift range cannot reach that lamp. Preserve higher gauges, Best Clear coverage, shift mode, and lower bound. Use the existing gauge/lamp mapping (including Hazard for Full Combo) and normalize the legacy Light Assist Easy ID.
- Score targets use the goal's exact fraction, rounded up to EX score, and proportional progression. Freeze the resolved target for a live attempt and its result. Preserve existing practice/course/auto-play exclusions.
- Persist only the selected ID and mode in profile settings; do not alter goal tables or migrate goal payloads. Resolve labels with a cache keyed by profile, chart, LN mode, preferred goal, and score revision.

## Approaches
The chosen behavior uses an exact ID after Challenge and the lowest unfinished score target for manual GOAL selection, with BEST fallback (confirmed by the user). Always choosing the highest goal or disabling missing-score targets was rejected in favor of that behavior. Reuse the existing pacemaker and selector refresh paths rather than adding a separate challenge screen.
