#!/usr/bin/env python3
"""Rebuild Combo cues using the Earshot revision recorded in generation.json.

Requires that Earshot checkout, numpy and soundfile; no models or network needed.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from earshot.forge import bank, sfx


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Alternate output directory for verification')
    args = parser.parse_args()
    assets = Path(__file__).resolve().parents[1] / 'assets/audio/kombo'
    manifest = json.loads((assets / 'generation.json').read_text())
    output = args.output or assets
    output.mkdir(parents=True, exist_ok=True)
    sr = manifest['sample_rate']
    for name, spec in manifest['sounds'].items():
        layers = []
        for layer in spec['layers']:
            entry = bank.get(layer['id'])
            entry['params'].update(layer['params'])
            samples, _ = bank.render(entry, seed=layer['seed'], peak_db=layer['peak_db'])
            if samples.ndim == 1:
                samples = np.column_stack([samples, samples])
            layers.append((int(layer['at'] * sr), samples))
        mix = np.zeros((max(at + len(samples) for at, samples in layers), 2), np.float64)
        for at, samples in layers:
            mix[at:at + len(samples)] += samples
        mix = bank.normalize(mix, spec['peak_db']).astype(np.float32)
        audit = sfx.check(mix, category='swipe' if name == 'complete' else
                          'mallet' if name == 'round-advance' else None, sound=name)
        if not audit['ok']:
            raise RuntimeError(f'{name}: {audit["summary"]}')
        path = output / (name + '.wav')
        sf.write(path, mix, sr, subtype=manifest['format'])
        if path.stat().st_size > 256 * 1024:
            raise RuntimeError(f'{name}: exceeds the bounded cue loader')
        print(f'{name}: {len(mix) / sr:.3f}s, {audit["summary"]}')


if __name__ == '__main__':
    main()
