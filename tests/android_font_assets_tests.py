"""Check font random-access support after assembling an Android APK.

Run with --apk android/app/build/outputs/apk/restricted_file_access/release/app-restricted_file_access-release.apk.
Compressed fonts cause AAsset_seek64 to repeatedly inflate large font files.
"""

import argparse
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    arguments = parser.parse_args()
    with zipfile.ZipFile(arguments.apk) as apk:
        fonts = [entry for entry in apk.infolist()
                 if entry.filename.startswith("assets/")
                 and entry.filename.lower().endswith((".ttf", ".otf", ".ttc"))]
        if not fonts:
            raise AssertionError("No packaged font assets found")
        compressed = [font.filename for font in fonts
                      if font.compress_type != zipfile.ZIP_STORED]
        if compressed:
            raise AssertionError("Random-access fonts are ZIP-compressed: "
                                 + ", ".join(compressed))
    print(f"PASS: {len(fonts)} font assets allow direct random access")


if __name__ == "__main__":
    main()
