# Streak sounds

Generated locally from [Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8` (MIT; license included).
No models, recordings, network service, or runtime dependencies are used.

| File | Bank sound | Duration | Peak |
| --- | --- | --- | --- |
| progress.wav | ui_pop/soft | 0.181 s | −15 dBFS |
| complete.wav | mallet/up4 | 0.749 s | −9 dBFS |
| freeze.wav | mallet/celesta | 0.414 s | −9 dBFS |
| round-advance.wav | mallet/flourish | 0.504 s | −11 dBFS |

All four pass Earshot's model-free artifact audit, including clicks, clipping,
DC, and cut tails. `generation.json` records the parameters and measurements.
Render with Earshot's Python API, then write 48 kHz PCM16 WAV:

```python
from earshot.forge import bank
import soundfile as sf
samples, metadata = bank.render("ui_pop/soft", peak_db=-15)
sf.write("progress.wav", samples, 48000, subtype="PCM_16")
```

Playback uses the application's system audio bus and master volume. Cues fire
on drawer opening, round promotion, or successful freeze purchase, never from a
render callback. Simultaneous round promotions play one cue; dismissing the
drawer or resuming after the animation has finished does not play a late cue.
