"""Compile production owner recovery and pointer handlers with inert UI geometry."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
fixture = (args.root / 'tests/ui_input_overflow_fixture.cpp').read_text()
for marker, path, signature in (
    ('SCENE_RECOVERY', 'src/scene/Scene.h', '  virtual void onInputQueueOverflow()'),
    ('BUTTON_HANDLER', 'src/view/Button.cpp', 'bool Button::handleEventsImpl('),
    ('SCROLL_CANCEL', 'src/view/ScrollView.cpp', 'void ScrollView::onPointerInputCancelled()'),
    ('CAPTURE_ACTIVATE', 'src/input/InputCaptureController.cpp', 'void InputCaptureController::considerControlActivation('),
    ('BUTTON_CONSUMED', 'src/view/Button.cpp', 'void Button::onPointerEventConsumed('),
):
    fixture = fixture.replace(marker, extract((args.root / path).read_text(), signature))
for marker, path, signature in (
    ('VIEW_CANCEL', 'src/view/View.h', '  void cancelPointerInput()'),
    ('CAPTURE_RESET', 'src/input/InputCaptureController.cpp', 'void InputCaptureController::resetInputState()'),
    ('INTRO_CANCEL', 'src/scene/IntroScene.cpp', 'void IntroScene::onInputQueueOverflow()'),
    ('BUTTON_CANCEL', 'src/view/Button.cpp', 'void Button::onPointerInputCancelled()'),
):
    fixture = fixture.replace(marker, extract((args.root / path).read_text(), signature))
for marker, path in (
    ('RECYCLER_CANCEL', 'src/view/RecyclerView.h'),
    ('PORTAL_CANCEL', 'src/view/OverlayPortal.h'),
):
    fixture = fixture.replace(marker, extract((args.root / path).read_text(), '  void onPointerInputCancelled() override'))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(fixture)
