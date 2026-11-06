import sys,json
from pathlib import Path
import soundfile as sf
sys.path.insert(0, str(Path(__file__).resolve().parent))
import kinetic
from earshot.forge import dsp,bank,sfx
out=Path(__file__).resolve().parent/'out'/'r3';out.mkdir(parents=True,exist_ok=True)
bases={
 'full-combo':dict(accent=.08,duration=1.25,floor=520,modal_tau=.13,sheen=.25,
    air=.21,air_tau=.08,body=.08,body_hz=210,glint=.12,root=880,
    cloud=.10,cloud_tau=.12,room=.09,rt60=.5,width=.4,pre=.07,tone_decay=.09),
 'perfect':dict(accent=.08,duration=1.65,floor=360,modal_tau=.19,sheen=.24,
    air=.26,air_tau=.11,body=.14,body_hz=145,glint=.12,root=660,
    cloud=.12,cloud_tau=.18,room=.13,rt60=.7,width=.55,pre=.085,tone_decay=.13),
 'max':dict(accent=.14,duration=2.0,floor=260,modal_tau=.23,sheen=.28,
    air=.30,air_tau=.13,body=.24,body_hz=95,glint=.14,root=520,
    cloud=.17,cloud_tau=.22,room=.15,rt60=.85,width=.65,pre=.13,tone_decay=.16),
}
manifest={}
for i,(outcome,base) in enumerate(bases.items()):
 for j,(variant,changes) in enumerate([('material',{}),('luminous',dict(glint=base['glint']*2.2,air=base['air']*.8,room=base['room']*.8))]):
  p={**kinetic.DEFAULTS,**base,**changes};seed=810+i*10+j
  x=kinetic.render(p,dsp.rng(seed));x=dsp.normalize(x,[-10,-8.5,-7][i])
  key=f'{outcome}--{variant}';sf.write(out/f'{key}.wav',x,48000,subtype='PCM_24')
  m=dict(outcome=outcome,params=p,seed=seed,file=f'{key}.wav',measure=bank.measure(x),audit=sfx.check(x,category='logo_sting'))
  manifest[key]=m;print(key,m['audit']['summary'],flush=True)
(out/'candidates.json').write_text(json.dumps(manifest,indent=2))
