"""Compile the preview's production renderer synchronization into the renderer test."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--restart-only', action='store_true')
args = parser.parse_args()
source = (args.root / 'src/scene/SettingsScenePreview.cpp').read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
signatures = ('void SettingsScene::resetPreviewSimulation()',) if args.restart_only else (
    'PlayfieldPresentationConfig\npreviewPresentationConfiguration(',
    'void SettingsScene::syncPreviewPresentationConfiguration()',
    'void SettingsScene::syncPreviewInputLayout()',
    'void SettingsScene::syncPreviewTouchLayout()',
    'void SettingsScene::syncPreviewAuthority()',
    'void SettingsScene::resetPreviewHudSample()',
    'void SettingsScene::publishPreviewJudgement(',
    'void SettingsScene::consumePreviewTransactions(',
    'void SettingsScene::advancePreviewPlayback(float dt)',
    'void SettingsScene::advancePreviewSimulation()',
    'void SettingsScene::capturePreviewVisualState()',
    'bms_parser::Note *SettingsScene::pressLane(int lane, double inputDelay)',
    'bms_parser::Note *SettingsScene::pressLane(int mainLane, int compensateLane,',
    'bms_parser::Note *SettingsScene::releaseLane(int lane, double inputDelay,',
)
methods = '\n'.join(extract(source, signature) for signature in signatures)
if not args.restart_only:
    controls = (args.root / 'src/scene/SettingsSceneControls.cpp').read_text()
    helpers = extract(controls, 'View *makeAppearanceColorPresets(')
    helpers += '\n' + extract(controls, 'class ScratchNoteSample') + ';'
    layout = (args.root / 'src/scene/SettingsSceneLayout.cpp').read_text()
    helpers += '\n' + extract(layout, 'void styleGameplaySkinChoiceButton(')
    methods = helpers + '\n' + methods
    for signature in (
        'void SettingsScene::closeAppearanceColorPopup()',
        'void SettingsScene::syncAppearanceColorPopup()',
        'void SettingsScene::appendAppearanceColorPicker(',
        'void SettingsScene::appendBuiltInNoteControls(',
        'void SettingsScene::appendBuiltInJudgeLineControls(',
        'void SettingsScene::appendBuiltInMeasureLineControls(',
        'void SettingsScene::appendBuiltInLanePercentControl(',
    ):
        methods += '\n' + extract(controls, signature)
args.output.write_text(methods)
