# Ranking Modal Dismissal and Close Controls Design

## Goal

Make both Bokutachi ranking overlays require explicit dismissal and replace their text close buttons with compact Font Awesome xmark controls.

## Scope

- Apply the behavior to both the main ranking modal and its score-detail modal.
- Outside mouse and touch input must not close either modal.
- Input outside the modal must remain blocked so it cannot activate controls in the underlying scene.
- Escape and Android Back must continue to close the currently active modal.
- The explicit close control must perform the same action as the existing `Close` text button.
- Refresh, retry, row selection, and all other ranking behavior remain unchanged.

## Design

The ranking view's local modal scrim will stop inspecting pointer coordinates and panel bounds. It will retain only the close callback needed for Escape and Android Back. The scrim will continue consuming modal input, preserving the existing protection against interaction with the scene beneath it.

A local icon action-button helper will render Font Awesome Solid's xmark glyph (`0xf00d`) using the existing icon font utilities. The button will use the current action-button visual treatment in a compact 48-by-48-pixel square.

Both existing 96-pixel-wide `Close` text buttons will be replaced with this icon control:

- The main ranking modal's xmark invokes the existing ranking-modal close request.
- The score-detail modal's xmark invokes the existing score-detail hide action.

This remains a focused change inside the ranking modal view. It does not introduce a shared modal component or refactor unrelated action buttons.

## Interaction Rules

| Input | Main ranking modal | Score-detail modal |
| --- | --- | --- |
| Outside mouse/touch | No state change; input remains blocked | No state change; input remains blocked |
| Escape / Android Back | Close ranking modal | Close score-detail modal |
| Xmark button | Close ranking modal | Close score-detail modal |

## Verification

- Add a regression check that the modal scrim closes only for Escape or Android Back and no longer performs outside-pointer hit testing.
- Verify both modal headers use the Font Awesome xmark control.
- Run the ranking modal tests and source-audit checks.
- Build the desktop `main` target to catch integration errors.
