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

- Implement Beatoraja's configurable Lift and Hidden planes, then apply the
  `isChangeLift` selector toggled by a short Start+Select conjunction. Pinned
  source `ControlInputProcessor.java` at
  `c2ed5db1a46145ed10790c3872f717e95b59db9d` uses that selector only when
  lane cover is disabled and both Lift and Hidden are enabled. AsoBMaShow
  currently has neither configuration nor renderer plane, so the input edge
  is preserved by `StartSelectControl` but intentionally has no visual effect.

## Gameplay-skin rhythm timer

- Capture Beatoraja `TIMER_RHYTHM` (140) exactly from
  `play/RhythmTimerProcessor.update`, including its `rhythmtimer` accumulator,
  section-line reset, current BPM, and play-speed inputs. The current bridge
  leaves timer 140 off rather than inventing an approximate measure pulse.
