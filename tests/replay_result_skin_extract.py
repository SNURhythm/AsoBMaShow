import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (args.root / 'src/ReplayVideoExporter.cpp').read_text()
start = source.index('class PreparedReplayResultPresentation {')
end = source.index('void populateReplayResultSkinData(', start)
fixture = (args.root / 'tests/replay_result_skin_fixture.cpp').read_text()
meta_start = source.index('bms_parser::ChartMeta courseResultMetaForReplayVideo(')
meta_end = source.index('RhythmState courseResultStateForReplayVideo(', meta_start)
image_source = (args.root / 'src/ResultImageExporter.cpp').read_text()
image_meta_start = image_source.index('bms_parser::ChartMeta courseResultMetaForReplay(')
image_meta_end = image_source.index('RhythmState courseResultStateForReplay(', image_meta_start)
args.output.write_text(fixture.replace('ASOBMS_RESULT_PRESENTATION', source[start:end])
                      .replace('ASOBMS_COURSE_META', source[meta_start:meta_end])
                      .replace('ASOBMS_COURSE_IMAGE_META', image_source[image_meta_start:image_meta_end]))
