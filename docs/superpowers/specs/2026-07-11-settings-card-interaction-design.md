# Settings Card Interaction Design

## Goal

Make Settings controls act on the card that visually owns them, and ensure a binding conflict is immediately visible regardless of scroll position.

## Root cause

The Profile tab has one scene-wide name draft rendered in the top card, but every profile row reads it for Rename and Duplicate. The same top card also cancels delete or overwrite confirmation owned by a specific profile UUID. In the Input tab, binding-conflict confirmation is inserted as ordinary scroll content above the binding list while the previous scroll offset is preserved, so it can remain outside the viewport.

## Profile interaction design

- The top Player Profiles card owns only the new-profile name draft and Create action.
- Rename and Duplicate start one inline editor inside the initiating profile card. Its state contains the action, target profile UUID, and private name draft.
- Rename starts with the current display name. Duplicate starts with `<current name> Copy`.
- Apply always invokes the stored action for the stored UUID; it never depends on current selection or another card's text.
- Cancel clears only the inline editor. A successful Apply closes it; a failed Apply keeps it open so the user can correct the draft.
- Only one inline profile editor can be active. Starting another replaces the previous editor intentionally.
- Delete and overwrite confirmations, explanatory text, and Cancel appear only in the target profile card identified by `confirmationProfileId()`.
- While a confirmation is pending, unrelated profile actions are disabled so selection cannot silently cancel it.
- General profile status is rendered as tab-level feedback rather than as content owned by the Portable Archive card.
- The controller's UUID-bound mutation and confirmation behavior remains unchanged.

The selected on-demand inline editor avoids both alternatives: always-visible per-card inputs would add substantial clutter, while a selected-profile editor in the top card would retain the cross-card relationship.

## Binding-conflict design

- `AwaitingConflictConfirmation` renders a full-window `BlockingOverlayView` attached directly to the Settings root, not a scroll card.
- The centered panel lists each conflicting assignment and provides Replace and Keep Existing actions using the current controller callbacks.
- The overlay blocks mouse, touch, wheel, keyboard, and text-entry events from reaching controls below it.
- Opening and closing the overlay preserves the Input tab's scroll offset.
- Capture persistence and conflict resolution remain transactional and unchanged.

## State and lifecycle

- Inline editor state lives outside rebuilt view objects and is keyed by the stable profile UUID.
- The editor is cleared when its target disappears, when leaving the Profile tab, or when a profile operation makes editing unavailable.
- Overlay pointers are reset with other Settings view pointers and resized with the window.
- Tab changes continue cancelling active input capture and profile confirmation through existing controller paths.

## Tests

- A focused profile-editor state test proves drafts and Apply targets cannot cross profile UUIDs, state survives a layout rebuild, Cancel is local, and missing targets are cleared.
- Profile controller tests continue proving delete and overwrite confirmation remain UUID-bound and cancellation performs no mutation.
- A view test proves a visible `BlockingOverlayView` prevents pointer, wheel, keyboard, and text-input events from reaching an underlying control; hiding it restores interaction.
- Existing input-capture tests continue proving Replace and Keep Existing are transactional.
- Verification includes the focused tests, Settings/input/profile CTest subset, desktop `main` build, and full CTest suite.

## Acceptance criteria

- Text entered for Create is never consumed by Rename or Duplicate.
- Rename and Duplicate operate only on the profile card where their inline editor is open.
- Confirmation and Cancel are visible only on their target profile card; other rows cannot cancel them implicitly.
- Binding conflicts are centered and visible at every scroll position, and underlying controls cannot be activated until the conflict is resolved.
- Existing profile data, input bindings, and controller persistence formats require no migration.
