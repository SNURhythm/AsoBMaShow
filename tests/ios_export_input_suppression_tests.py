#!/usr/bin/env python3
"""Exercise the real iOS export boundaries and producer pump without UIKit."""
from pathlib import Path
import subprocess
import tempfile

from support.fixture_compiler import FixtureCompiler

root = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def generate(source):
    runtime = source[source.index("struct Runtime {"):source.index("bool SDLCALL lifecycleWatch")]
    pump_start = source.index("const auto pumpGeneration =")
    pump = source[pump_start:source.index("#ifndef NDEBUG", pump_start)]
    methods = "\n".join(function(source, signature) for signature in (
        "void BeginIOSReplayExport(", "void EndIOSReplayExport(",
        "void ResumeIOSGameplayTouchInput(", "bool PollIOSApplicationEvent(",
    ))
    fixture = (root / "tests/ios_export_input_suppression_fixture.cpp").read_text()
    return fixture.replace("RUNTIME_DEFINITIONS", runtime).replace("RUNTIME_METHODS", methods).replace("PRODUCER_PUMP", pump)


def run(source):
    compiler = FixtureCompiler.from_environment()
    with tempfile.TemporaryDirectory(prefix="ios-export-input-") as directory:
        cpp = Path(directory) / "boundary.cpp"
        binary = Path(directory) / ("boundary" + compiler.executable_suffix)
        cpp.write_text(generate(source))
        compiler.build([cpp, root / "src/platform/ApplicationEventQueue.cpp"], binary,
                       directory, standard=23, includes=(root / "src", root / "SDL/include"))
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    run((root / "src/platform/IOSApplicationRuntime.mm").read_text())
    print("iOS export input suppression boundary passed")
