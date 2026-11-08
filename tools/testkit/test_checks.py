"""Tests of checks.py: python -m unittest discover -s tools/testkit -p "test_*.py" """
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit import checks  # noqa: E402
from testkit._fakes import FakeGameCase  # noqa: E402
from testkit import waitplay  # noqa: E402


def write_png(path, width, height, pixel, channels=3, ftype=0):
    """pixel(x, y) -> tuple of `channels` bytes. ftype: PNG filter 0 (none) or 1 (sub) to exercise the decoder."""
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            line += bytes(pixel(x, y))
        if ftype == 1:
            out = bytearray(line)
            for i in range(len(line) - 1, channels - 1, -1):
                out[i] = (line[i] - line[i - channels]) & 255
            line = out
        rows.append(bytes([ftype]) + bytes(line))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    ctype = {1: 0, 3: 2, 4: 6}[channels]
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ctype, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


class PngTest(unittest.TestCase):
    def test_black_picture_is_blank_and_a_lit_one_is_not(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            write_png(d / "black.png", 64, 36, lambda x, y: (0, 0, 0))
            write_png(d / "dark.png", 64, 36, lambda x, y: (5, 5, 5))
            write_png(d / "lit.png", 64, 36, lambda x, y: (x * 4, y * 7, 90))
            write_png(d / "lit_sub.png", 64, 36, lambda x, y: (x * 4, y * 7, 90), ftype=1)
            write_png(d / "lit_rgba.png", 64, 36, lambda x, y: (200, 100, 50, 255), channels=4)
            write_png(d / "gray.png", 8, 8, lambda x, y: (200,), channels=1)
            self.assertTrue(checks.png_is_blank(d / "black.png"))
            self.assertTrue(checks.png_is_blank(d / "dark.png"))
            self.assertFalse(checks.png_is_blank(d / "lit.png"))
            self.assertEqual(checks.png_pixels_lit(d / "lit.png"), checks.png_pixels_lit(d / "lit_sub.png"))
            self.assertFalse(checks.png_is_blank(d / "lit_rgba.png"))
            self.assertFalse(checks.png_is_blank(d / "gray.png"))

    def test_a_mostly_black_picture_with_a_tiny_overlay_is_still_blank(self):
        with tempfile.TemporaryDirectory() as d:
            write_png(Path(d) / "dev.png", 100, 100, lambda x, y: (255, 255, 255) if x < 10 and y < 2 else (0, 0, 0))
            self.assertTrue(checks.png_is_blank(Path(d) / "dev.png"))  # the DEV stamp alone is not a picture

    def test_not_a_png(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "x.png").write_bytes(b"nope")
            with self.assertRaises(ValueError):
                checks.png_is_blank(Path(d) / "x.png")


class AudioTest(unittest.TestCase):
    def test_peak_and_seconds_of_a_big_endian_dump(self):
        with tempfile.TemporaryDirectory() as d:
            raw = Path(d) / "a.raw"
            raw.write_bytes(struct.pack(">hhhh", 0, 0, -1234, 99) + b"\x00\x00" * 2 * 32000 * 2)
            self.assertEqual(checks.audio_peak(raw), 1234)
            self.assertAlmostEqual(checks.audio_seconds(raw), (8 + 4 * 64000) / 128000, places=3)
            silent = Path(d) / "s.raw"
            silent.write_bytes(b"\x00" * 1000)
            self.assertEqual(checks.audio_peak(silent), 0)
            self.assertEqual(checks.audio_seconds(Path(d) / "missing.raw"), 0.0)

    def test_loopcheck_flags_a_repeating_dump_and_passes_noise(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed")
        tools = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as d:
            rng = np.random.default_rng(1)
            loop = rng.normal(0, 3000, 28000)
            looped = np.tile(loop, 40)[:32000 * 20]
            noise = rng.normal(0, 3000, 32000 * 20)
            for name, mono in (("loop", looped), ("noise", noise)):
                st = np.stack([mono, mono], axis=1).astype(">i2")
                (Path(d) / f"{name}.raw").write_bytes(st.tobytes())
            self.assertGreater(checks.loopcheck(Path(d) / "loop.raw", tools), 0.9)
            self.assertLess(checks.loopcheck(Path(d) / "noise.raw", tools), 0.5)


class HdProbeTest(FakeGameCase):
    def test_probe_file_has_six_tab_separated_fields_per_pack_file(self):
        g = self.game(hd=True)
        n = checks.write_hd_probe(g.play, g.src_play / "texture_replacements" / "ScoobyHD")
        self.assertEqual(n, 1)
        line = (g.play / "UserData" / "devmenu_textures.txt").read_text().splitlines()[0]
        fields = line.split("\t")
        self.assertGreaterEqual(len(fields), 6)  # the game skips shorter lines silently
        self.assertEqual(fields[0], fields[1])

    def test_no_pack_is_not_run(self):
        g = self.game(hd=False)
        with self.assertRaises(checks.CheckNotRun):
            checks.write_hd_probe(g.play, g.play / "nowhere")


class WaitForSceneTest(FakeGameCase):
    def test_follows_a_scene_change(self):
        g = self.game(env={"FAKE_CUTSCENE": "0"}, boot="i004").start()
        waitplay.wait_until_playable(g, "i004", timeout=20, log=lambda *a: None)
        g.devmenu.command("scene e001", 10)
        self.assertEqual(waitplay.wait_for_scene(g, "e001", timeout=10)[0], "e001")
        with self.assertRaises(waitplay.PlayTimeout):
            waitplay.wait_for_scene(g, "h001", timeout=1.5)


class FpsTest(unittest.TestCase):
    TEXT = ("[vi] fps 31.0: game 3.1 ms (gx 1.0 ms)\n[vi] interp: displayed 59.0 fps\n[vi] fps 59.8: game 3 ms\n"
            "[vi] fps 60.0: game 3 ms\nnoise fps 5\n[vi] fps 58.0: game 3 ms\n")

    def test_samples_are_the_fps_lines_only(self):
        self.assertEqual(checks.fps_samples(self.TEXT), [31.0, 59.8, 60.0, 58.0])
        self.assertEqual(checks.fps_samples("nothing here"), [])

    def test_summary_drops_the_loading_window_when_there_are_enough_samples(self):
        s = checks.fps_summary(checks.fps_samples(self.TEXT))
        self.assertEqual(s, {"n": 3, "min": 58.0, "median": 59.8, "last": 58.0})
        self.assertEqual(checks.fps_summary([20.0, 40.0])["median"], 30.0)   # two samples: nothing is dropped
        self.assertEqual(checks.fps_summary([20.0])["n"], 1)
        self.assertIsNone(checks.fps_summary([]))

    def test_slow_is_the_median_below_the_limit(self):
        self.assertTrue(checks.fps_too_slow(checks.fps_summary([10, 12, 11, 12]), 30))
        self.assertFalse(checks.fps_too_slow(checks.fps_summary([10, 59, 60, 61]), 30))   # one dip is not slow
        self.assertFalse(checks.fps_too_slow(None, 30))    # no samples: not "slow", the caller reports "not run"


if __name__ == "__main__":
    unittest.main()
