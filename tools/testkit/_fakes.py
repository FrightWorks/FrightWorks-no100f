"""Test helpers: a stand-in game built into a temp folder (no real game, no window)."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit.launch import TestGame  # noqa: E402

FAKE = Path(__file__).resolve().parent / "fake_game.py"


class FakeGameCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.src = root / "srcPlay"
        (self.src / "button_icons").mkdir(parents=True)
        (self.src / "button_icons" / "a.png").write_bytes(b"png")
        (self.src / "FrightWorks-No100F-Game.exe").write_bytes(b"MZ fake")
        (self.src / "SDL3.dll").write_bytes(b"dll")
        (self.src / "texture_replacements" / "ScoobyHD").mkdir(parents=True)
        (self.src / "texture_replacements" / "ScoobyHD" / "t.png").write_bytes(b"png")
        self.disc = root / "Disc" / "DATA"
        (self.disc / "files").mkdir(parents=True)
        (self.disc / "files" / "sd2.ini").write_text("BOOT=none\nShowMenuOnBoot=1\neSPECIAL_Spring=0\n", encoding="latin-1")
        self.state = root / "state"
        self.state.mkdir()
        self.games = []

    def game(self, name="fake", mode="normal", env=None, **kw):
        extra = {"FAKE_MODE": mode, "FAKE_STATE_DIR": str(self.state), "FAKE_BOOT_DELAY": "0.3"}
        extra.update(env or {})
        work = Path(self.tmp.name) / "work" / f"{name}-{len(self.games)}"
        g = TestGame(name, work, boot=kw.pop("boot", "h001"), src_play=self.src, disc_data=self.disc,
                     game_cmd=[sys.executable, str(FAKE)], extra_env=extra, **kw)
        g.prepare()
        self.games.append(g)
        self.addCleanup(g.cleanup)
        return g
