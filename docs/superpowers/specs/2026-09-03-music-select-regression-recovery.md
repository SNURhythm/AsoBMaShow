# Music-select regression recovery

## Goal

Restore four regressed selector behaviors without adding compatibility policy:

1. A touched skin `StringWriter` search field presents a correctly framed
   native editor and receives input.
2. Song-select sorting follows the pinned Beatoraja `BarManager` and
   `BarSorter` semantics at the root and in directories.
3. Difficulty-table folder order remains the metadata order while the songs
   inside a table level continue to use the selected skin sort.
4. The selector's retained Records modal exposes the same replay actions as
   the Main Menu Records modal, including watch and video export.

## Compatibility evidence

`/Users/xf/workspace/SNURhythm/beatoraja` at
`c2ed5db1a46145ed10790c3872f717e95b59db9d` is authoritative.

- `BarManager.updateBar` initializes `isSortable` to `true`, so the root is
  sorted. A selected directory then supplies its own `isSortable` value.
- `TableBar` and `HashBar` inherit `DirectoryBar.isSortable == true`.
- `BarSorter` sorts only `SongBar` and `FolderBar`; pairs of other bar classes
  compare equal and therefore keep their authored order under the stable
  sorter. This preserves `TableBar` and `HashBar` order while allowing
  `HashBar` song children to sort.

## Design

`TextInputBox::beginEditing` will apply its current layout before the native
editor is started. The skin selector already positions its transient input and
forwards the initiating touch; applying the layout makes its declared bounds
the native input rectangle before that touch reaches the platform editor.

`MusicSelectBarManager` will use the source's default root-sort behavior and
the source comparator's participating bar classes. Repository projection will
keep difficulty table and hash directories sortable, as the source classes
are; stable comparisons preserve the metadata order of their non-song child
bars.

The existing Main Menu Records modal will be moved behind a retained shared
component. It owns record loading, filtering, selection, the action controls,
and operation progress. Its owner supplies scene-transition callbacks, so both
Main Menu and Music Select keep their own retained-scene return target while
using one Records UI and action contract.

No resource limit, parser validation, fallback, or new skin compatibility
rule is introduced.

## Verification

- Text-input regression test asserts editing begins with the latest declared
  input rectangle.
- Bar-manager and projection tests assert Beatoraja root ordering, stable
  difficulty-folder ordering, and sorted HashBar song children.
- Records-modal tests assert selected replay actions are exposed through the
  shared owner callbacks.
- Run focused tests, the desktop build, then the full CTest suite.
