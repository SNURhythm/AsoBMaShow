# LITONE12 course result recall

Source comparison: local beatoraja revision
`c2ed5db1a46145ed10790c3872f717e95b59db9d` and the supplied
`LITONE12/Result` Lua scripts, inspected on 2026-10-02.

## Clear and failed animations

Beatoraja's `MusicResult.updateScoreDatabase` changes a course stage's FAILED
chart lamp to NO PLAY. Its `BooleanPropertyFactory` properties 90/91 compare
the chart lamp **equal to FAILED**, and separately check the running course's
failure. NO PLAY alone does not request the FAILED animation.

The running course becomes failed when the current stage's final gauge is zero.
For recalled stages, this must use the displayed stage's gauge, not the saved
course's eventual final outcome. A successful first stage must still display
its clear animation when a later stage failed. `ResultSkinStateBridge` applies
that rule without changing the recorded lamp.

## Per-stage rows use skin-owned history

The supplied `Result/lua/songlog.lua` appends each course-mode result to
`skin/LITONE12/Result/playerdata/coursesongs.json`. This includes both normal
stage results (type 7) and the aggregate course result (type 15).
`Result/lua/course.lua` reads the day's entire array, excluding only the last
entry. It does not select records by course identity or recall session.

Both result types therefore need to run the matching LITONE scripts to populate
these custom rows. Beatoraja selects type 7 and type 15 independently; the
engine does not run an unselected result skin to populate its private log.

The supplied `Result/result.lua` clears this log only after its course-result
draw callback has been evaluated 20,000 times. Its adjacent comment says two
seconds, but the code increments an invocation counter, not elapsed time.
Exiting the result before that callback count leaves records for the next
recall. The engine does not rewrite or clear this skin-owned log.

A repeated-load experiment with unchanged Lua scripts and one shared package
directory reproduced the stale rows:

| Load sequence | Stage rates | Aggregate rate |
| --- | --- | --- |
| First course | 80, 40 | 60 |
| Second course | 10, 30 | 20 |

On the second course, the actual `course_rate1` and `course_rate2` callbacks
returned 80 and 40. `course_averagerate` returned 44: the mean of
`80, 40, 60, 10, 30`, excluding only the new final 20. An old aggregate-only
entry similarly occupies the first chart row on a later recall.

This experiment exercised header/configured loading, persistent Lua file I/O,
and retained numeric callbacks; it was not an interactive screen test.
The normal external-skin regression uses a fresh package copy and verifies
the first course's stage values. That passing check does not establish that
the skin's retained-history cleanup works between recalls.
