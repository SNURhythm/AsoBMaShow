import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract
p = argparse.ArgumentParser()
p.add_argument('--root', type=Path)
p.add_argument('--output', type=Path)
a = p.parse_args()
source = (a.root / 'src/audio/MusicPlayerService.cpp').read_text()
methods = '\n'.join(extract(source, signature) for signature in (
    'bool MusicPlayerService::SetStorageMaintenance(',
    'bool MusicPlayerService::PlayCurrent(',
    'bool MusicPlayerService::PlayNext(',
    'bool MusicPlayerService::PlayPrevious(',
    'bool MusicPlayerService::Resume(',
    'bool MusicPlayerService::StartPlaybackAsync(',
))
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text((a.root / 'tests/storage_music_admission_fixture.cpp').read_text().replace('METHODS', methods))
