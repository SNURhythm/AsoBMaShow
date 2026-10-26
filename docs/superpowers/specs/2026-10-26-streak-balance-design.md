# Streak balance and daily completion

The user accepted the balance recommendations and selected freeze pricing of
20 points, +5 per purchase, capped at 50 until a streak reset. Preserve attendance
protection, next-day routine edits, the two-freeze purchase capacity and existing
balances. No deployment or worktree changes.

Use a shared daily bonus budget of 10 points with no per-assignment cap, per the
user's follow-up. A goal-reaching play still earns no bonus. Allocate simultaneous
bonus completions in stable assignment-ID order and stop further assignment
progression once the shared budget is spent. Derive the daily total from persisted
bonus counters; older days above the new budget retain points but earn no more.
One assignment may earn all ten. No persistence schema change is needed.

The user also requested a fixed chart count of one for every extra-point round,
regardless of the assignment's normal quota. On goal completion, switch every
assignment, including unfinished rounds, to a one-chart quota with zero partial
count. Keep used-chart identities and earned points. Existing saved bonus rounds
use the same rule on their next play; partial old counts grant no free rewards.
Tomorrow restores the configured normal counts.

Keep randomized score/lamp escalation only when the finishing attempt already
meets the next threshold. When no bounded increase is available, leave the target
unchanged. Bonus chart counts never escalate. This avoids unsupported target
jumps; it does not guarantee a sufficient pool of unplayed charts for every
user-defined routine.

Lead the completed dashboard with a themed success panel, “Today is complete,”
streak length, and an explicit “Done for today” action. Show goal points capped
at the goal, with bonus points separately. Collapse optional challenges by
default and disclose them through an explicit action. After the daily budget is
spent, show completion rather than another unfinished round. Built-in results
should give the same completed-day message. Use the existing native theme and
localization system; full animated celebrations remain a separate polish pass.

The shared budget, bounded escalation and presentation details are implementation
choices supporting the accepted recommendations. Continue in the current checkout,
verify, commit/push and update PR #160.
