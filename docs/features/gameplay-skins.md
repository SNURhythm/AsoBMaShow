# Gameplay skins

## Intent and user flow

Desktop builds can load Beatoraja-compatible Lua gameplay skins alongside the
built-in playfield. Users manage packages and configuration in Settings, select
an eligible skin for a chart, and receive an explicit failure page when a
selected skin cannot safely prepare or render. Android intentionally defaults
this optional feature off.

Each custom gameplay skin has a **FAST/SLOW** dropdown with the same judgement
thresholds as the built-in skin: PGREAT or below, GREAT or below, GOOD or below,
BAD or below, and Off. The default is GREAT or below, including for existing
profiles, so PGREAT does not show an early/late label. The choice is saved per
skin, gameplay mode, and presentation orientation. It filters the compatible
skin's early/late conditions for all three judge regions; the skin's own display
switch and animation still apply. Numeric millisecond timing is unaffected.

## Code map

- `src/skin/` owns feature gating, selection, lifecycle, settings, package
  paths, commits, safety policy, and presentation types.
- `src/skin/beatoraja/` implements Lua host bindings, manifest validation,
  model normalization, resource handling, and 2D rendering.
- `src/scene/play/GameplaySkinSessionFactory.*` and presentation coordinator
  code create chart-lifetime sessions without giving the scene direct renderer
  ownership.
- `src/scene/GameplaySkinSettings*` presents package and configuration work.

## Boundaries and invariants

Package preparation and commit are transactional; profile configuration writes
are queued and durable. Lua/runtime file access follows the selected package
root and current safety policy. A selected-skin failure is distinct from an
intentional built-in selection: it must not silently change the user's chosen
presentation. Skin sessions consume prepared playfield state and do not become
the authority for judging or result persistence.

## Verification

Use the `lua_skin_*`, `play_skin_*`, `skin_*`,
`gameplay_skin_*`, and `beatoraja_skin_*` test targets. External compatibility
and acceptance evidence remains in [the skin references](../skin-compat/).

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Settings and user interface](settings-and-user-interface.md)
- [Build, release, and verification](build-release-and-verification.md)
