# Find BMS Collision-Resistant Storage Identity

**Date:** 2026-07-21

**Status:** Approved design

**Implementation branch:** `feature/skip-unzip`

## Context

Find BMS currently derives both the extracted destination and retained archive
name from the downloaded archive's display name. Two unrelated packages named
`song.zip` therefore share `BMSSEARCH/song` and
`BMSSEARCH/_archives/song.zip`. A later download can merge with, overwrite, or
delete files belonging to the earlier song. The retained-archive path also
loses its supported extension when a long display name is truncated, and
alternate cleanup only targets the current extension instead of every archive
representation for the same storage identity.

The current branch also treats every stored `Documents/BMS` entry as built in.
That is correct for the synthesized Android fallback, but on desktop the row
can be an ordinary folder explicitly added by the user and must remain
removable.

## Goals

- Give every Find BMS package a deterministic storage identity so unrelated
  same-named packages never share an extracted folder or retained archive.
- Keep generated paths readable while adding enough identity entropy to make
  accidental collisions impractical.
- Preserve the supported archive extension after all sanitization and length
  limiting so retained archives remain discoverable by `ChartLibraryScanner`.
- Treat `.zip`, `.7z`, and other supported archive variants with the same
  storage identity as alternate representations and remove stale variants on
  a successful commit.
- Treat the stable ID suffix, rather than the readable base, as the cleanup
  identity so provider-side filename changes do not leave duplicate downloads.
- Never promote a package-source candidate to automatic download unless the
  chosen package archive extension is supported by the current build.
- Keep stale-alternate removal best effort after the destination commit.
- Keep desktop folders explicitly added at `Documents/BMS` removable while
  retaining Android's synthesized built-in fallback behavior.

## Non-goals

- Migrating or renaming Find BMS downloads already stored by older builds.
- Deduplicating two searches that identify the same package through unrelated
  source identifiers.
- Changing chart hashes, scanner virtual paths, or library database identity.
- Adding a user-visible database or metadata sidecar for Find BMS packages.
- Resolving or replying to GitHub review threads automatically.

## Considered approaches

### Readable name plus stable ID suffix (chosen)

Store extracted packages as `<name>--<id>` and retained archives as
`<name>--<id><extension>`. This preserves human context, prevents same-name
collisions, works with the current recursive scanner, and gives packed and
extracted forms one shared identity.

### ID-only paths

Using only the source ID would be slightly simpler, but library folders and
debugging output would become opaque. It provides no meaningful safety or
scanner advantage over the chosen suffix format.

### Nested name and ID folders

Using `<name>/<id>` would keep both values visible, but would change the
existing destination layout, complicate packed/extracted alternate matching,
and create extra scanner-visible hierarchy without improving identity.

## Storage identity

The download boundary selects one stable identity seed in this order:

1. The source package/file ID when the provider exposes one. Horie downloads
   use the selected candidate's file ID, including title-only searches with no
   chart hash.
2. The requested chart's normalized SHA-256 or MD5 key for sources that do not
   expose a package ID.
3. The stable display/download URL when neither source ID nor chart hash is
   available.

Hash the complete seed with SHA-256 and use the first 16 lowercase hexadecimal
characters as the visible suffix. Hashing normalizes arbitrary provider IDs
and URLs into a filesystem-safe, fixed-width 64-bit identifier. The full seed
is never used as a path component.

Different IDs intentionally produce different storage paths even when their
display archive names are identical. Repeating a download with the same name
and ID targets the same storage path and retains the current overwrite/merge
semantics.

The readable base is not part of identity. If the provider later renames the
same package, the new destination may use the new base, but post-commit cleanup
matches the unchanged `--<16-hex identity>` suffix and removes the old named
representation.

## Filename construction

Determine the archive extension from the preferred display name when it still
contains a recognized archive suffix; otherwise use the extension already
detected from the source URL or response metadata. Sanitize the extension-free
display base with the existing Find BMS filename rules.

The storage key is:

`<sanitized base>--<16-hex identity>`

The retained archive filename is:

`<storage key><archive extension>`

Limit the complete retained filename to 128 bytes by shortening only the
sanitized display base. Never truncate the `--<id>` suffix or archive
extension. If the display base becomes empty, use `archive`.

The staging download uses the final retained archive filename, so archive
readers continue to receive a supported suffix even when the original display
name was long or unsafe.

## Alternate representation cleanup

The extracted destination remains `BMSSEARCH/<storage key>`. The retained
destination becomes `BMSSEARCH/_archives/<storage key><extension>`.

After a successful destination swap, remove the opposite extracted/archive
form and enumerate both the download root and `_archives`. Extract the stable
ID only from storage keys ending in `--` plus exactly 16 lowercase hexadecimal
characters. Remove other extracted directories and supported archive files
whose extension-free filename has the same stable ID. Exclude `_archives` and
the newly installed destination. This removes a previous `.7z` when the
current result is `.zip`, the reverse, and an older provider filename for the
same package.

All candidate paths are derived from the already validated `downloadRoot` and
`storageKey`. Directory enumeration and removal errors remain best effort and
must not turn an installed download into a failure or pending decision.

## Package archive support

Package APIs can provide a filename even when their download URL has no
extension. Finalize the candidate using the extension on that chosen package
filename. An unsupported candidate is promoted to automatic download only
when `isSupportedArchiveExtension()` accepts that extension for the current
build. A recognized but unsupported package extension remains unsupported,
keeps `knownUnsupportedArchive`, and reports the existing manual-source reason
instead of reaching extraction and failing there.

Put the finalization policy behind a focused function that accepts the support
decision as an input. Production supplies the real build capability; tests can
exercise the ZIP-only result on a development machine that has libarchive.

## Desktop fallback removability

Only Android synthesizes `DefaultBmsFolderPath()` as an effective built-in
entry. Limit the `removable = false` treatment for a stored default-path row to
Android. Desktop and iOS rows remain ordinary explicit entries and can be
deleted through Settings. Android keeps both its existing deletion guard and
the synthesized fallback when the row is absent.

## Error handling and compatibility

- An empty identity seed falls back to the best available URL before filename
  construction; storage naming never intentionally emits an ID-less key.
- Unsafe display characters affect only the readable base, not the identity
  suffix or extension.
- Existing downloads are not migrated. A first download after upgrade may
  coexist with a legacy unsuffixed path; subsequent same-ID downloads converge
  on the new path.
- Stale-variant cleanup accepts only supported archive extensions and matches
  either the exact legacy storage key or a validated `--<16-hex-id>` suffix.
- A cleanup failure leaves the committed result successful and allows the
  automatic library refresh to run.

## Verification

Automated coverage will verify:

- same display name plus different identity seeds produces different storage
  keys and retained archive names;
- the same display name and identity reproduces the same paths;
- generated storage keys contain a 16-hex suffix;
- long `.zip` and compound `.tar.gz` names retain their complete extension and
  fit the 128-byte limit;
- unsafe names cannot escape the Find BMS destination;
- extracting a `.zip` removes an older `.7z` with the same storage key;
- downloading the same stable ID under a renamed archive removes the older
  extracted folder and retained archive without touching a different ID;
- package candidates with a recognized but build-unsupported extension remain
  unsupported, while a build-supported extension can still override an
  extensionless package URL;
- current-extension and extracted alternates still clean up;
- cleanup failures remain successful commits;
- a desktop stored `Documents/BMS` entry remains removable, while the Android
  synthesized fallback remains non-removable;
- the Find BMS source-flow audit confirms Horie passes its file ID and other
  sources provide a chart-hash or URL fallback;
- all CTest targets and the iOS build-only path pass.
