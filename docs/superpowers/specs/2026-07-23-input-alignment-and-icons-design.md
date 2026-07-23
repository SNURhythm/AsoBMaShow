# Input Alignment and FontAwesome Controls Design

## Goal

Align text consistently inside input boxes, give selected text fields an
embedded clear action, and render checkbox state with the bundled FontAwesome
font instead of Unicode checkbox characters.

## Text Input Layout

`TextInputBox` will apply 12 pixels of horizontal content padding by default.
Text, caret, selection, pointer-to-cursor mapping, composition underlines, and
the native IME rectangle will all use the same padded content rectangle.
Symmetric default padding keeps centered numeric inputs centered while giving
left-aligned inputs the missing leading inset.

## Clearable Inputs

`TextInputBox` will expose `setClearable(bool)`. When enabled, the component
owns an embedded clear button using the FontAwesome x-mark glyph. The button is
visible only when the field contains text. While visible, the input reserves
enough trailing space to prevent text and caret overlap.

Activating the button clears the editing value, keeps the behavior inside the
common component, and invokes the normal text-change callbacks so consumers
update filters or state immediately. Pointer and touch events handled by the
button do not fall through to text selection.

The property will be enabled for:

- the main-menu chart search;
- the Music Player library search;
- the Music Player favorites search; and
- the difficulty-table URL field.

The two standalone Music Player search `Clear` buttons will be removed because
their behavior moves into the input component.

## Checkbox Icons

Checkbox state will use FontAwesome square and square-check glyphs. Text labels
remain in the normal UI font, so controls that combine an icon and label use
separate text views inside one button.

The migration covers:

- IR upload queue selection buttons;
- the Play Options Club Beat button; and
- the Music Player Club Beat button.

The existing FontAwesome chart-filter checkbox implementation is the visual
and code reference.

## Verification

Focused tests will cover default input padding, clear-button visibility and
callback behavior, clear-button event consumption, and the expected
FontAwesome checkbox glyphs/content structure. Existing IR upload and Play
Options view tests will be extended where appropriate. After focused tests,
the desktop `main` target will be built through the existing
`cmake-build-debug` directory.
