"""Tests of launch.py with no real game: python -m unittest discover -s tools/testkit -p "test_*.py" """
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit import launch  # noqa: E402
from testkit._fakes import FakeGameCase  # noqa: E402


class EnvTest(unittest.TestCase):
    def test_scrubs_leftover_switches_and_isolates_controllers(self):
        base = {"PATH": "/bin", "NO100F_SKIP_LINES": "1", "AX_DIAG_DIR": "x", "NO100F_SCRIPT_REMOTES": "2",
                "AUDIO_RAW_DUMP": "x.raw", "BUTTON_ICONS": "xbox", "AURORA_GFX_FLAGS": "validate", "NO100F_FLAT_TEST": "1",
                "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1", "SDL_GAMECONTROLLER_IGNORE_DEVICES": "0x1/0x2",
                "SDL_WINDOW_ACTIVATE_WHEN_SHOWN": "1", "NO100F_INPUT_SCRIPT": "1000:A"}
        env = launch.test_env(base)
        self.assertEqual(env["PATH"], "/bin")
        for gone in ("NO100F_SKIP_LINES", "AX_DIAG_DIR", "NO100F_SCRIPT_REMOTES", "AUDIO_RAW_DUMP", "BUTTON_ICONS",
                     "AURORA_GFX_FLAGS", "NO100F_FLAT_TEST", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS",
                     "SDL_GAMECONTROLLER_IGNORE_DEVICES", "SDL_WINDOW_ACTIVATE_WHEN_SHOWN"):
            self.assertNotIn(gone, env)
        self.assertEqual(env["NO100F_INPUT_SCRIPT"], launch.DUMMY_PAD_SCRIPT)  # the caller's script is replaced
        self.assertEqual(env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"], launch.IGNORE_PADS_EXCEPT)
        self.assertEqual(env["NO100F_DEV_TOOLS"], "1")

    def test_background_events_hint_can_not_come_back_through_extra(self):
        env = launch.test_env({}, extra={"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1", "AUDIO_RAW_DUMP": "a.raw"})
        self.assertNotIn("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", env)
        self.assertEqual(env["AUDIO_RAW_DUMP"], "a.raw")  # a test's own switches do pass

    def test_ignore_id_is_well_formed(self):
        self.assertRegex(launch.IGNORE_PADS_EXCEPT, r"^0x[0-9A-Fa-f]{4}/0x[0-9A-Fa-f]{4}$")


class ConfigTest(unittest.TestCase):
    def test_config_is_valid_toml_with_rumble_off_and_sound_muted(self):
        text = launch.test_config("D:\\Disc\\DATA", hd=True)
        self.assertIn("rumble = false", text)
        self.assertIn("muted = true", text)
        self.assertIn("texture_replacements = true", text)
        self.assertIn("dvd_root = 'D:/Disc/DATA'", text)
        self.assertIn("texture_replacements = false", launch.test_config("x", hd=False))
        try:
            import tomllib
        except ImportError:
            return
        data = tomllib.loads(text)
        self.assertIs(data["controller"]["rumble"], False)
        self.assertIs(data["audio"]["muted"], True)
        self.assertEqual(data["video"]["display_mode"], "windowed")

    def test_boot_ini(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "files").mkdir()
            (Path(d) / "files" / "sd2.ini").write_text("BOOT=xx\nShowMenuOnBoot=1\n", encoding="latin-1")
            self.assertEqual(launch.boot_ini(d, "i004"), "BOOT=i004\nShowMenuOnBoot=0\n")


def git(cwd, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args], cwd=str(cwd), check=True,
                   capture_output=True)


class BuildIdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        (self.repo / "launcher").mkdir()
        (self.repo / "launcher" / "FrightWorks.Launcher.csproj").write_text("<Version>2.3.4</Version>")
        git(self.repo, "init", "-q", "-b", "work")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "x")

    def test_clean_checkout(self):
        bid = launch.expected_build_id(self.repo)
        self.assertRegex(str(bid), r"^2\.3\.4 work@[0-9a-f]{7}$")
        self.assertEqual(bid.changes, "")

    def test_uncommitted_change_adds_a_hash(self):
        (self.repo / "launcher" / "FrightWorks.Launcher.csproj").write_text("<Version>2.3.4</Version>\n")
        bid = launch.expected_build_id(self.repo)
        self.assertRegex(str(bid), r"^2\.3\.4 work@[0-9a-f]{7}\+[0-9a-f]{4}$")

    def test_exe_scan_finds_the_id_in_binary_data(self):
        blob = b"\x00\x01junk\x00" + b"2.3.4 work@abcdef0" + b"\x00more\xff\xfe"
        self.assertEqual(launch.find_build_id(blob.decode("latin-1")), "2.3.4 work@abcdef0")
        self.assertEqual(launch.find_build_id("2.3.4 work@abcdef0+1a2b"), "2.3.4 work@abcdef0+1a2b")
        self.assertEqual(launch.find_build_id("no id here"), "")
        # the real exe has the pieces as separate strings first, NUL separated (seen in a 1.1.1 build)
        real = b"zMe\x001.1.1\x00tools/smoke-test\x002116fe1\x00\x001.1.1 tools/smoke-test@2116fe1\x00\x00\x01"
        self.assertEqual(launch.find_build_id(real.decode("latin-1")), "1.1.1 tools/smoke-test@2116fe1")

    def test_check_build_exact_id_and_garbage(self):
        exp = launch.BuildId("2.3.4", "work", "abcdef0", "")
        self.assertEqual(launch.check_build("2.3.4 work@abcdef0", exp), (True, "build ID 2.3.4 work@abcdef0"))
        self.assertFalse(launch.check_build("", exp)[0])
        self.assertFalse(launch.check_build("2.3.4 work@1234567", exp)[0])  # no repo: only the exact ID passes
        self.assertFalse(launch.check_build("2.3.4 other@abcdef0", exp)[0])

    def commit(self, rel, text="x"):
        p = self.repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", rel)

    def built_id(self):
        return str(launch.expected_build_id(self.repo))

    def test_head_ahead_by_test_and_doc_files_only_is_accepted(self):
        built = self.built_id()
        self.commit("tools/testkit/new.py")
        self.commit("tools/smoke-test.py")
        self.commit("docs/testing.md")
        self.commit(".agents/REPO.md")
        self.commit("README.md")
        (self.repo / "autotest").mkdir()
        ok, msg = launch.check_build(built, launch.expected_build_id(self.repo), self.repo)
        self.assertTrue(ok, msg)
        self.assertIn("only test/doc files differ", msg)
        self.assertIn("and 1 more", msg)  # 5 files, 4 of them named
        self.assertIn(built.split("@")[1], msg)

    def test_uncommitted_test_file_edit_is_accepted_too(self):
        built = self.built_id()
        self.commit("tools/testkit/a.py")
        (self.repo / "tools" / "testkit" / "a.py").write_text("edited")
        exp = launch.expected_build_id(self.repo)
        self.assertNotEqual(exp.changes, "")
        self.assertTrue(launch.check_build(built, exp, self.repo)[0])

    def test_a_game_file_changed_since_the_build_fails_and_says_which(self):
        built = self.built_id()
        self.commit("tools/testkit/a.py")
        self.commit("wiicompiled/runtime/src/main.cpp")
        ok, msg = launch.check_build(built, launch.expected_build_id(self.repo), self.repo)
        self.assertFalse(ok)
        self.assertIn("wiicompiled/runtime/src/main.cpp", msg)
        self.assertIn("rebuild", msg)

    def test_uncommitted_game_change_fails(self):
        built = self.built_id()
        (self.repo / "launcher" / "FrightWorks.Launcher.csproj").write_text("<Version>2.3.4</Version>\n")
        ok, msg = launch.check_build(built, launch.expected_build_id(self.repo), self.repo)
        self.assertFalse(ok)
        self.assertIn("csproj", msg)

    def test_a_build_made_from_uncommitted_changes_that_differ_now_fails(self):
        csproj = self.repo / "launcher" / "FrightWorks.Launcher.csproj"
        csproj.write_text("<Version>2.3.4</Version>\n")
        built = self.built_id()
        self.assertIn("+", built)
        self.assertTrue(launch.check_build(built, launch.expected_build_id(self.repo), self.repo)[0])  # same edits: exact ID
        csproj.write_text("<Version>2.3.4</Version>\n\n")
        self.assertFalse(launch.check_build(built, launch.expected_build_id(self.repo), self.repo)[0])

    def test_other_branch_or_unknown_commit_fails(self):
        built = self.built_id()
        self.assertFalse(launch.check_build(built.replace(" work@", " other@"), launch.expected_build_id(self.repo), self.repo)[0])
        self.commit("tools/testkit/a.py")
        ok, msg = launch.check_build("2.3.4 work@1234567", launch.expected_build_id(self.repo), self.repo)
        self.assertFalse(ok)
        self.assertIn("not in this repository", msg)


class LinkTest(unittest.TestCase):
    def test_safe_rmtree_never_deletes_through_a_link(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            precious = d / "main" / "texture_pack"
            precious.mkdir(parents=True)
            (precious / "keep.png").write_bytes(b"x")
            copy = d / "copy"
            (copy / "UserData").mkdir(parents=True)
            (copy / "file.txt").write_text("x")
            launch.make_dir_link(copy / "texture_replacements", precious)
            self.assertTrue(launch.is_link(copy / "texture_replacements"))
            launch.safe_rmtree(copy)
            self.assertFalse(copy.exists())
            self.assertTrue((precious / "keep.png").exists())

    def test_safe_rmtree_of_a_link_itself(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "t").mkdir()
            (d / "t" / "f").write_text("x")
            launch.make_dir_link(d / "l", d / "t")
            launch.safe_rmtree(d / "l")
            self.assertFalse(os.path.lexists(d / "l"))
            self.assertTrue((d / "t" / "f").exists())


class TestGameTest(FakeGameCase):
    def test_prepare_makes_an_own_copy_with_links_and_config(self):
        g = self.game(hd=True)
        self.assertTrue((g.play / "FrightWorks Game.exe").is_file())
        self.assertNotEqual(g.play.resolve(), self.src.resolve())
        self.assertTrue(launch.is_link(g.play / "button_icons"))
        self.assertTrue(launch.is_link(g.play / "texture_replacements"))
        cfg = (g.play / "UserData" / "Config.toml").read_text()
        self.assertIn("rumble = false", cfg)
        self.assertIn("texture_replacements = true", cfg)
        self.assertIn("BOOT=h001", (g.play / "UserData" / "mods" / "zz-smoke" / "files" / "sd2.ini").read_text())
        g.cleanup()
        self.assertTrue((self.src / "texture_replacements" / "ScoobyHD" / "t.png").exists())  # the source survives
        self.assertTrue((self.src / "button_icons" / "a.png").exists())

    def test_hd_off_links_no_pack(self):
        g = self.game(hd=False)
        self.assertFalse(os.path.lexists(g.play / "texture_replacements"))

    def test_set_boot_none_removes_the_boot_mod_and_keeps_saves(self):
        g = self.game()
        saves = g.play / "UserData" / "Saves" / "MemoryCard"
        saves.mkdir(parents=True)
        (saves / "slot.gci").write_bytes(b"save")
        g.set_boot(None)
        self.assertFalse((g.play / "UserData" / "mods" / "zz-smoke").exists())
        self.assertTrue((saves / "slot.gci").exists())

    def test_started_game_gets_isolation_and_reports_its_build(self):
        g = self.game(env={"FAKE_BUILD_ID": "9.9.9 fake@abcdef0"})
        g.start()  # the stand-in asserts on the three environment facts and dies otherwise
        self.assertTrue(g.wait_console("h001.HIP", 20))
        g.check_crash()
        self.assertEqual(launch.find_build_id(g.console_text()), "9.9.9 fake@abcdef0")
        g.stop()
        self.assertFalse(g.alive())

    def test_missing_game_is_a_clear_error(self):
        with tempfile.TemporaryDirectory() as d:
            g = launch.TestGame("none", Path(d) / "w", src_play=Path(d) / "nothing", disc_data=self.disc)
            with self.assertRaises(FileNotFoundError):
                g.prepare()


if __name__ == "__main__":
    unittest.main()
