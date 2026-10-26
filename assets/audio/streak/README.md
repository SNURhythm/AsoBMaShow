# Streak sounds

Generated locally from [Earshot](https://github.com/Leonxlnx/earshot), commit
`591fa3e028032e6dce56620a20e932e35667e8d8` (MIT; license included).
No models, recordings, network service, or runtime dependencies are used.

| File | Bank sound | Duration | Peak |
| --- | --- | --- | --- |
| progress.wav | ui_pop/step | 0.187 s | −9 dBFS |
| complete.wav | mallet/up4 | 0.749 s | −9 dBFS |
| freeze.wav | mallet/celesta | 0.414 s | −9 dBFS |

All three pass Earshot's model-free artifact audit, including clicks, clipping,
DC, and cut tails. `generation.json` records the parameters and measurements.
Render with Earshot's Python API, then write 48 kHz PCM16 WAV:

```python
from earshot.forge import bank
import soundfile as sf
samples, metadata = bank.render("ui_pop/step", peak_db=-9)
sf.write("progress.wav", samples, 48000, subtype="PCM_16")
```

Playback uses the application's system audio bus and master volume. Cues fire
on drawer opening or successful freeze purchase, never from a render callback.
