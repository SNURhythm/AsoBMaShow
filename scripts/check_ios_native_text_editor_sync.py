#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
header_path = root / "src/iOSNatives.hpp"
native_path = root / "src/iOSNatives.mm"
text_input_path = root / "src/view/TextInputBox.cpp"
header = header_path.read_text(encoding="utf-8") if header_path.is_file() else ""
native = native_path.read_text(encoding="utf-8") if native_path.is_file() else ""
text_input = (
    text_input_path.read_text(encoding="utf-8") if text_input_path.is_file() else ""
)
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


require(
    "void SetIOSNativeTextEditorState(" in header,
    "the iOS bridge must expose complete native editor state updates",
)
require(
    "- (void)setState:(const IOSNativeTextEditorState &)state;" in native,
    "the native editor view must accept programmatic text and selection",
)
require(
    "_textField.text = text != nil ? text : @\"\";" in native
    and "[self setSelectionStart:state.selectionStart end:state.selectionEnd];"
    in native,
    "the native editor must apply both programmatic text and selection",
)
require(
    "[gNativeTextEditor context] != editorContext" in native
    and "[gNativeTextEditor setState:editorState];" in native,
    "native editor updates must be scoped to the requesting input",
)
require(
    text_input.count("syncNativeTextEditorText();") >= 2,
    "programmatic editing and clear-button paths must sync UIKit text",
)

set_editing_start = text_input.find("void TextInputBox::setEditingText(")
set_editing_end = text_input.find("size_t TextInputBox::getNextUnicodePos(")
set_editing_body = (
    text_input[set_editing_start:set_editing_end]
    if set_editing_start >= 0 and set_editing_end > set_editing_start
    else ""
)
require(
    "syncNativeTextEditorText();" in set_editing_body
    and "hideNativeTextEditor(false);" not in set_editing_body,
    "programmatic text updates must sync the visible editor without hiding it",
)

clear_start = text_input.find("void TextInputBox::clearFromButton()")
clear_end = text_input.find("void TextInputBox::updateCompositionGeometry(")
clear_body = (
    text_input[clear_start:clear_end]
    if clear_start >= 0 and clear_end > clear_start
    else ""
)
require(
    "syncNativeTextEditorText();" in clear_body
    and clear_body.find("syncNativeTextEditorText();")
    < clear_body.find("refreshDisplay(true);"),
    "clear-button updates must reach UIKit before publishing the text change",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("iOS native text editor synchronization audit passed")
