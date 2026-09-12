"""Original 128 BPM arcade-funk cue: Signal Select, 32 bars in D minor.

Eight-bar statements form A / A+ / bridge / A-final. A beat-anchored rising hook
and a falling answer recur over voiced ninth chords; the bridge changes the
harmony and register, not the identity of the tune. All event times are beats.
Finite note releases and delay taps wrap around the score, not into silence.
"""

from array import array
from dataclasses import dataclass
from functools import lru_cache
import math
import random


BPM = 128
BEATS_PER_BAR = 4
BAR_COUNT = 32
TOTAL_BEATS = BAR_COUNT * BEATS_PER_BAR
PERCUSSION = ("kick", "snare", "clap", "hat", "open_hat", "rim", "tom")


@dataclass(frozen=True)
class Event:
    instrument: str
    beat: float
    note: int
    duration: float
    velocity: float


def midi_note(name):
    pitch_classes = {"C": 0, "C#": 1, "D": 2, "Eb": 3, "E": 4,
                     "F": 5, "F#": 6, "G": 7, "Ab": 8, "A": 9,
                     "Bb": 10, "B": 11}
    return 12 * (int(name[-1]) + 1) + pitch_classes[name[:-1]]


def add_phrase(score, bar, instrument, phrase, velocity=1.0):
    for offset, note, duration, accent in phrase:
        if not 0 <= offset < BEATS_PER_BAR or duration <= 0:
            raise ValueError("phrase events need bar-relative beats and positive durations")
        score.append(Event(instrument, bar * BEATS_PER_BAR + offset,
                           midi_note(note), duration, accent * velocity))


MAIN_HARMONY = (
    ("D2", ("F3", "A3", "C4", "E4")),
    ("G2", ("F3", "A3", "B3", "E4")),
    ("C2", ("E3", "G3", "B3", "D4")),
    ("A2", ("G3", "Bb3", "C#4", "E4")),
    ("D2", ("F3", "A3", "C4", "E4")),
    ("G2", ("F3", "A3", "B3", "E4")),
    ("E2", ("G3", "Bb3", "D4", "F4")),
    ("A2", ("G3", "B3", "C#4", "E4")),
)
BRIDGE_HARMONY = (
    ("Bb2", ("A3", "C4", "D4", "F4")),
    ("C2", ("G3", "A3", "D4", "E4")),
    ("A2", ("G3", "B3", "C4", "E4")),
    ("D2", ("F3", "A3", "C4", "E4")),
    ("G2", ("F3", "A3", "Bb3", "D4")),
    ("C2", ("E3", "Bb3", "D4", "A4")),
    ("F2", ("E3", "A3", "C4", "G4")),
    ("A2", ("G3", "Bb3", "C#4", "E4")),
)
HOOK = (
    ((0, "A4", .70, .94), (1, "C5", .65, .82),
     (2, "D5", .70, 1), (3, "E5", .30, .84), (3.5, "D5", .35, .78)),
    ((0, "B4", .70, .86), (1, "A4", .28, .80),
     (1.5, "G4", .32, .70), (2, "A4", .65, .87), (3, "B4", .70, .88)),
    ((0, "G4", .70, .89), (1, "B4", .65, .78),
     (2, "D5", .70, .98), (3, "E5", .30, .84), (3.5, "D5", .35, .75)),
    ((0, "C#5", .70, .86), (1, "B4", .65, .74),
     (2, "A4", .70, .87), (3, "E5", .65, .77)),
    ((0, "A4", .70, .95), (1, "C5", .65, .82),
     (2, "D5", .70, 1), (3, "F5", .30, .86), (3.5, "E5", .35, .78)),
    ((0, "D5", .70, .90), (1, "B4", .28, .83),
     (1.5, "A4", .32, .70), (2, "G4", .65, .83), (3, "E4", .70, .78)),
    ((0, "G4", .70, .88), (1, "Bb4", .65, .80),
     (2, "D5", .70, .96), (3, "F5", .30, .83), (3.5, "E5", .35, .70)),
    ((0, "C#5", .70, .89), (1, "B4", .65, .73),
     (2, "A4", .70, .88), (3, "G4", .22, .78), (3.5, "C#5", .30, .67)),
)
BRIDGE = (
    ((0, "F4", .70, .90), (1, "A4", .65, .78), (2, "C5", 1.5, .91)),
    ((0, "B4", .30, .83), (.5, "A4", .65, .72),
     (2, "G4", .65, .82), (3, "E4", .65, .74)),
    ((0, "E4", .70, .83), (1, "G4", .65, .75), (2, "B4", 1.5, .91)),
    ((0, "A4", .65, .84), (1, "F4", .65, .76),
     (2, "E4", .65, .78), (3, "D4", .70, .83)),
    ((0, "D5", .70, .88), (1, "Bb4", .65, .78),
     (2, "A4", .65, .86), (3, "G4", .65, .80)),
    ((0, "E4", .70, .82), (1, "G4", .65, .74),
     (2, "A4", .70, .87), (3, "Bb4", .65, .81)),
    ((0, "C5", .70, .90), (1, "A4", .65, .80),
     (2, "G4", .65, .82), (3, "E4", .65, .76)),
    ((0, "E4", .30, .84), (.5, "G4", .30, .67),
     (1, "A4", .65, .81), (2, "B4", .28, .90),
     (2.5, "C#5", .28, .76), (3, "E5", .65, .92)),
)


def compose_select():
    score = []
    harmony = MAIN_HARMONY * 2 + BRIDGE_HARMONY + MAIN_HARMONY
    for bar, (root, chord) in enumerate(harmony):
        section, phrase_index = divmod(bar, 8)
        bridge = section == 2
        phrase = BRIDGE[phrase_index] if bridge else HOOK[phrase_index]
        add_phrase(score, bar, "lead", phrase, .84 if bridge else 1.0)
        if section in (1, 3) and phrase_index % 2:
            answer = ((2.50, chord[-1], .18, .57),
                      (3, chord[-2], .18, .45), (3.5, chord[1], .16, .40))
            add_phrase(score, bar, "bell", answer)
        root_note = midi_note(root)
        next_root = midi_note(harmony[(bar + 1) % BAR_COUNT][0])
        intervals = {(midi_note(note) - root_note) % 12 for note in chord}
        fifth = 6 if 6 in intervals else 7
        seventh = 11 if 11 in intervals else 10 if 10 in intervals else 9
        bass_line = ((0, 0, .62, 1), (.75, 12, .21, .66),
                     (1.5, fifth, .38, .82), (2.25, seventh, .22, .69),
                     (2.75, 0, .48, .96))
        if bridge and phrase_index < 4:
            bass_line = ((0, 0, 1.15, .90), (1.75, fifth, .65, .76),
                         (3, 12, .38, .75))
        for offset, interval, duration, velocity in bass_line:
            score.append(Event("bass", bar * 4 + offset, root_note + interval,
                               duration, velocity))
        score.append(Event("bass", bar * 4 + 3.75, next_root - 1, .19, .61))
        stabs = ((.0, .70, .73), (1.5, .32, .58), (2.75, .63, .69))
        if bridge:
            stabs = ((0, 1.2, .64), (2.5, .90, .57))
        for offset, duration, velocity in stabs:
            for voice_index, note in enumerate(chord):
                score.append(Event("keys", bar * 4 + offset + voice_index * .012,
                                   midi_note(note), duration, velocity))
        kicks = (0, 1, 2, 3)
        if bridge and phrase_index < 4:
            kicks = (0, 2)
        for offset in kicks:
            score.append(Event("kick", bar * 4 + offset, 0, .25, 1))
        for offset in (1, 3):
            score.append(Event("snare", bar * 4 + offset, 0, .25, .86))
            if not bridge or phrase_index >= 4:
                score.append(Event("clap", bar * 4 + offset, 0, .25, .35))
        for step in range(8):
            offset = step * .5
            score.append(Event("hat", bar * 4 + offset, 0, .12,
                               .42 if step % 2 else .26))
        if section != 2 or phrase_index >= 4:
            for offset in (1.5, 3.5):
                score.append(Event("open_hat", bar * 4 + offset, 0, .20, .28))
            if bar % 2:
                for offset in (2.25, 3.25):
                    score.append(Event("hat", bar * 4 + offset, 0, .12, .18))
        if bar % 4 == 2:
            score.append(Event("rim", bar * 4 + 3.25, 0, .12, .32))
        if phrase_index == 7:
            for offset, note, velocity in ((3.25, 50, .44), (3.5, 47, .52),
                                            (3.75, 43, .65)):
                score.append(Event("tom", bar * 4 + offset, note, .25, velocity))
    return tuple(score)


@lru_cache(maxsize=192)
def voice(instrument, note, gate_samples, sample_rate):
    gate = gate_samples / sample_rate
    durations = {"kick": .29, "snare": .24, "clap": .19, "hat": .052,
                 "open_hat": .20, "rim": .065, "tom": .22}
    release = {"bass": .045, "keys": .20, "lead": .075, "bell": .26}.get(instrument, 0)
    length = round(durations.get(instrument, gate + release) * sample_rate)
    samples = array("f", [0]) * length
    frequency = 440 * 2 ** ((note - 69) / 12)
    noise = random.Random(0xA50B + note * 31 + sum(map(ord, instrument)))
    low = 0.0
    smoothing = 1 - math.exp(-2 * math.pi * 3200 / sample_rate)
    for index in range(length):
        time = index / sample_rate
        phase = 2 * math.pi * frequency * time
        if instrument in PERCUSSION:
            white = noise.uniform(-1, 1)
            low += smoothing * (white - low)
            high = white - low
            attack = min(1.0, time / .001)
            if instrument == "kick":
                kick_phase = 2 * math.pi * (49 * time + 115 * .025 * (1 - math.exp(-time / .025)))
                sample = math.sin(kick_phase) * math.exp(-time / .072)
                sample += .12 * high * math.exp(-time / .005)
            elif instrument == "snare":
                sample = .62 * high * math.exp(-time / .065)
                sample += .30 * math.sin(2 * math.pi * 185 * time) * math.exp(-time / .033)
            elif instrument == "clap":
                envelope = sum(math.exp(-(time - pulse) / .014)
                               for pulse in (0, .010, .021) if time >= pulse)
                sample = high * .65 * envelope
            elif instrument in ("hat", "open_hat"):
                decay = .012 if instrument == "hat" else .053
                sample = high * math.exp(-time / decay)
            elif instrument == "rim":
                sample = (math.sin(2 * math.pi * 790 * time) +
                          .45 * math.sin(2 * math.pi * 1730 * time)) * math.exp(-time / .012)
            else:
                tom_phase = 2 * math.pi * (frequency * time + 45 * .025 * (1 - math.exp(-time / .025)))
                sample = math.sin(tom_phase) * math.exp(-time / .060)
            samples[index] = sample * attack * min(1.0, (length - 1 - index) / (sample_rate * .005))
            continue
        attack = min(1.0, time / (.008 if instrument == "keys" else .004))
        envelope = attack * min(1.0, max(0.0, (gate + release - time) / release))
        if instrument == "bass":
            brightness = math.exp(-time / .11)
            sample = math.sin(phase)
            sample += brightness * (.30 * math.sin(2 * phase) + .10 * math.sin(3 * phase))
            envelope *= .68 + .32 * math.exp(-time / .14)
        elif instrument == "keys":
            sample = math.sin(phase + 1.25 * math.exp(-time / .095) * math.sin(2 * phase))
            sample += .13 * math.sin(2 * phase) * math.exp(-time / .13)
            envelope *= math.exp(-time / .43)
        elif instrument == "lead":
            sample = math.sin(phase + .42 * math.exp(-time / .12) * math.sin(2 * phase))
            if 3 * frequency < .45 * sample_rate:
                sample += .10 * math.sin(3 * phase)
            envelope *= .66 + .34 * math.exp(-time / .10)
        elif instrument == "bell":
            sample = math.sin(phase + 1.1 * math.exp(-time / .05) * math.sin(3 * phase))
            envelope *= math.exp(-time / .15)
        else:
            raise ValueError("unknown instrument: " + instrument)
        samples[index] = sample * envelope
    return samples


def render_events(events, total_beats=TOTAL_BEATS, sample_rate=44100):
    samples_per_beat = sample_rate * 60 / BPM
    length = round(total_beats * samples_per_beat)
    output = array("f", [0]) * length
    for event in events:
        if not 0 <= event.beat < total_beats or event.duration <= 0:
            raise ValueError("event outside the score or with nonpositive duration")
        onset = round(event.beat * samples_per_beat)
        samples = voice(event.instrument, event.note,
                        round(event.duration * samples_per_beat), sample_rate)
        for index, sample in enumerate(samples):
            destination = (onset + index) % length
            output[destination] += sample * event.velocity
    return output


def delayed_sample(samples, index, sample_rate, taps):
    return sum(samples[(index - round(seconds * sample_rate)) % len(samples)] * gain
               for seconds, gain in taps)


def render_select(sample_rate=44100):
    score = compose_select()
    drums = render_events((event for event in score if event.instrument in PERCUSSION),
                          sample_rate=sample_rate)
    bass = render_events((event for event in score if event.instrument == "bass"),
                         sample_rate=sample_rate)
    keys = render_events((event for event in score if event.instrument == "keys"),
                         sample_rate=sample_rate)
    melody = render_events((event for event in score if event.instrument in ("lead", "bell")),
                           sample_rate=sample_rate)
    duck = array("f", [1]) * len(drums)
    for event in score:
        if event.instrument != "kick":
            continue
        onset = round(event.beat * sample_rate * 60 / BPM)
        for index in range(round(sample_rate * .23)):
            time = index / sample_rate
            gain = 1 - .28 * (1 - math.exp(-time / .002)) * math.exp(-time / .07)
            destination = (onset + index) % len(duck)
            duck[destination] = min(duck[destination], gain)
    delay = .5 * 60 / BPM
    output = array("f", [0]) * len(drums)
    for index in range(len(output)):
        lead = melody[index] + delayed_sample(melody, index, sample_rate,
                                              ((delay, .10), (delay * 2, .035)))
        chords = keys[index] + delayed_sample(keys, index, sample_rate,
                                              ((.113, .075), (.173, .045)))
        mix = .64 * drums[index] + duck[index] * (.33 * bass[index] + .15 * chords)
        mix += .25 * lead
        output[index] = math.tanh(mix * 1.15) / 1.15
    mean = sum(output) / len(output)
    for index in range(len(output)):
        output[index] -= mean
    voice.cache_clear()
    return output
