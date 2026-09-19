"""Compile production sleep-timer methods against controlled synchronization."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/audio/MusicPlayerService.cpp").read_text()
    fixture = (args.root / "tests/music_player_sleep_timer_fixture.cpp").read_text()
    methods = "\n\n".join(extract(source, signature) for signature in (
        "bool MusicPlayerService::SetSleepTimer(",
        "void MusicPlayerService::ClearSleepTimer()",
        "long long MusicPlayerService::SleepTimerRemainingMicros() const",
        "void MusicPlayerService::EnsureSleepTimerWorker()",
        "void MusicPlayerService::StopSleepTimerWorker()",
        "void MusicPlayerService::SleepTimerWorker(",
    ))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture.replace("TIMER_METHODS", methods))


if __name__ == "__main__":
    main()
