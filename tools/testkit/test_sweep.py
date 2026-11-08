"""Tests of tools/level-sweep.py (per-level outcomes, retry, slots, workers, report) with the stand-in game, not the real one:
python -m unittest discover -s tools/testkit -p "test_*.py" """
import importlib.util
import json
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from testkit import launch, slots  # noqa: E402
from testkit._fakes import FAKE, FakeGameCase  # noqa: E402
from testkit.test_checks import write_png  # noqa: E402

spec = importlib.util.spec_from_file_location("level_sweep", Path(__file__).resolve().parent.parent / "level-sweep.py")
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


class SweepTest(FakeGameCase):
    def setUp(self):
        super().setUp()
        self.out = Path(self.tmp.name) / "out"
        for sub in ("shots", "logs", "work"):
            (self.out / sub).mkdir(parents=True)
        self.pool = slots.SlotPool(slots=7, state=Path(self.tmp.name) / "kit", poll=0.05)
        self.args = types.SimpleNamespace(timeout=6, seconds=1.5, skip_build_check=True, fps30=False, min_fps=None, workers=2,
                                          slots=7)
        self.env = {"FAKE_STATE_DIR": str(self.state), "FAKE_BOOT_DELAY": "0.3", "FAKE_FPS": "60"}
        self.lit = True
        shots = mock.patch.object(launch.TestGame, "screenshot", self.fake_screenshot)
        shots.start()
        self.addCleanup(shots.stop)

    def fake_screenshot(self, png):
        Path(png).parent.mkdir(parents=True, exist_ok=True)
        v = 120 if self.lit else 0
        write_png(png, 16, 9, lambda x, y: (v, v, v))
        return True

    def configure(self, **env):
        self.args.game_kwargs = dict(src_play=self.src, disc_data=self.disc, game_cmd=[sys.executable, str(FAKE)],
                                     extra_env=dict(self.env, **env))

    def make_game(self, worker=0):
        self.configure()
        game = sweep.new_game(worker, self.args, self.out)
        self.addCleanup(game.cleanup)
        return game

    def one(self, level="h001", **env):
        self.configure(**env)
        game = sweep.new_game(0, self.args, self.out)
        self.addCleanup(game.cleanup)
        return sweep.run_level(level, game, self.args, self.out, self.pool, False)

    # -- outcomes
    def test_pass_with_every_check(self):
        r = self.one(FAKE_MODE="normal")
        self.assertEqual(r["outcome"], "passed", r)
        names = {c: s for c, s, _d in r["checks"]}
        for check in ("playable", "in h001", "alive, no crash or fault", "picture", "frame rate"):
            self.assertEqual(names.get(check), "passed", (check, r["checks"]))
        self.assertEqual(r["times"]["fps"]["median"], 60.0)
        self.assertGreater(r["times"]["playable"], 0)
        self.assertEqual(self.pool.status()["live"], {})  # the slot was given back
        self.assertTrue((self.out / "logs" / "h001-1.err.log").exists())
        self.assertTrue((self.out / "logs" / "h001-1.console.log").exists())

    def test_timeout_then_passed_on_retry(self):
        r = self.one(FAKE_MODE="hang_once")
        self.assertEqual(r["outcome"], "passed on retry", r)
        self.assertEqual(r["attempts"], 2)
        self.assertIn("timed out", r["detail"])

    def test_two_timeouts_fail(self):
        self.args.timeout = 3
        r = self.one(FAKE_MODE="hang")
        self.assertEqual(r["outcome"], "failed: timeout", r)
        self.assertEqual(r["attempts"], 2)

    def test_exit_fails_at_once_and_is_not_retried(self):
        r = self.one(FAKE_MODE="crash")
        self.assertEqual(r["outcome"], "failed: crash (exited)", r)
        self.assertEqual(r["attempts"], 1)

    def test_access_violation_fails_at_once(self):
        r = self.one(FAKE_MODE="fault")
        self.assertEqual(r["outcome"], "failed: crash (access violation)", r)
        self.assertEqual(r["attempts"], 1)
        self.assertIn("AccessViolation", r["detail"])

    def test_crash_kinds(self):
        self.assertEqual(sweep.crash_kind("w0: MKW AccessViolation at 0x80"), "access violation")
        self.assertEqual(sweep.crash_kind("w0: the game exited with code 3"), "exited")
        self.assertEqual(sweep.crash_kind("w0: OS panic: bad thing"), "panic")

    def test_black_picture_fails(self):
        self.lit = False
        r = self.one()
        self.assertEqual(r["outcome"], "failed: picture", r)

    def test_slow_frame_rate_fails_and_the_picture_check_still_ran(self):
        r = self.one(FAKE_FPS="12")
        self.assertEqual(r["outcome"], "failed: frame rate", r)
        self.assertIn(("picture", "passed"), [(c, s) for c, s, _d in r["checks"]])

    def test_min_fps_and_the_30_fps_target(self):
        self.args.fps30 = True
        self.assertEqual(self.one(FAKE_FPS="20")["outcome"], "passed")     # half of 30 is 15
        self.args.fps30 = False
        self.assertEqual(self.one(FAKE_FPS="20")["outcome"], "failed: frame rate")
        self.args.min_fps = 10
        self.assertEqual(self.one(FAKE_FPS="20")["outcome"], "passed")

    def test_no_fps_lines_is_not_run_never_a_pass(self):
        self.env.pop("FAKE_FPS")
        r = self.one()
        self.assertEqual(r["outcome"], "passed (some checks not run)", r)

    def test_black_and_slow_are_both_reported(self):
        self.lit = False
        r = self.one(FAKE_FPS="5")
        self.assertEqual(r["outcome"], "failed: picture", r)
        self.assertIn("frame rate", r["detail"])

    def test_wrong_scene_fails(self):
        game = self.make_game()
        with mock.patch.object(sweep.waitplay, "wait_until_playable",
                               return_value={"seconds": 1.0, "presses": 0, "where": "i004 0 0 0 yaw 0"}):
            r = sweep.run_level("h001", game, self.args, self.out, self.pool, False)
        self.assertEqual(r["outcome"], "failed: in h001", r)

    def test_wrong_build_fails_without_retry(self):
        self.args.skip_build_check = False
        r = self.one()
        self.assertEqual(r["outcome"], "failed: wrong build", r)
        self.assertEqual(r["attempts"], 1)

    def test_error_in_the_test_itself_is_a_failed_level(self):
        game = self.make_game()
        with mock.patch.object(sweep, "play_level", side_effect=ValueError("bug in the test")):
            r = sweep.run_level("h001", game, self.args, self.out, self.pool, False)
        self.assertEqual(r["outcome"], "failed: error", r)
        self.assertIn("ValueError", r["detail"])

    # -- settings
    def test_fps60_on_by_default_and_off_with_the_flag(self):
        game = self.make_game()
        text = (game.play / "UserData" / "Config.toml").read_text()
        self.assertIn("fps60 = true", text)
        self.assertNotIn("frame_interpolation_fps", text)
        self.assertIn("rumble = false", text)
        self.assertIn("muted = true", text)
        self.args.fps30 = True
        game2 = self.make_game(1)
        self.assertIn("fps60 = false", (game2.play / "UserData" / "Config.toml").read_text())

    def test_the_game_logs_fps_and_is_isolated_from_real_pads(self):
        game = self.make_game()
        self.assertEqual(game.extra_env["NSMBW_LOG_FPS"], "1")
        env = launch.test_env(extra=game.extra_env)
        self.assertEqual(env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"], launch.IGNORE_PADS_EXCEPT)
        self.assertEqual(env["NSMBW_INPUT_SCRIPT"], launch.DUMMY_PAD_SCRIPT)
        self.assertEqual(env["NSMBW_LOG_FPS"], "1")

    # -- many levels
    def test_sweep_runs_all_levels_in_order_on_the_workers(self):
        self.configure(FAKE_MODE="normal")
        levels = ["h001", "i004", "g001", "e001", "c001"]
        results = sweep.sweep(levels, self.args, self.out, self.pool)
        self.assertEqual([r["level"] for r in results], levels)
        self.assertTrue(all(r["outcome"] == "passed" for r in results), [(r["level"], r["outcome"]) for r in results])
        self.assertEqual(self.pool.status()["live"], {})
        self.assertEqual(sorted(p.name for p in (self.out / "work").iterdir()), ["w0", "w1"])  # two Play copies, reused

    def test_a_failed_level_does_not_stop_the_others(self):
        self.configure(FAKE_MODE="normal")
        real = sweep.play_level

        def play(rec, game, level, args, attempt):
            if level == "i004":
                raise launch.GameCrashed("w0: the game exited with code 9")
            return real(rec, game, level, args, attempt)
        with mock.patch.object(sweep, "play_level", play):
            results = sweep.sweep(["h001", "i004", "g001"], self.args, self.out, self.pool)
        self.assertEqual([r["outcome"] for r in results], ["passed", "failed: crash (exited)", "passed"])

    def test_levels_share_the_slots(self):
        """Six levels, two slots: never more than two games at once, however many workers."""
        pool = slots.SlotPool(slots=2, state=Path(self.tmp.name) / "kit2", poll=0.05)
        self.args.workers = 4
        running, peak, lock = [0], [0], threading.Lock()

        def play(rec, game, level, args, attempt):
            with lock:
                running[0] += 1
                peak[0] = max(peak[0], running[0])
            time.sleep(0.4)
            with lock:
                running[0] -= 1
        self.configure()
        with mock.patch.object(sweep, "play_level", play):
            results = sweep.sweep(list("abcdef"), self.args, self.out, pool)
        self.assertEqual(len(results), 6)
        self.assertEqual(peak[0], 2)

    def test_stop_event_ends_the_sweep_early(self):
        self.configure()
        stop = threading.Event()
        stop.set()
        self.assertEqual(sweep.sweep(["h001", "i004"], self.args, self.out, self.pool, stop=stop), [])

    def test_missing_play_copy_is_a_failed_level_not_a_crash_of_the_sweep(self):
        self.configure()
        self.args.game_kwargs["src_play"] = Path(self.tmp.name) / "nowhere"
        self.args.game_kwargs.pop("game_cmd")
        results = sweep.sweep(["h001"], self.args, self.out, self.pool)
        self.assertEqual(results[0]["outcome"], "failed: error")
        self.assertIn("build it first", results[0]["detail"])

    # -- report
    def test_report_files(self):
        ok = self.one("h001", FAKE_MODE="normal")
        bad = self.one("i004", FAKE_MODE="crash")
        sweep.write_report(self.out, [ok, bad], self.args, "build ID test", 42.0, planned=3)
        data = json.loads((self.out / "results.json").read_text())
        self.assertEqual([d["level"] for d in data], ["h001", "i004"])
        report = (self.out / "REPORT.md").read_text()
        self.assertIn("build ID test", report)
        self.assertIn("2 of 3 level(s)", report)
        self.assertIn("1 passed, 1 failed", report)
        self.assertIn("## Problems", report)
        self.assertIn("| i004 | failed: crash (exited) |", report)
        self.assertIn("| h001 | passed |", report)
        self.assertIn("fps60 on, 60 fps", report)
        self.args.fps30 = True
        sweep.write_report(self.out, [ok], self.args, "b", 1.0)
        self.assertIn("fps60 off, 30 fps", (self.out / "REPORT.md").read_text())


class LevelListTest(unittest.TestCase):
    def test_all_levels_skips_menus_and_movies(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for folder, names in (("H0", ["H001.HIP", "H002.HIP"]), ("MN", ["mnu3.HIP"]), ("fmv", ["x.HIP"]), ("B0", ["b001.HIP"])):
                (d / folder).mkdir()
                for n in names:
                    (d / folder / n).write_bytes(b"")
            (d / "sd2.ini").write_text("x")
            self.assertEqual(sweep.all_levels(d), ["b001", "h001", "h002"])

    def test_parse_levels(self):
        self.assertEqual(sweep.parse_levels("h001, I004", ["h001", "i004"]), (["h001", "i004"], []))
        self.assertEqual(sweep.parse_levels("h001,zzz,h9", ["h001"])[1], ["zzz", "h9"])


if __name__ == "__main__":
    unittest.main()
