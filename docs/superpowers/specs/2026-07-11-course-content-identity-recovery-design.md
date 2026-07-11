# Course Content Identity and Record Recovery Design

## Goal

Make course scores, clear lamps, and replays follow the playable course
definition rather than the transient `difficulty_courses.id`, matching
beatoraja's content-addressed behavior. Recover records orphaned by earlier
difficulty-table refreshes whenever the stored evidence identifies a current
course safely.

## Chosen approach

Use a versioned canonical course-definition key as the authoritative identity.
Keep `course_id` only for chart-library navigation and as a compatibility
fallback for legacy rows that contain no content identity.

Two alternatives were rejected:

- Preserving numeric IDs alone prevents future churn but cannot recover rows
  already orphaned and leaves every consumer coupled to chart-database row
  allocation.
- Removing numeric IDs entirely would require an unnecessary chart-library and
  UI rewrite. IDs remain useful navigation handles even though they are not
  record identity.

## Canonical identity

`CourseIdentity` serializes the following payload unambiguously and stores its
SHA-256 digest as `course:v1:<digest>`:

1. A fixed schema/version tag.
2. The ordered chart sequence. Each chart uses normalized SHA-256 when
   available, otherwise normalized MD5. Paths and display metadata are not
   durable identity.
3. The semantic course constraints in fixed type order: no-speed, judgement
   restriction, gauge profile, and forced LN type.

Course name, group, table URL/ID, level, sort order, trophy conditions, title,
artist, and download URLs are excluded. Grade/grade-mirror/grade-random merely
restrict the actual play option and are also excluded; the chosen option stays
on each record. Therefore renamed or reordered table entries retain records,
while changed chart membership/order or score-affecting constraints create a
new identity. Duplicate course definitions intentionally share records, as they
do in beatoraja.

The chart database stores the chosen key on `difficulty_courses`. On a refresh,
the existing strongest-common hash comparison remains in place so adding a
secondary hash does not split identity; a content-equivalent course retains its
existing key and navigation ID. `CoursePlaySession` carries this stored key so
new score and replay writes use the chart definition's authoritative identity.

## Score database

Score schema version 6 keeps the existing `course_scores.course_key` column but
changes its meaning to the canonical definition key. Migration converts the
legacy value (`name + raw constraints + ordered chart tokens`) into the new key
without using the name.

Course scores also gain `ln_mode`. New rows store the effective selected LN
mode, allowing course lamps and best-record lookup to distinguish LN/CN/HCN in
the same way beatoraja's score mode does. Older rows cannot reconstruct this
selection exactly, so migration stores a documented legacy wildcard value;
those records remain visible in every LN mode instead of being guessed or
discarded.

Clear-rank caches group by `(course_key, ln_mode)`. The main-menu course folder
looks up its content key for the selected LN mode. Detailed best-score lookup
uses the key and compatible LN mode. Numeric fallback is permitted only when a
row's key is empty; a nonempty mismatching key must never match by ID.

## Replay database

Replay schema version 4 adds `course_replays.course_key` and an index ordered by
key and replay ID. New replay saves receive the full session key before stages
are reduced to a completed/recorded prefix.

Migration backfills complete legacy course replays from their ordered stage
chart identities and canonical constraints. Partial legacy replays cannot
derive the unplayed suffix from replay data alone. Recovery therefore uses
current chart-course definitions and, when available, full legacy score keys.
A partial replay is assigned only when its canonical constraints, total stage
count, and recorded ordered prefix resolve to one distinct current content key.
Ambiguous or corrupt rows remain unkeyed.

Replay listing uses `course_key`, with numeric fallback only for rows whose key
is empty. Loading a replay by replay-row ID remains unchanged.

## Recovery flow

`ChartDBHelper` exposes current course definitions containing navigation ID,
canonical key, normalized constraints, and ordered SHA-256/MD5 identities.
Main-menu profile/cache refresh runs an idempotent recovery pass before loading
course lamps and replay summaries:

1. Score migration canonicalizes all legacy nonempty score keys.
2. Exact or strongest-common current-definition matches normalize the stored
   key and may update the non-authoritative `course_id` for diagnostics.
3. Replay migration/backfill handles complete rows.
4. The recovery pass resolves partial replay rows conservatively against the
   current definitions and score evidence.

Each profile database is repaired in its own transaction under its existing
singleton connection and profile database activity guard. There is no
cross-database transaction or second connection to a profile database. Failure
in one database rolls back that database and leaves all original rows intact.
Reads still work for already-canonical records.

Profile initialization and archive import must run the supported v5-to-v6
score and v3-to-v4 replay migrations before strict "current version"
validation. Databases newer than the application remain rejected.

Rows without enough evidence are never guessed, deleted, or assigned merely by
course name. They keep their legacy ID fallback when that ID still exists.

## Error handling and performance

- Schema/data migrations are atomic and bump the database user version only
  after successful backfill.
- Future-version databases remain read-rejected without mutation.
- Invalid hashes, malformed legacy keys, missing replay stages, and ambiguous
  candidates are counted/logged and left untouched.
- Recovery is idempotent and scans only noncanonical, unkeyed, or stale-ID
  course rows. Normal startup performs indexed key reads.
- The existing numeric-ID preservation remains as compatibility protection and
  for hash-enrichment continuity, but no authoritative record lookup depends on
  it.

## Verification

Tests must prove the red/green behavior for:

- identity stability across rename, metadata changes, constraint formatting,
  and table refresh;
- identity changes for chart order/content and gameplay-constraint changes;
- legacy score-key migration, wildcard LN-mode recovery, and key-based lamps;
- no numeric fallback for a nonempty mismatching score/replay key;
- complete replay backfill and unique partial-replay recovery;
- ambiguous/corrupt recovery remaining untouched;
- schema transaction rollback and future-version immutability;
- main-menu course lamps and replay listing surviving changed navigation IDs.

The final gate is all application CTest entries (with Yoga excluded), desktop
`main`, and the unsigned iOS build-only check.
