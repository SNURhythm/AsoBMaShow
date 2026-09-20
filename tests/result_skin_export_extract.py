import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (args.root / 'src/scene/ResultScene.cpp').read_text()
start = source.index('void ResultScene::exportPhoto() {')
end = source.index('void ResultScene::setResultPhotoExportPresentation(', start)
normal_start = source.index('    const bool rendered = resultSkinSession->render(',
                            source.index('void ResultScene::renderScene()'))
normal_end = source.index('    appendResultSkinRenderDiagnostics();', normal_start) + len('    appendResultSkinRenderDiagnostics();')
fixture = (args.root / 'tests/result_skin_export_fixture.cpp').read_text()
exporter = (args.root / 'src/ResultImageExporter.cpp').read_text()
draw_start = exporter.index('  bool rendered = true;', exporter.index('ResultImageExportResult renderResultImageWithSkinData('))
draw_end = exporter.index('  bgfx::blit(', draw_start)
args.output.write_text(fixture.replace('ASOBMS_EXPORT_PHOTO_METHOD', source[start:end])
                      .replace('ASOBMS_NORMAL_SKIN_RENDER', source[normal_start:normal_end])
                      .replace('ASOBMS_EXPORT_DRAW', exporter[draw_start:draw_end]))
