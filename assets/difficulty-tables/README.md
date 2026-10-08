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

The script reads the default URLs from `ChartLibraryOperations.cpp` and only
replaces the snapshot after every download succeeds. Review the resulting
snapshot before committing it. Desktop, iOS, and Android package this directory
through their existing shared-assets rules.

Before showing the initial UI, startup imports missing source URLs from the
bundle if the first-launch online update has not completed. Existing tables
always win. Snapshot import does not set `defaultDifficultyTablesSeeded`:
the normal background downloads still refresh these same sources and persist
completion only after all eight succeed. Offline/failed updates leave snapshots
available and retry on a later library refresh. Once the online seed completes,
startup does not restore tables the user has subsequently deleted.

Existing installations also receive the two Aery snapshots once, tracked by
`aeryDifficultyTablesSeeded`. This upgrade imports only those sources and
preserves installed copies. It does not restore other deleted default tables.
After the upgrade and online seed complete, deleted Aery tables stay deleted.
