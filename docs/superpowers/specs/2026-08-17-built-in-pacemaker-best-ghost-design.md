# Built-in Pacemaker BEST Ghost Progression

## Goal

Make the built-in gameplay Pacemaker follow the saved-best replay progression
when `BEST` is selected and that replay has already been accepted for the
personal-best ghost channel.

## Scope

The change applies only to the built-in `BMSRenderer` presentation. Beatoraja
skin target properties continue to use their existing linear pacemaker target,
matching upstream Beatoraja.

## Data flow

`GamePlayScene` already loads the saved-best replay asynchronously and updates
`activeBestScoreTarget` when its score progression is valid. While `BEST` is
selected, it will publish this existing target and its derived snapshot in a
separate built-in Pacemaker authority. Before the replay finishes loading—or
when it is invalid—the target remains the ordinary linear BEST target.

`BMSRenderer` consumes the built-in authority. Skin presentation continues to
consume `pacemakerTarget` and `pacemakerStatus`, so no Beatoraja property
changes occur.

## Validation

Add a regression test with a BEST target whose replay progression differs from
its linear projection. It must show that the built-in authority switches to the
saved-best progression while the skin Pacemaker authority remains linear.
