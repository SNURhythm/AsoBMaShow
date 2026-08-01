# IR Checkbox and Native Clear Synchronization Design

## Goal

Polish the IR upload selection control and keep the iOS UIKit text editor in
sync when a common `TextInputBox` clear button is pressed.

## IR Upload Checkbox

The IR upload queue continues to use the shared `CheckboxButtonContent`, but
its selection button becomes an icon-only hit target rather than a tinted
container around a smaller icon.

- The 34 by 34 selection button keeps its existing hit target and position.
- Its normal, hover, and pressed backgrounds are transparent, with no visible
  outer square or border.
- The FontAwesome checkbox glyph increases to 30 pixels so the
  glyph itself is the dominant visual.
- The checked glyph uses the cyan selection tint.
- The unchecked glyph uses the muted text tint.
- Selection behavior, locking, callbacks, and the existing hit target remain
  unchanged.
- The Club Beat controls retain their current labeled checkbox styling.

The icon size remains a constructor-level choice on
`CheckboxButtonContent`, so the IR instance can be enlarged without changing
the shared component's other consumers.

## UIKit Text Synchronization

`TextInputBox` remains the source of truth for editing text. The iOS native
editor bridge gains an operation that updates the active `UITextField` only
when its context matches the requesting `TextInputBox`.

The bridge accepts the complete text plus selection start and end offsets. It
runs immediately on the UIKit main thread or dispatches asynchronously to the
main thread when needed. The native editor converts the supplied UTF-8 byte
offsets through the same UTF-8-to-UTF-16 selection logic already used by
`SetIOSNativeTextEditorSelection`.

Programmatic `TextInputBox::setEditingText()` synchronizes the native editor
when it is visible. The embedded clear button clears the C++ editing state,
resets the cursor and selection, updates the visible `UITextField` to an empty
string, and then emits the existing single text-change callback. Updating the
native field programmatically must not synthesize another editing-changed
callback, so consumers continue to receive exactly one notification.

If no native editor is visible or its context belongs to another input, the
bridge is a no-op. Clearing does not dismiss the UIKit editor or keyboard.

## Error and Lifecycle Handling

- Native synchronization is guarded by the existing editor context pointer,
  preventing a stale input from changing a newer editor.
- Empty or invalid UTF-8 conversion results use an empty UIKit string rather
  than leaving stale native text.
- Selection offsets are clamped to valid UTF-8 boundaries before UIKit range
  conversion.
- Existing editor show, submit, finish, and hide behavior remains unchanged.

## Verification

Regression coverage will verify:

- the IR queue uses a larger checkbox glyph without a tinted outer button;
- selected and unchecked IR checkbox states tint the glyph itself;
- clearing still emits one normal `TextInputBox` text-change callback and
  hides the embedded clear icon;
- the iOS bridge exposes context-scoped text-and-selection synchronization;
- the native editor applies programmatic text and selection without closing;
- desktop tests and the `main` target still pass; and
- the iOS build-only path compiles the C++/Objective-C++ bridge integration
  without uploading a build.

## Out of Scope

- Changing Club Beat checkbox visuals.
- Changing the UIKit editor's built-in clear button appearance.
- Dismissing the keyboard after clearing.
- Enabling clear buttons for additional text fields.
