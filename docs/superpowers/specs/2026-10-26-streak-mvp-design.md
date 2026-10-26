# Streak MVP

Build a profile-local, non-commercial practice routine. Players choose a daily
point goal and repeatable assignments combining levels from multiple difficulty
tables, previous lamps (any/exact/or below), target lamp and/or score, distinct
chart count, and point reward. Rank targets include E through MAX, including
MAX -, plus a custom EX rate. A saved attempt must meet both enabled targets.

Meeting the daily point goal advances the streak once. Any manually played
single chart protects it without advancement, including failed or ineligible
attempts. Autoplay, replay viewing and practice do not count. No-play days consume
one owned freeze automatically; without a freeze the streak resets. Earned
milestones and the best streak survive a reset. Streak days count goal days,
not consecutive calendar dates.

Assignments repeat automatically. Each chart counts only once per assignment
per local calendar day, even across rounds or overlapping table entries. One
play may advance multiple assignments. After the daily goal is reached, finishing
an assignment randomly increases one enabled requirement for its next round:
chart count +1, the next score rank (custom rate +2 percentage points), or the next
lamp. Requirements never weaken or exceed MAX/Full Combo. Rewards stay fixed.
Next day restores the configured requirements. Eligibility uses the record
before the qualifying attempt, not its newly saved lamp.

Points above the daily goal enter a savings balance. A freeze costs today's
daily goal; at most two may be purchased into inventory. Spending does not reduce
daily, weekly or monthly earned points. MVP defaults: daily 100, weekly 500,
monthly 2000. Players can set all three targets. Goal/assignment edits take
effect tomorrow once play has started; before play they apply today. Calendar
boundaries use local midnight, weeks start Monday. Clock rollback cannot reroll
an already opened day. Saved retries must be idempotent. Late persisted attempts
use their original play date, and can return an unnecessarily consumed freeze.

Streak milestones: 7, 14, 30, 50, 100, 365. Weekly/monthly goal achievements are
retained. MVP provides a usable native dashboard/editor and a short result
acknowledgment. Rich animation, sound, share cards, browsing eligible charts
from an assignment, and custom-skin dashboard integration are follow-up polish.

## Approaches

Use the existing native scene/view system and profile score SQLite database.
Persist streak updates in the same transaction as a newly projected result;
this preserves retry safety and profile backup/switch behavior. A pure model
owns rules and calendar accounting; the repository bridges stored attempts and
table membership. A separate JSON file was rejected because a score and reward
could then commit independently. A new gameplay mode was rejected because
ordinary play should count automatically.

The user requested implementation now, with UI/UX refinement after the MVP.
Unspecified defaults above are implementation choices, not additional user
requirements. No monetization, deployment, worktree, or parser changes.

## Resolved implementation policies

Previous eligibility includes imported IR best lamps, following selector records.
Modified manual attempts can earn assignments using their effective assist-capped
lamp. The round crossing the daily goal immediately escalates its next round.
Recovered unnecessary freezes may exceed purchase capacity temporarily; they are
first reassigned to subsequent unprotected absences. These are MVP choices.
