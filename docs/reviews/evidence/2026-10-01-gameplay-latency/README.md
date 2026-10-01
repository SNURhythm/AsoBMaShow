# Latency audit evidence

These are one-off investigation probes, not application code or automated
latency regression tests. Run from the repository root on this macOS checkout.
They do not modify the CMake build directory or application settings. The audio
probe opens the default output device and outputs silence; no input capture is
performed.

Baseline: `5750d646bda5d1c1d9814793712caed530809371`; Apple M1 Pro.
SDL commit: `066b750c46a24d1a85e91dbffecfe4f1c5cfa04f`.
bgfx wrapper commit: `6e2138a92033af5f1e24d262448e3a81b987df10`.

The output directory defaults to `/tmp/asobmashow-latency-audit`.

```sh
mkdir -p /tmp/asobmashow-latency-audit
latency_evidence=docs/reviews/evidence/2026-10-01-gameplay-latency

c++ -std=c++20 -O2 "$latency_evidence/audio_probe.cpp" \
  -I cmake-build-debug/vcpkg_installed/arm64-osx/include \
  cmake-build-debug/vcpkg_installed/arm64-osx/debug/lib/libportaudio.a \
  -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
  -framework CoreFoundation -framework CoreServices \
  -o /tmp/asobmashow-latency-audit/audio_probe

c++ -std=c++23 -O2 -Isrc "$latency_evidence/mix_probe.cpp" \
  src/audio/AudioMix.cpp src/settings/AudioVideoSettings.cpp \
  -o /tmp/asobmashow-latency-audit/mix_probe

python3 "$latency_evidence/build_worker_probe.py"

/tmp/asobmashow-latency-audit/audio_probe
/tmp/asobmashow-latency-audit/mix_probe
/tmp/asobmashow-latency-audit/worker_probe
```

`build_worker_probe.py` reads Ninja's existing worker-test link command, compiles
its production C++ sources with `-O2 -DNDEBUG` into the temporary directory, and
reuses the existing SDL/localization libraries. It does not run Ninja builds.
It is specific to the current macOS build layout and is not a portable build
tool. `LATENCY_AUDIT_OUT` can override its output directory.

`audio_probe.txt` retains one approximately one-second stream for each of five
buffer settings. Callback samples are written into preallocated storage; summary
printing/sorting occurs after stream shutdown. Its DAC lead is PortAudio's
`outputBufferDacTime - currentTime`, not acoustic output latency. No audible
signal or loopback measurement is involved.

`mix_probe_run{1,2,3}.txt` retains three runs after compilation completed. Each
mixer cell has 50 warmups and 500 retained samples; each schedule-activation
cell has 50 warmups and 200 retained samples. All voices use the same resident
stereo PCM buffer, so cache behavior is optimistic. Schedule construction and
voice reset are outside the timed region. Times are wall-clock durations and
include any OS preemption. Effects and the full backend callback are excluded.

`worker_probe_run{1,2,3}.txt` retains three runs. Each chart has 50 warmup
transitions and 400 retained transitions, alternating press/release before
activation. The fake song clock stays at zero. Sound-commit samples cover
presses only. Snapshot reads actively yield/check until the corresponding
transition is published. The fake sink measures command dispatch rather than
audio onset; pre-activation events intentionally isolate publication from
normal judgement work. Large charts are scaling stress cases.

The report uses the median of each statistic across the three retained runs.
These small probes establish code-path behavior and scaling, not a device
latency guarantee. Initial exploratory mixer/worker runs were excluded from
the retained series because compilation/other probe work could overlap.

Functional check executed against existing binaries:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -j 6 \
  -R '^(foundation_av_audio_mix|foundation_av_audio_wrapper_lifecycle|foundation_av_audio_device|realtime_gameplay_worker_tests|realtime_gameplay_authority_policy_tests|realtime_touch_input_router_tests|realtime_gameplay_input_registration_tests|foundation_input_timestamp|foundation_input_windows_realtime_mapping|foundation_av_frame_pacer|gameplay_automatic_authority_tests)$'
```

Result: 11/11 passed. No production-code edits, full application rebuild,
physical non-macOS run, or distribution action was part of this audit.

## Fix verification probes

`worker_snapshot_fix.txt` compares the unchanged preparation-publication probe
against the before/after worker implementations in alternating serialized runs.
The full raw results, including scheduling outliers, are retained there.

`backend_timing_probe.cpp` uses the production backend factory, renders silence,
and performs two start/stop cycles for each of three callback preferences.
`backend_timing_fix.txt` retains its output. To reproduce on this checkout:

```sh
printf '#define MINIAUDIO_IMPLEMENTATION\n#include <miniaudio.h>\n' \
  > /tmp/asobmashow-miniaudio-probe.cpp
c++ -std=c++23 -O2 -Isrc -Iinclude \
  -Icmake-build-debug/vcpkg_installed/arm64-osx/include \
  docs/reviews/evidence/2026-10-01-gameplay-latency/backend_timing_probe.cpp \
  /tmp/asobmashow-miniaudio-probe.cpp src/audio/AudioBackend.cpp \
  src/audio/AudioMix.cpp src/settings/AudioVideoSettings.cpp \
  cmake-build-debug/vcpkg_installed/arm64-osx/debug/lib/libportaudio.a \
  -framework CoreAudio -framework AudioToolbox -framework AudioUnit \
  -framework CoreFoundation -framework CoreServices \
  -o /tmp/asobmashow-backend-timing-probe
/tmp/asobmashow-backend-timing-probe
```

Its native timestamp lead is relative to callback receipt in the steady clock
epoch. It does not measure acoustic output or exercise the application mixer.
