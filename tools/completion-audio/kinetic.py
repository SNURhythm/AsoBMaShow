"""A contact-excited material, air and diffuse tail, built with Earshot DSP.

Single gesture, with an optional restrained pitched glint. Not a bell sequence.
The fixed accent is expressed in seconds from the final gameplay judgement.
"""
import numpy as np
from scipy import signal
from earshot.forge import dsp, fx
from earshot.forge.recipes.impact import hall_ir

SR = dsp.SR
DEFAULTS = dict(accent=.08, duration=1.4, floor=420., modal_tau=.16,
                sheen=.28, air=.23, air_tau=.095, body=.10, body_hz=180.,
                glint=.11, root=700., cloud=.12, cloud_tau=.15, room=.10,
                rt60=.55, width=.42, pre=.07, tone_decay=.10, tilt=-1.5)
SPACE = dict(modal_tau=(.10,.25), sheen=(.16,.36), air=(.15,.40),
             air_tau=(.07,.16), glint=(.025,.18), cloud=(.06,.18),
             room=(.065,.16), width=(.3,.7))
TARGET = 'A crisp metallic snap followed by a short airy whoosh and a bright glassy resonance, a polished game achievement sound effect.'
ALTS = ['A simple electronic phone notification ding.', 'A little melody played on high pitched bells.',
        'A cartoon video game bleep.', 'A laser gun firing.', 'A loud explosion with rumbling bass.']


def unit(x):
    return np.asarray(x) / (np.max(np.abs(x)) + 1e-12)


def render(p, r):
    p = {**DEFAULTS, **p}
    duration = p['duration']
    n = round((duration-p['accent'])*SR)
    t = np.arange(n)/SR
    # Dense modes change the material itself; this is not just an EQ on a ding.
    modes=dsp.modal_cloud(p['floor'], 9800, 92, p['modal_tau'], .027,
                          n/SR, r, tilt=p['tilt'])
    contact=dsp.contact(.0007, r, bright=.65)
    material=unit(dsp.excite(modes, contact))
    material=fx.saturate(material,drive_db=3,mix=.14,os=2,level='peak')
    material=dsp.butter(material,'high',180,2)
    material*=1-np.exp(-t/.0018)
    # Dry head and a small, broad friction tail share one physical envelope.
    noise=dsp.air(n/SR,r,center=3100,flutter=.15,rate=11,bright=.3)
    air=noise*(1-np.exp(-t/.006))*np.exp(-t/p['air_tau'])
    air=unit(air)
    tick=dsp.butter(r.standard_normal(n),'band',(2800,10800),2)
    tick=unit(tick*(1-np.exp(-t/.0005))*np.exp(-t/.007))
    # Slightly irregular dyad in the body, deliberately quieter than the material.
    glint=dsp.modal([p['root'],p['root']*1.49,p['root']*2.016,p['root']*3.13],
                    [p['tone_decay'],p['tone_decay']*.7,.045,.026],
                    [1,.32,.16,.07],n/SR,r,jitter=.007,glide=.004)
    glint=unit(dsp.excite(glint,contact))
    glint*=1-np.exp(-t/.004)
    body=dsp.sub_drop(n/SR,r,f0=p['body_hz']*1.35,f1=p['body_hz'],
                      drop=.012,decay=.052,harm=.3,click=0,attack=.004)
    dry=.66*material+p['sheen']*tick+p['air']*air+p['glint']*glint+p['body']*body
    dry=dsp.butter(dry,'low',13500,3)
    out=np.column_stack((dry,dry))
    # Particles are short overlapping fragments of this same material. This
    # introduces a moving, nonperiodic texture instead of an endless bell ring.
    src=dsp.butter(material[:round(.25*SR)],'band',(2100,8200),3)
    grains=fx.granular(src,n/SR,r,grain=.038,density=115,pos=(.02,.55),
                      pitch=.3,pitch_jitter=1.4,time_jitter=.9,spread=.75)
    grain_env=(1-np.exp(-t/.016))*np.exp(-t/p['cloud_tau'])
    grains=unit(grains)*grain_env[:,None]
    out+=p['cloud']*grains
    # Upper energy spreads after contact. Bass and the first head stay centered.
    hi=dsp.butter(dry,'high',900,3)
    spread=fx.decorrelate(hi,p['width'],cut=900)-hi[:,None]
    out+=spread*(1-np.exp(-t/.025))[:,None]
    send=dsp.butter(dry,'high',700,3)
    for c in range(2):
        ir=hall_ir(r,p['rt60'],.75,size=.35,predelay=.013+.003*c,seconds=.85)
        out[:,c]+=p['room']*signal.fftconvolve(send,ir)[:n]
    # The gather is unpitched air, with a smooth join under the strike.
    at=round(p['accent']*SR)
    final=np.zeros((round(duration*SR),2))
    final[at:at+n]+=out[:len(final)-at]
    leadn=at+round(.014*SR)
    lead=dsp.butter(r.standard_normal(leadn),'band',(650,5500),2)
    u=np.arange(leadn)/SR
    env=np.minimum(u/max(p['accent'],.001),1)**2.3
    env[at:]*=np.linspace(1,0,leadn-at)**2
    lead=unit(lead*env)*p['pre']
    final[:leadn]+=lead[:,None]
    final=dsp.butter(final,'high',38,2)
    final=dsp.fade(final,.002,.22)
    return dsp.normalize(final,-1).astype(np.float32)
