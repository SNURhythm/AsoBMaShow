# Default difficulty table snapshots

`defaults.json` contains the headers and chart data captured from the eight
default table sources. Each entry records its original page, header, and data
URLs; `captured_at` records the UTC download time (a per-table value overrides
the snapshot timestamp for later additions). Chart/music files are not
included. The original metadata is preserved, including courses and links.

Refresh from the repository root with:

```sh
python3 scripts/snapshot_default_difficulty_tables.py
```

The sources include Aery 5K and 7K at `https://asumatoki.kr/table/aery/header.json`
and `https://asumatoki.kr/table/aery7/header.json`. The script accepts both table
webpages and direct JSON headers.

The script reads the default URLs from `src/DifficultyTableDefaults.h` and only
replaces the snapshot after every download succeeds. Review the resulting
snapshot before committing it. Desktop, iOS, and Android package this directory
through their existing shared-assets rules.

The source registry assigns each table a seed revision: revision 1 contains
six original sources, and revision 2 adds Aery 5K and 7K. Add future sources
under a new revision without renumbering existing entries.

Startup imports pending revisions from the bundle before showing the initial
UI. Existing sources always win. The background library refresh separately
downloads pending revisions from their online sources. Both completion markers
live in application-wide state alongside the shared chart database; profiles
do not own them. Each marker advances only after its revision completes, so
failed downloads retry without repeating completed older revisions. Completed
online revisions also satisfy the bundled fallback if the snapshot was unavailable.

Legacy completion flags migrate to the appropriate revisions. Profile imports
clear those device-local flags, and older builds leave application state from
newer schema versions untouched. Once bundled and online seeding for a revision
complete, deleting one of its tables does not cause seeding to restore it.
