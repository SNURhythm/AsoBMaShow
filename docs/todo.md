# TODO

## Gameplay-skin chart information

- Preserve Beatoraja-equivalent `SongInformation` density analysis in Aso's
  immutable chart state, then wire Lua integer properties 360–365 and 368 to
  that data. Until this exists, `PlaySkinStateBridge` correctly returns
  Beatoraja's `Integer.MIN_VALUE` sentinel for these properties when chart
  information is unavailable.
