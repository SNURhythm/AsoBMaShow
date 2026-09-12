import argparse
from pathlib import Path


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"Unterminated function: {signature}")


parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/bms_search/DownloadSupport.cpp").read_text()
args.output.write_text("\n\n".join(extract(source, signature) for signature in [
    "bool ensureDownloadDirectory(",
    "std::optional<std::filesystem::path> saveIosDebugArtifacts(",
    "std::filesystem::path makeDownloadDirectory(",
    "bool downloadAndExtractArchive(",
]))
