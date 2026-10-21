#!/usr/bin/env python3
"""Create a sparse ZIP64 fixture for the opt-in chart audio benchmark.

Requires a filesystem supporting sparse files. The archive contains a 128 GiB
zero-filled, unselected member, many unrelated empty entries, and a one-second
stereo WAV referenced twice by a small chart. It never writes the zero payload.
Run benchmark_chart_audio_preparation.py with OUTPUT/song/chart.bms as the chart.
"""

import argparse
import io
import json
from pathlib import Path
import struct
import wave
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--entries', type=int, default=100000,
                    help='Number of unrelated empty entries (default: 100000)')
options = parser.parse_args()
if options.output.exists() or not 0 <= options.entries <= 1000000:
    parser.error('output must not exist and entries must be between 0 and 1000000')

size = 128 * 1024**3
# CRC32 of 2**37 zero bytes, computed by repeated crc32_combine doubling.
# Keeping the constant avoids reading or allocating the sparse payload.
crc = 0x190A55AD
name = b'unrelated/huge.bin'
extra = struct.pack('<HHQQ', 1, 16, size, size)
with options.output.open('xb') as file:
    file.write(struct.pack('<IHHHHHIIIHH', 0x04034B50, 45, 0, 0, 0, 0,
                           crc, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra)))
    file.write(name + extra)
    file.seek(size, 1)
    central_offset = file.tell()
    file.write(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014B50, 45, 45, 0, 0, 0, 0,
                           crc, 0xFFFFFFFF, 0xFFFFFFFF, len(name), len(extra), 0,
                           0, 0, 0, 0))
    file.write(name + extra)
    central_size = file.tell() - central_offset
    end_offset = file.tell()
    file.write(struct.pack('<IQHHIIQQQQ', 0x06064B50, 44, 45, 45, 0, 0,
                           1, 1, central_size, central_offset))
    file.write(struct.pack('<IIQI', 0x07064B50, 0, end_offset, 1))
    file.write(struct.pack('<IHHHHIIH', 0x06054B50, 0, 0, 1, 1,
                           central_size, 0xFFFFFFFF, 0))

wav = io.BytesIO()
with wave.open(wav, 'wb') as file:
    file.setnchannels(2)
    file.setsampwidth(2)
    file.setframerate(44100)
    file.writeframes(struct.pack('<hh', 1000, 1000) * 44100)
with zipfile.ZipFile(options.output, 'a', compression=zipfile.ZIP_STORED,
                     allowZip64=True) as archive:
    archive.writestr('song/chart.bms', '#PLAYER 1\n#TITLE sparse archive scale\n'
                     '#BPM 120\n#WAV01 tone.wav\n#WAV02 TONE.ogg\n#00111:0102\n')
    archive.writestr('song/tone.wav', wav.getvalue())
    for index in range(options.entries):
        archive.writestr(f'unrelated/{index:06d}.txt', b'')

status = options.output.stat()
with zipfile.ZipFile(options.output) as archive:
    # Read only selected payloads; never testzip(), which would scan 128 GiB.
    assert archive.read('song/tone.wav') == wav.getvalue()
    assert archive.getinfo('unrelated/huge.bin').file_size == size
    print(json.dumps({
        'logical_bytes': status.st_size,
        'allocated_bytes': status.st_blocks * 512 if hasattr(status, 'st_blocks') else None,
        'entries': len(archive.infolist()),
        'selected_wav_bytes': len(wav.getvalue()),
        'central_directory_and_footer_bytes': status.st_size - archive.start_dir,
        'chart': str(options.output.resolve()) + '/song/chart.bms',
    }, indent=2))
