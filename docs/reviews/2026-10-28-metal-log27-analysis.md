# Capture 27: built-in rendering dominates the measured scene interval

The new instrumentation separates built-in preparation and rendering from the
surrounding scene. In the retained strictly active manual-play frames, built-in
rendering accounts for about **82% of aggregate scene-render elapsed time**.
That interval still combines note traversal, HUD, effects and submission work;
it does not identify which individual operation dominates.

Source: user-supplied `AsoBMaShow-performance-log 27.txt`, SHA-256
`e7ab45e847dae3f150753548ffd504d601f62e44cc247322bb54edfd226ae375`.
Raw paths are private and are not committed. Source inspected at `3fad892e`;
the capture itself does not identify its build or device. The v2 ring contains
8,192 consecutive tokens 14450–22641, over 68.859 seconds, generation zero,
with zero reported dropped updates and no open or invalid stage masks. Nine
records lack published CPU data. A phase mask of zero includes these records;
it must not be interpreted as proof of a non-gameplay scene.

## Strict active cohort

Selecting phase mask 4, manual mode, collected CPU data and no invalid/open
stages yields **7,344 frames** over 61.259 seconds; all select built-in
presentation. Values are inclusive elapsed milliseconds, not CPU busy time;
quantiles interpolate at `(n-1)*p` with no outlier removal. The render child
stages must not be added to their parent.

| Interval | Median | P99 | Max |
| --- | ---: | ---: | ---: |
| Scene render | 0.796 | 1.323 | 1.954 |
| Built-in preparation | 0.014 | 0.116 | 0.159 |
| Built-in render | 0.654 | 1.107 | 1.568 |
| Capture | 0.014 | 0.031 | 0.048 |
| `bgfx::frame` | 0.269 | 1.561 | 2.214 |
| Preparation to present request | 1.140 | 2.922 | 4.440 |
| Whole command-buffer GPU | 0.766 | 0.924 | 1.960 |

All strict cohort requests precede their deadline by at least 3.850 ms and all
have display timestamps within −0.033 to +0.033 ms of target. The 7,335
consecutive-token displayed intervals range from 8.333167 to 8.333708 ms.
This does not establish that the entire song or every gameplay-tagged callback
was hitch-free.

**Frame 16064** is gameplay-tagged, but CPU data was not published, so its
phase and inner costs are unknown. It requested presentation 1.271 ms after
its deadline and has no display timestamp. Its surrounding displayed frames
are 16.667 ms apart. The missing timestamp cannot prove what happened to that
frame, but the neighboring gap and late request warrant calling it a possible
missed refresh rather than treating the capture as uniformly smooth. Frame
preparation took 0.869 ms, and GPU execution 0.762 ms; the remaining submit
interval is not subdivided by this capture.

## End and transition

- Frame 21826 crosses post-notes to ending. Result preparation/persistence
  takes 60.624 ms within 61.675 ms scene update; the preceding display gap is
  66.668 ms. This does not isolate an individual operation or storage write.
- Frame 22067 takes 364.075 ms in deferred scene work after submitting; frame
  22068 then spends 43.496 ms in `bgfx::frame`. Their displayed timestamps are
  408.340 ms apart. These are transition frames, not normal note play.

For active rendering, instrument `BMSRenderer::renderFrame` in useful
nonoverlapping sections (playfield traversal/submission, HUD, and remaining
scene UI) before choosing a change. The preparation timer is already small.
For `bgfx::frame`, source contains encoder and render-thread waits as well as
frame swapping; current timestamps cannot split them. bgfx's reported wait
statistics need verified frame ownership and units before being joined to these
tokens. No application optimization or device-latency conclusion follows from
this capture alone. Different retained windows and conditions prevent causal
comparison with capture 26.
