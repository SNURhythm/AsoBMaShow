# Synthetic acceptance chart assets

`acceptance_7k.bms` and the four adjacent assets are tiny, purpose-built
AsoBMaShow fixtures. They contain no third-party chart, audio, image, font,
video, or skin payload.

The BMS is deliberately silent and deterministic. It covers seven-key normal
notes, simultaneous chords, an `#LNOBJ` long-note ending, runnable `#LNTYPE 1`
long channels, landmine channels (`D1`-`D9`), base/layer/poor BGA sequences,
an MP4 BGA reference, BPM changes, STOPs, and SCROLL changes. It does not claim
invisible-note coverage. The final note and BGA event provide a fixed song-end
marker. The normal notes, long-note phases, chords, mines, and timing changes
exercise the runtime's judgment, combo, gauge, lane-cover, speed, retry, and
song-end paths when a harness drives it.

The chart intentionally omits `#LNMODE`, so the undefined long-channel data is
interpreted using the selected runtime mode. A single static chart does not
simultaneously encode LN, CN, and HCN. Acceptance therefore requires a separate
LN runtime/autoplay run, a separate CN runtime/autoplay run, and a separate HCN
runtime/autoplay run over the same long-channel data.

Channel 06 uses the visible green layer card as its poor frame and the file
named `acceptance_bga_miss.png` as a distinct nonzero transparent gap frame.
The historical filename does not imply that the transparent card is a visible
miss image.

Static BMS cannot encode player input, a retry action, HUD assertions, or the
six display/layout captures. Those remain explicitly pending runtime work:
one deterministic autoplay script per manifest scenario, a controlled miss and
retry sequence, and physical iPad evidence for the configured display and
refresh rate. Do not mark acceptance evidence complete merely because this
fixture parses.

The base and layer PNG files are 2x2 opaque color cards; the gap PNG is a 2x2
fully transparent card. The MP4 is a one-frame, silent, generated color card.
Regenerate only with local deterministic tooling and update the fixture
contract test hash in the same review.
