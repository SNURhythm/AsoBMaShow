#!/usr/bin/env python3
"""Master the chosen local prototypes and retain explicit before/after feedback."""
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
from scipy import signal
import soundfile as sf
from earshot import api
from earshot.forge import bank, sfx, dsp
from earshot.forge.critic import Critic
from refine import TARGETS
from search_hybrid import CONFUSIONS

ROOT=Path(__file__).resolve().parent
OUT=ROOT/'out'


def encode(x,path,preroll=False):
    if preroll:
        x=np.pad(x,((31_200,max(48_000,round(4.0*48_000)-31_200-len(x))),(0,0)))
    sf.write(path.with_suffix('.wav'),x,48000,subtype='PCM_24')
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(path.with_suffix('.wav')),
                    '-c:a','aac','-b:a','160k',str(path.with_suffix('.m4a'))],check=True)


def main():
    report={'revision':'v2-local','earshot_commit':'591fa3e028032e6dce56620a20e932e35667e8d8',
            'selection':'r4 candidate 01 per outcome; selected after alternate-seed robustness and independent AST description',
            'prototype_only':True,'sounds':{}}
    import sys
    only=sys.argv[1:]
    if only and (OUT/'revision-feedback.json').exists():
        report=json.loads((OUT/'revision-feedback.json').read_text())
    for outcome,hz in [('full-combo',3540),('perfect',3490),('max',2263)]:
        if only and outcome not in only: continue
        folder=OUT/'r4'/outcome
        search=json.loads((folder/'report.json').read_text())
        pick=search['picks'][0]
        path=folder/(pick['name']+'.wav')
        x,sr=sf.read(path)
        # Feedback-specific damping: leave contact intact; reduce the identified
        # ringing band only as it enters its tail. Never use a hard audio gate.
        b,a=signal.iirnotch(hz,9,fs=sr)
        damped=signal.lfilter(b,a,x,axis=0)
        accent=.14 if outcome=='max' else .08
        u=np.clip((np.arange(len(x))/sr-accent-.022)/.075,0,1)
        blend=.76*(u*u*(3-2*u))
        x=x*(1-blend[:,None])+damped*blend[:,None]
        if outcome=='max':
            # MAX's larger star needs a low-mid bloom as well as its bright
            # contact. Excite it at the same accent; no extra hit or melody.
            length=(len(x)-round(accent*sr))/sr
            bloom=dsp.modal([123.47,184.997,246.94,329.63,369.99],
                            [.18,.22,.18,.14,.11],[1,.6,.38,.24,.14],
                            length,dsp.rng(940),jitter=.003)
            t=np.arange(len(bloom))/sr
            bloom*=1-np.exp(-t/.009)
            bloom=dsp.normalize(bloom,-1)*.28
            x[round(accent*sr):round(accent*sr)+len(bloom)]+=bloom[:,None]
            x=dsp.fade(x,.002,.20)
        before=bank.measure(x)
        target={'full-combo':-18.0,'perfect':-17.0,'max':-14.5}[outcome]
        gain=min(target-before['lufs_m_max'],-4-before['peak_db'])
        x*=10**(gain/20)
        encode(x,OUT/outcome)
        encode(x,OUT/(outcome+'-preview'),preroll=True)
        old,_=sf.read(OUT/'v1'/(outcome+'.wav'))
        new_measure=bank.measure(x)
        old_measure=bank.measure(old)
        old*=10**((new_measure['lufs_m_max']-old_measure['lufs_m_max'])/20)
        encode(old,OUT/('v1-'+outcome+'-preview'),preroll=True)
        print('FINAL',outcome,new_measure,flush=True)
        critic=Critic(TARGETS[outcome],alts=CONFUSIONS,mode='designed',category='logo_sting')
        feedback=critic.score([x])[0]
        desc=api.describe(str(OUT/(outcome+'.wav')))
        report['sounds'][outcome]={'selected':pick,'damped_hz':hz,'gain_db':gain,
            'measure':new_measure,'audit':sfx.check(x,category='logo_sting'),
            'critic':feedback,'describe':desc,
            'sha256':hashlib.sha256((OUT/(outcome+'.wav')).read_bytes()).hexdigest()}
        print(feedback['summary'],flush=True)
        print(desc['summary'],flush=True)
        (OUT/'revision-feedback.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__':main()
