# Daily eligibility and accumulated chart achievements

Once a chart satisfies an assignment's full eligibility decision on a manual
attempt, it remains eligible for that assignment for the local day. Eligibility
covers selected table/level membership and previous-lamp filters together, not
only lamps. Initially ineligible charts can qualify on later attempts. Eligibility
is evaluated before the attempt; results from ineligible attempts do not grant
retroactive assignment progress. Persist the earliest qualifying attempt's
canonical UTC time so a delayed earlier ineligible save cannot borrow later
eligibility.

The user selected accumulation of target achievements across eligible attempts.
For each eligible chart/assignment/day, retain the best achieved lamp and best
EX-rate fraction. Compare these achievements against the current target. One
attempt may meet the score target and another the lamp target. Store actual
values, not satisfied-condition booleans, so later escalated targets still need
the higher achievement. A chart counts once per assignment/day as before.

Normal and one-chart bonus rounds, the shared ten-point bonus limit, next-day
routine edits and freeze behavior stay intact. Day rollover starts fresh
eligibility and achievements. Pending retries remain atomic/idempotent with
score projection. No chart-payload or archive scans are needed.

Persist per-chart achievements in version 3 streak state. Version 1/2 upgrades
preserve earned points, used charts and balances. Seed already-used chart entries
with empty achievements; past unsuccessful attempts cannot be reconstructed
reliably, so accumulation begins on subsequent eligible attempts after upgrade.
Validate chart identities, achievement ranges and used-chart membership.
