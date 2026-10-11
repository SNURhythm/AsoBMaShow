#!/usr/bin/env python3
"""Compare complete bitmap-font results for local descriptors and generated cases."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--skin-root', type=Path, required=True)
options = parser.parse_args()
work = options.output.resolve()
results = []
for variant in ('before', 'after'):
    result = subprocess.run(
        [str(work / f'font-{variant}'), str(options.skin_root / 'LITONE12'),
         str(options.skin_root / 'ModernChic')], capture_output=True, text=True, check=True)
    (work / f'font-{variant}.json').write_text(result.stdout)
    results.append(json.loads(result.stdout))
before, after = results
if before['records'] != after['records']:
    raise RuntimeError('font parser results differ')
print(f"Matched {before['files']} real descriptors and {before['generated']} generated cases")
