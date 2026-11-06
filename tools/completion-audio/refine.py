#!/usr/bin/env python3
"""Local candidate generation and Earshot listening feedback; no game integration."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
from earshot.forge import dsp, sfx, bank
from earshot.forge.recipes import sting

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'out'
TARGETS = {
    'full-combo': 'A crisp metallic snap followed by a short airy whoosh and a bright glassy resonance, a polished game achievement sound effect.',
    'perfect': 'A single rich glass and metal impact with a warm resonant body and a luminous shimmering decay, a polished game achievement sound effect.',
    'max': 'A short cinematic crystalline impact with a deep punchy body, bright metallic sparkle and a wide airy decay, a triumphant game achievement sound effect.',
}
ALTS = ['A simple electronic phone notification ding.',
        'A little melody played on high pitched bells.',
        'A cartoon video game bleep.',
        'A laser gun firing.',
        'A loud explosion with rumbling bass.']

# Single-excitation material engine, with fixed picture timing. Granular upper
# resonance comes from the struck material itself, not unrelated bell layers.
BASE = dict(mode='launch', lead='whoosh', lead_s=.08, lead_level=.16,
            lead_f0=800, lead_f1=5200, tick=.55, tick_f=4200, tick_ms=5.5,
            thump=.08, thump_f=175, material='glass', root=740,
            notes='one', bright=.36, ring=.9, strike_tau=.07,
            ring_mix=.16, ring_tau=.21, bloom_att=.006, bloom_delay=.007,
            shimmer=.14, shimmer_tau=.20, shimmer_smooth=.85,
            verb=.11, verb_rt=.72, width=.45, low_cut=155,
            low=0, low_f=75, low_tau=.09)

CANDIDATES = {
 'full-combo': {
   'glass-snap': dict(root=920, material='glass', bright=.28, tick=.65,
                     strike_tau=.048, ring_mix=.12, ring_tau=.16, shimmer=.08,
                     verb=.08, width=.35),
   'silk-alloy': dict(root=660, material='triangle', bright=.2, tick=.55,
                     strike_tau=.055, ring_mix=.12, ring_tau=.18, shimmer=.10,
                     verb=.10, width=.44),
   'ceramic-release': dict(root=560, material='fm', bright=.18, tick=.48,
                          strike_tau=.06, ring_mix=.14, ring_tau=.18, shimmer=.08,
                          lead_level=.22, verb=.09, width=.40),
 },
 'perfect': {
   'glass-bloom': dict(root=740, material='glass', notes='fifth', ring=1.0,
                      bright=.32, tick=.5, strike_tau=.09, ring_mix=.23,
                      ring_tau=.25, bloom_att=.009, shimmer=.16,
                      shimmer_tau=.24, width=.64, verb=.14, verb_rt=.82,
                      thump=.13, thump_f=160, low_cut=110),
   'gold-alloy': dict(root=520, material='triangle', notes='one', ring=1.05,
                     bright=.23, tick=.45, strike_tau=.085, ring_mix=.20,
                     ring_tau=.25, bloom_att=.012, shimmer=.15,
                     shimmer_tau=.25, width=.6, verb=.16, verb_rt=.85,
                     thump=.16, thump_f=145, low_cut=100),
   'silk-resonance': dict(root=620, material='fm', notes='fifth', ring=1.0,
                         bright=.24, tick=.42, strike_tau=.09, ring_mix=.22,
                         ring_tau=.26, bloom_att=.010, shimmer=.18,
                         shimmer_tau=.25, width=.65, verb=.13, verb_rt=.82,
                         thump=.14, thump_f=155, low_cut=110),
 },
 'max': {
   'crystal-pressure': dict(root=520, material='glass', notes='fifth', ring=1.1,
                           bright=.38, tick=.65, strike_tau=.11, ring_mix=.24,
                           ring_tau=.31, bloom_att=.010, shimmer=.22,
                           shimmer_tau=.30, width=.72, verb=.16, verb_rt=.9,
                           thump=.22, thump_f=145, low_cut=85, low=.25,
                           low_f=66, low_tau=.10, lead_s=.14, lead_level=.24),
   'alloy-star': dict(root=420, material='triangle', notes='one', ring=1.15,
                     bright=.29, tick=.6, strike_tau=.10, ring_mix=.23,
                     ring_tau=.30, bloom_att=.010, shimmer=.23,
                     shimmer_tau=.31, width=.70, verb=.17, verb_rt=.92,
                     thump=.24, thump_f=145, low_cut=85, low=.27,
                     low_f=66, low_tau=.10, lead_s=.14, lead_level=.24),
   'dense-prism': dict(root=460, material='fm', notes='fifth', ring=1.15,
                      bright=.27, tick=.60, strike_tau=.10, ring_mix=.24,
                      ring_tau=.31, bloom_att=.010, shimmer=.24,
                      shimmer_tau=.30, width=.72, verb=.16, verb_rt=.9,
                      thump=.24, thump_f=140, low_cut=85, low=.24,
                      low_f=66, low_tau=.10, lead_s=.14, lead_level=.24),
 },
}


def render(params, seed, peak=-8):
    wave = sting.render(params, dsp.rng(seed))
    # The output remains one gesture. Smoothly darken and end its quiet tail.
    wave = dsp.fade(wave, .001, .20)
    return dsp.normalize(wave, peak).astype(np.float32)


def generate(folder):
    folder.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for oi,(outcome, designs) in enumerate(CANDIDATES.items()):
        for di,(name, edits) in enumerate(designs.items()):
            params = {**BASE, **edits}
            seed = 310 + oi*10 + di
            wave = render(params, seed, [-10, -8.5, -7][oi])
            key = f'{outcome}--{name}'
            path = folder / f'{key}.wav'
            sf.write(path, wave, 48000, subtype='PCM_24')
            manifest[key] = dict(outcome=outcome, params=params, seed=seed, file=path.name,
                                 measure=bank.measure(wave), audit=sfx.check(wave,category='logo_sting'))
            print('RENDER',key,manifest[key]['audit']['summary'],flush=True)
    (folder/'candidates.json').write_text(json.dumps(manifest,indent=2))


def evaluate(folder):
    from earshot.forge.critic import Critic
    from earshot import api
    manifest=json.loads((folder/'candidates.json').read_text())
    feedback={}
    for outcome,target in TARGETS.items():
        keys=[k for k,v in manifest.items() if v['outcome']==outcome]
        paths=[OUT/'v1'/f'{outcome}.wav']+[folder/manifest[k]['file'] for k in keys]
        print('LISTEN',outcome,flush=True)
        critic=Critic(target,alts=ALTS,mode='designed',category='logo_sting')
        waves=[sf.read(p)[0] for p in paths]
        # Small batches avoid a large temporary feature tensor on the laptop.
        embeddings=np.concatenate([critic.embed(waves[i:i+2],batch=2) for i in range(0,len(waves),2)])
        scores=critic.score(waves,E=embeddings)
        group={}
        for name,path,score in zip(['rejected-v1']+keys,paths,scores):
            group[name]={'critic':score,'path':str(path)}
            print(name,score['summary'],flush=True)
        # Independent held-out tagger describes every new candidate, not only a
        # high-scoring winner. Keep the full language rather than just its score.
        for key in keys:
            desc=api.describe(str(folder/manifest[key]['file']))
            group[key]['describe']=desc
            print('DESCRIPTION',key,desc['summary'],flush=True)
        feedback[outcome]={'target':target,'alternatives':ALTS,'candidates':group}
        (folder/'feedback.json').write_text(json.dumps(feedback,indent=2))


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('action',choices=['render','listen','both'])
    parser.add_argument('--round',default='r2')
    args=parser.parse_args()
    folder=OUT/args.round
    if args.action in ('render','both'): generate(folder)
    if args.action in ('listen','both'): evaluate(folder)

if __name__=='__main__': main()
