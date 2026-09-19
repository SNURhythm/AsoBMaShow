import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (args.root / 'src/scene/ResultScene.cpp').read_text()
start = source.index('void ResultScene::loadPreviousBest() {')
end = source.index('void ResultScene::loadDifficultyLabel()', start)
replay_start = source.index('void ResultScene::startCourseReplay() {')
replay_end = source.index('void ResultScene::startCourseReplayStage(', replay_start)
gameplay = (args.root / 'src/scene/play/GamePlayScene.cpp').read_text()
restart_start = gameplay.index('  session->currentIndex = 0;',
                               gameplay.index('bool GamePlayScene::restartCourseFromBeginning()'))
restart_end = gameplay.index('  session->playOption.reset();', restart_start)
args.output.write_text(source[start:end] + source[replay_start:replay_end] +
                      'void resetCourseForReplayRestart(CoursePlaySession *session) {\n' +
                      gameplay[restart_start:restart_end] + '}\n')
