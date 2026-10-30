# Drawable storage across display-link recreation

This standalone research harness includes the production `MetalDisplayLink.mm`
and drawable provider. Real Simulator callbacks supply the drawables. It renders
160 clear passes in four phases, requesting 60 Hz and latency one with a
three-drawable layer:

1. Forty frames at 48×32.
2. Request the production manual link recreation; forty more frames at 48×32.
3. Change drawable size on main to 32×48; forty frames.
4. Restore 48×32; forty frames.

The small texture is intentional: this tests storage identity, not fullscreen
scanout. There is no SDL or bgfx renderer in this harness. The public
`MTLTexture.iosurface` and `IOSurfaceGetID` APIs identify backing storage. Each
distinct IOSurface is retained until the experiment finishes, preventing a
freed ID from being recycled and mistaken for reuse. Drawables/textures are not
retained across frames by the collector; no surface is locked or read back.
Holding surfaces does extend their memory lifetime, so this is not an allocation
performance benchmark or an observation of uninstrumented memory reclamation.

## Observed result

On the iOS 27.1 iPhone Duo Simulator, using the final checked-in harness:

- All 160 frames completed; zero begin-frame retries or rejected drawables.
- First phase: link serial 1, IOSurfaces 168, 173, 181.
- Recreated link: serial 2, **the same IOSurfaces** 168, 173, 181.
- Swapped size: serial 2, new IOSurfaces 35, 52, 251.
- Restored size: serial 2, new IOSurfaces 256, 263, 265.

See `results.json` for every sampled frame. IDs are local to this run, not
stable identifiers to assert in future runs. Assert intersection between the
first two phase sets, disjoint sets at resize, correct dimensions and increased
link serial instead. The script below checks those properties.

An earlier derivative of the transaction harness produced the same pool result,
with one automatic link replacement and 57 failed `beginFrame` calls including
retry-cooldown polls. Those polls were not 57 demonstrated dropped frames. The
retained final run above did not take that fallback path.

The experiment proves that this link-recreation operation can preserve drawable
storage. It does **not** demonstrate the failing iPad's pool contents, a stale
orientation flag, background purging, Direct/Composited mode, physical refresh
rate, or latency. Resizing replaced storage here, which argues against a simple
claim that rotation necessarily leaves the old-size buffers in place.

## Build and run

The existing research builder is reused. In `--pool-only` mode no SDL is linked
or copied. No production app is built or distributed.

```sh
python3 docs/reviews/evidence/metal-clock/build_simulator.py \
  --pool-only \
  --output /tmp/asobmashow-pool-evidence
xcrun simctl install booted /tmp/asobmashow-pool-evidence/Audit.app
xcrun simctl launch booted com.snurhythm.live-pool-audit
```

After the bounded run finishes, read `Documents/pool.json` inside the data
container returned by:

```sh
xcrun simctl get_app_container booted com.snurhythm.live-pool-audit data
```

For the retained result (or substitute the new result's path):

```sh
python3 - <<'PY'
import json
from pathlib import Path
result = json.loads(Path('docs/reviews/evidence/metal-pool/results.json').read_text())
rows = result['rows']
assert len(rows) == 160
groups = [rows[n * 40:(n + 1) * 40] for n in range(4)]
surfaces = [{r['surface'] for r in group} for group in groups]
assert all(0 not in ids for ids in surfaces)
assert surfaces[0] & surfaces[1]
assert not surfaces[1] & surfaces[2]
assert not surfaces[2] & surfaces[3]
assert groups[1][0]['serial'] > groups[0][-1]['serial']
for group, size in zip(groups, [(48, 32), (48, 32), (32, 48), (48, 32)]):
    assert {(r['width'], r['height']) for r in group} == {size}
print('Storage identity and recreation assertions passed')
PY
xcrun simctl uninstall booted com.snurhythm.live-pool-audit
```
