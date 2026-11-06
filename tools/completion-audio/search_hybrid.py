#!/usr/bin/env python3
"""Earshot's bounded render/critic/mutate loop for the revised reward family."""
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import soundfile as sf
from earshot import api
from earshot.forge import forge, dsp, bank, sfx
from earshot.forge.critic import Critic
from earshot.forge.recipes import sting
import kinetic
from refine import TARGETS, ALTS

ROOT=Path(__file__).resolve().parent
OUT=ROOT/'out'
CONFUSIONS=ALTS+['A dry snare drum hit.', 'An unpleasant electronic error buzz.', 'A doorbell ringing.']


def search(outcome):
    r2=json.loads((OUT/'r2'/'candidates.json').read_text())
    r3=json.loads((OUT/'r3'/'candidates.json').read_text())
    core_name={'full-combo':'silk-alloy','perfect':'gold-alloy','max':'alloy-star'}[outcome]
    core=r2[f'{outcome}--{core_name}']['params']
    texture=r3[f'{outcome}--luminous']['params']
    base=dict(blend=.74, shimmer=core['shimmer'], ring_tau=core['ring_tau'],
              width=core['width'], ring=core['ring'], body=texture['body'])
    space=dict(blend=(.58,.88),shimmer=(.07,.30),ring_tau=(core['ring_tau']*.85,core['ring_tau']*1.3),
               width=(.35,.76),ring=(.72,1.22),body=(texture['body']*.7,texture['body']*1.3))
    def render(p,r):
        metal=sting.render({**core,**{k:p[k] for k in ['shimmer','ring_tau','width','ring']}},r)
        grain=kinetic.render({**texture, 'body':p['body'], 'glint':.035},r)
        n=max(len(metal),len(grain))
        y=np.zeros((n,2));y[:len(metal)]+=p['blend']*metal
        y[:len(grain)]+=(1-p['blend'])*grain
        # One envelope, one main accent. The quiet late halo is smoothly gated.
        return dsp.normalize(dsp.fade(y,.0015,.20),-1).astype(np.float32)
    recipe=SimpleNamespace(__name__='completion_hybrid_'+outcome.replace('-','_'),
        DEFAULTS=base,SPACE=space,render=render,TARGET=TARGETS[outcome],ALTS=CONFUSIONS)
    critic=Critic(TARGETS[outcome],alts=CONFUSIONS,mode='designed',category='logo_sting')
    folder=OUT/'r4'/outcome
    print('SEARCH',outcome,flush=True)
    report=forge(recipe,critic=critic,budget=16,pop=4,keep=2,seed=42,
        out=str(folder),init=[base],robust=2,shortlist=2,outputs=False,
        holdout=False,prescreen=0,workers=1,log=lambda x:print(x,flush=True))
    # Independent AST descriptions are saved after the CLAP-guided search.
    descriptions={}
    for path in sorted(folder.glob('*.wav')):
        if 'audition' in path.name: continue
        desc=api.describe(str(path))
        descriptions[path.name]=desc
        print('HELD-OUT',path.name,desc['summary'],flush=True)
    (folder/'descriptions.json').write_text(json.dumps(descriptions,indent=2))
    return report

if __name__=='__main__':
    import sys
    for outcome in (sys.argv[1:] or ['full-combo','perfect','max']):search(outcome)
