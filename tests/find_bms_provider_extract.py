"""Compile production provider selection against controlled network effects."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
packages = (args.root / "src/bms_search/PackageSourceDrivers.cpp").read_text()
service = (args.root / "src/BmsSearchService.cpp").read_text()
args.output.write_text(
    "namespace asobmshow::bms_search {\n" +
    extract(packages, "PackageSourceLookupResult WriggleDriver::lookupByMd5(") + "\n" +
    extract(packages, "bool EndlessDreamSourcesDriver::tryDownloadByMd5(") +
    "\n}\nusing namespace asobmshow::bms_search;\n" +
    extract(service, "std::string BmsSearchService::patternUrlForSha256(") + "\n" +
    extract(service, "BmsSearchResult BmsSearchService::findAndDownload(") + "\n")
