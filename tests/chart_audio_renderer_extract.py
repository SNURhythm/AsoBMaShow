import pathlib
import sys


def generate(root, output):
    source = (root / "src/audio/MusicPlayerService.cpp").read_text()
    start = source.index("void MusicPlayerService::AdjacentPreloadWorker(")
    end = source.index("void MusicPlayerService::StopPlaybackWorker()", start)
    method = source[start:end]
    fixture = (root / "tests/chart_audio_renderer_tests.cpp").read_text()
    if fixture.count("ASOBMS_ADJACENT_PRELOAD_METHOD") != 1:
        raise ValueError("Missing adjacent preload fixture anchor")
    output.write_text(fixture.replace("ASOBMS_ADJACENT_PRELOAD_METHOD", method))


if __name__ == "__main__":
    generate(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
