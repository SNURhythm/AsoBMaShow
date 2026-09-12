import importlib.util
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "generate_select_sounds", ROOT / "scripts/generate_select_sounds.py")
synth = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = synth
SPEC.loader.exec_module(synth)
import select_bgm as score


def rms(samples):
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


class SelectArrangementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        synth.SR = 8000
        cls.samples = synth.make_select()

    def test_score_is_repeatable_and_fills_every_bar(self):
        self.assertEqual(score.compose_select(), score.compose_select())
        self.assertEqual(self.samples, synth.make_select())
        self.assertEqual(len(self.samples), 60 * synth.SR)
        self.assertTrue(all(math.isfinite(sample) for sample in self.samples))
        melody = [event for event in score.compose_select() if event.instrument == "lead"]
        self.assertEqual({int(event.beat // 4) for event in melody}, set(range(32)))

    def test_phrase_offsets_are_relative_once_and_later_notes_sound(self):
        events = []
        score.add_phrase(events, 3, "lead", ((.5, "A4", .25, 1),))
        self.assertEqual(events[0].beat, 12.5)
        samples = score.render_events(events, total_beats=16, sample_rate=synth.SR)
        onset = round(12.5 * synth.SR * 60 / score.BPM)
        self.assertEqual(max(abs(sample) for sample in samples[:onset]), 0)
        self.assertGreater(rms(samples[onset:onset + 200]), .1)
        with self.assertRaises(ValueError):
            score.add_phrase([], 3, "lead", ((12.5, "A4", .25, 1),))

    def test_lead_and_kick_share_the_bar_downbeat_and_midpoint(self):
        events = score.compose_select()
        for bar in range(32):
            for beat in (bar * 4, bar * 4 + 2):
                with self.subTest(bar=bar, beat=beat):
                    aligned = [event for event in events if event.beat == beat]
                    self.assertIn("lead", {event.instrument for event in aligned})
                    self.assertIn("kick", {event.instrument for event in aligned})
                    for instrument in ("lead", "kick"):
                        event = next(event for event in aligned if event.instrument == instrument)
                        rendered = score.render_events((event,), sample_rate=8000)
                        onset = round(beat * 3750)
                        self.assertGreater(rms(rendered[onset + 8:onset + 80]), .01)
                        if onset:
                            self.assertEqual(rms(rendered[onset - 80:onset]), 0)

    def test_lead_uses_straight_eighths_against_an_exact_backbeat(self):
        for event in score.compose_select():
            with self.subTest(instrument=event.instrument, beat=event.beat):
                if event.instrument == "lead":
                    self.assertEqual(event.beat * 2, round(event.beat * 2))
                elif event.instrument == "snare":
                    self.assertIn(event.beat % 4, (1, 3))

    def test_note_releases_wrap_into_next_lap(self):
        samples = score.render_events((score.Event("lead", 3.9, 69, .3, 1),),
                                      total_beats=4, sample_rate=synth.SR)
        self.assertGreater(rms(samples[:200]), .1)
        self.assertEqual(rms(samples[2000:3000]), 0)

    @unittest.skipUnless(shutil.which("sndfile-convert"), "libsndfile tools not installed")
    def test_ogg_roundtrip_keeps_headroom_and_replaces_legacy_wav(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            legacy = root / "assets/select.wav"
            legacy.parent.mkdir()
            legacy.write_bytes(b"old music")
            synth.write_select_ogg(root, self.samples, shutil.which("sndfile-convert"))
            self.assertFalse(legacy.exists())
            encoded = root / "assets/select.ogg"
            self.assertLess(encoded.stat().st_size, 1024 * 1024)
            subprocess.run(["sndfile-convert", "-pcm16", str(encoded), str(root / "decoded.wav")],
                           check=True)
            rate, decoded = synth.read_wav(root / "decoded.wav")
            self.assertEqual((rate, len(decoded)), (synth.SR, len(self.samples)))
            self.assertLess(max(abs(sample) for sample in decoded), 31000)

    def test_encoder_failure_preserves_existing_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            for name in ("select.wav", "select.ogg"):
                (assets / name).write_bytes(b"previous audio")
            with self.assertRaises(OSError):
                synth.write_select_ogg(root, self.samples[:synth.SR], str(root / "missing-encoder"))
            for name in ("select.wav", "select.ogg"):
                self.assertEqual((assets / name).read_bytes(), b"previous audio")
            self.assertEqual(len(list(assets.iterdir())), 2)

    def test_ui_effects_remain_byte_identical(self):
        with tempfile.TemporaryDirectory() as root, patch.object(synth, "SR", 44100):
            for path, render in synth.WRITERS:
                synth.write_wav(root, path, render())
                self.assertEqual((Path(root) / path).read_bytes(), (ROOT / path).read_bytes())

    def test_seam_check_rejects_a_click_without_a_silent_gap(self):
        samples = [.1 * math.sin(2 * math.pi * 100 * index / synth.SR)
                   + (.4 if index >= synth.SR // 2 else 0)
                   for index in range(synth.SR)]
        with tempfile.TemporaryDirectory() as root:
            synth.write_wav(root, "click.wav", samples)
            with self.assertRaisesRegex(ValueError, "click"):
                synth.loop_seam_check(root, "click.wav")

    def test_loop_keeps_its_groove_across_the_boundary(self):
        edge = synth.SR // 20
        window = synth.SR // 2
        self.assertGreater(rms(self.samples[:edge]),
                           0.3 * rms(self.samples[:window]),
                           "the downbeat must not fade up from silence each lap")
        self.assertGreater(rms(self.samples[-edge:]),
                           0.3 * rms(self.samples[-window:]),
                           "the turnaround must not fade away before the loop")

    def test_seam_check_rejects_a_silent_gap_even_without_a_click(self):
        samples = [0.25 * math.sin(2 * math.pi * 100 * index / synth.SR)
                   for index in range(synth.SR)]
        for index in range(synth.SR // 10):
            samples[index] = 0.0
            samples[-1 - index] = 0.0
        with tempfile.TemporaryDirectory() as root:
            synth.write_wav(root, "gap.wav", samples)
            with self.assertRaisesRegex((AssertionError, ValueError), "gap|silent|energy"):
                synth.loop_seam_check(root, "gap.wav")


if __name__ == "__main__":
    unittest.main()
