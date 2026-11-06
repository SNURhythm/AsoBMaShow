#!/usr/bin/env python3
"""Soft melodic reward prototypes using Earshot; local authoring only."""
import json
from pathlib import Path
import subprocess
import numpy as np
from scipy import signal
import soundfile as sf
from earshot.forge import dsp, bank
from earshot.forge.recipes import mallet
from earshot.forge.recipes.impact import hall_ir
from earshot import api

OUT=Path(__file__).resolve().parent/'out'
NAMES={'C':0,'D':2,'E':4,'F':5,'G':7,'A':9,'B':11}
def hz(note):
    return 440*2**(((int(note[-1])+1)*12+NAMES[note[0]]-69)/12)

def voice(note, hold, velocity, seed, pan=0):
    p={**mallet.DEFAULTS,'bar':'celesta','f0':hz(note),'hard':.16,'strike':-49,
       'onset':.012,'gate':hold,'gate_tau':.055,'decay':.45,'slope':1.1,
       'hp':110,'lp':4800,'width':.25,'pan':pan,'room':.025,'rt':.32,
       'oct':-19,'oct_ratio':.5,'beat':.45,'jit':.12}
    x=mallet.render(p,dsp.rng(seed))
    return x*velocity

MELODIES={
 'full-combo': [('G4',.08,.72),('C5',.22,.86),('E5',.36,.98),('D5',.56,.76),('C5',.76,.90)],
 'perfect': [('G4',.08,.68),('C5',.20,.80),('E5',.34,.87),('G5',.54,1.0),('E5',.74,.83),('D5',.88,.74),('C5',1.08,.96)],
 'max': [('G4',.14,.69),('C5',.26,.79),('E5',.38,.88),('G5',.58,.96),('A5',.78,.94),('G5',.92,.82),('E5',1.08,.76),('C6',1.28,1.0)]}

def encode(x,name):
    sf.write(OUT/(name+'.wav'),x,48000,subtype='PCM_24')
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',str(OUT/(name+'.wav')),
                    '-c:a','aac','-b:a','160k',str(OUT/(name+'.m4a'))],check=True)

def main():
    report={'revision':'melodic-local','sounds':{}}
    for index,(name,notes) in enumerate(MELODIES.items()):
        length=[1.65,2.0,2.4][index]
        layers=[]
        for j,(note,at,velocity) in enumerate(notes):
            last=j==len(notes)-1
            layers.append((voice(note,.34 if last else .21,velocity,1200+index*20+j, (j/(len(notes)-1)-.5)*.28),at,0))
        if index:
            # Soft harmony under the phrase, played with the same rounded voice.
            for j,note in enumerate(['C4','E4','G4']):
                layers.append((voice(note,.62,.35,1300+index*10+j),notes[0][1]+j*.008,-12))
            for j,note in enumerate(['C4','G4','E5']):
                layers.append((voice(note,.42,.40,1400+index*10+j),notes[-1][1]+j*.006,-12))
        x=dsp.stack(layers,length=length)
        send=dsp.butter(x,'high',400,2)
        for channel in range(2):
            ir=hall_ir(dsp.rng(1500+index*2+channel),.55,.8,size=.3,predelay=.015,seconds=.65)
            x[:,channel]+=.09*signal.fftconvolve(send[:,channel],ir)[:len(x)]
        x=dsp.fade(x,.002,.18)
        measure=bank.measure(x)
        gain=min([-18,-17,-16][index]-measure['lufs_m_max'],-5-measure['peak_db'])
        x*=10**(gain/20)
        encode(x,name)
        lead=np.pad(x,((31200,192000-31200-len(x)),(0,0)))
        encode(lead,name+'-preview')
        # Compare the immediately previous, tougher version at the same level.
        old,_=sf.read(OUT/'v2'/(name+'.wav'))
        old*=10**((bank.measure(x)['lufs_m_max']-bank.measure(old)['lufs_m_max'])/20)
        old=np.pad(old,((31200,192000-31200-len(old)),(0,0)))
        encode(old,'v1-'+name+'-preview')
        target='A short gentle uplifting melody played on a warm soft electric piano, a musical game victory jingle.'
        alts=['A harsh metallic impact.', 'A snare drum hit.', 'A single electronic notification beep.']
        match=api.match(str(OUT/(name+'.wav')),target,alts=alts)
        desc=api.describe(str(OUT/(name+'.wav')))
        report['sounds'][name]={'notes':notes,'measurement':bank.measure(x),'match':match,'description':desc}
        (OUT/'melody-feedback.json').write_text(json.dumps(report,indent=2))
        print(name,desc['summary'],flush=True)
if __name__=='__main__':main()
