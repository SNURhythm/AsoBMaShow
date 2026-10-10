"""Extract the Android renderer pause callback for the device-free fixture."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/main.cpp").read_text()
    callback = extract(source, "auto applyAndroidRenderSuspend = [&](bool suspend)")
    lifecycle = extract(source, "auto setAppBackground = [&](bool background)")
    sync = extract(source, "auto syncAndroidRenderSuspend = [&]()")
    recovery = extract(source, "if (pressureRecovery) {\n      context.inputDeviceRegistry.reconcileSdlDevices();")
    fixture = (args.root / "tests/android_render_suspend_fixture.cpp").read_text()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture.replace("PRODUCTION_SUSPEND", callback).replace("PRODUCTION_LIFECYCLE", lifecycle).replace("PRODUCTION_SYNC", sync).replace("PRODUCTION_RECOVERY", recovery))


if __name__ == "__main__":
    main()
