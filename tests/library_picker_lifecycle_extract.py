"""Compile complete platform picker methods with controlled native effects."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/library/ChartLibraryPlatform.cpp").read_text()
    header = (args.root / "src/library/ChartLibraryPlatform.h").read_text()
    fixture = (args.root / "tests/library_picker_lifecycle_fixture.cpp").read_text()
    start = source.index("struct SoundSetFolderPicker::Impl {")
    end = source.index("} // namespace chart_library_platform", start)
    sound_picker = source[start:end].replace(
        "std::atomic_bool pickerActive = false;", "ObservedActive pickerActive = false;")
    fixture = fixture.replace("SOUND_PICKER_DECLARATIONS", "\n".join(
        extract(header, signature) + ";" for signature in (
            "struct SoundSetFolderPick", "class SoundSetFolderPicker final")))
    fixture = fixture.replace("SOUND_PICKER_METHODS", sound_picker)
    fixture = fixture.replace("IMPORT_METHOD", extract(source, "void requestImport(bool folder)"))
    fixture = fixture.replace("FOLDER_METHODS", "\n".join(extract(source, signature) for signature in (
        "FolderActionService::~FolderActionService()",
        "void FolderActionService::requestAddFolder()",
        "void FolderActionService::requestImportArchive()",
        "bool FolderActionService::active() const noexcept")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
