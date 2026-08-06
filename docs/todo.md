# TODO

## Gameplay-skin chart information

- Preserve Beatoraja-equivalent `SongInformation` density analysis in Aso's
  immutable chart state, then wire Lua integer properties 360–365 and 368 to
  that data. Until this exists, `PlaySkinStateBridge` correctly returns
  Beatoraja's `Integer.MIN_VALUE` sentinel for these properties when chart
  information is unavailable.

## Gameplay-skin play configuration

- If AsoBMaShow adds a selectable constant-speed play configuration, carry its
  enabled state through the gameplay frame authority and use it for Beatoraja
  boolean property 400 (`OPTION_CONSTANT`). The current app has no such
  configuration, so the bridge reports the upstream false state (and `-400`
  consequently reports true).
