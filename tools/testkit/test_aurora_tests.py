"""Tests of tools/aurora-tests.py with no build: python -m unittest discover -s tools/testkit -p "test_*.py" """
import importlib.util
import io
import contextlib
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "aurora-tests.py"
spec = importlib.util.spec_from_file_location("aurora_tests", SCRIPT)
at = importlib.util.module_from_spec(spec)
spec.loader.exec_module(at)

NT, POSIX = "nt", "posix"

PASS_RUN = """\
Test project /b
    Start 1: gx_readback_tests
1/3 Test #1: gx_readback_tests ....................   Passed    4.10 sec
2/3 Test #2: gx_readback_tests_validate ...........   Passed    5.20 sec
3/3 Test #3: gx_readback_tests_no_float32_blend ...   Passed    5.00 sec
"""
NOT_BUILT = """\
1/2 Test #1: gx_fifo_tests ........................   Passed    0.10 sec
2/2 Test #2: os_alloc_tests_NOT_BUILT .............***Not Run   0.00 sec
"""
FAILED = """\
1/2 Test #1: gx_readback_tests ....................***Failed    1.00 sec
2/2 Test #2: gx_readback_tests_validate ...........***Timeout 300.00 sec
"""


def touch(folder, *names):
    folder.mkdir(parents=True, exist_ok=True)
    for n in names:
        (folder / n).write_bytes(b"x")


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "checkout"
        self.root.mkdir()


class FindPlayTest(Tmp):
    def full_play(self, folder):
        touch(folder, *at.WINDOWS_LIBS)
        return folder

    def test_default_is_checkout_play(self):
        play = self.full_play(self.root / "Play")
        self.assertEqual(at.find_play(self.root, system=NT), play)

    def test_play_without_sdl3_is_not_usable(self):
        touch(self.root / "Play", "FrightWorks Game.exe", "libc++.dll")
        with self.assertRaises(at.SetupError):
            at.find_play(self.root, system=NT)

    def test_missing_play_folder(self):
        with self.assertRaises(at.SetupError) as cm:
            at.find_play(self.root, system=NT)
        self.assertIn("--play", str(cm.exception))

    def test_argument_wins_and_must_exist(self):
        self.full_play(self.root / "Play")
        other = self.full_play(Path(self.tmp.name) / "elsewhere")
        self.assertEqual(at.find_play(self.root, other, system=NT), other)
        with self.assertRaises(at.SetupError):
            at.find_play(self.root, Path(self.tmp.name) / "nope", system=NT)

    def test_argument_without_sdl3_refused(self):
        bare = Path(self.tmp.name) / "bare"
        touch(bare, "readme.txt")
        with self.assertRaises(at.SetupError) as cm:
            at.find_play(self.root, bare, system=NT)
        self.assertIn("SDL3.dll", str(cm.exception))

    def test_linux_versioned_soname(self):
        touch(self.root / "Play", "libSDL3.so.0.4.4")
        self.assertEqual(at.find_play(self.root, system=POSIX), self.root / "Play")


class LibraryCheckTest(Tmp):
    def test_everything_present(self):
        play = self.root / "Play"
        touch(play, *at.WINDOWS_LIBS)
        self.assertEqual(at.check_libraries(play, self.root, NT), [])

    def test_toolchain_supplies_runtime_libs(self):
        play = self.root / "Play"
        touch(play, "SDL3.dll", "webgpu_dawn.dll")
        touch(self.root / "toolchain" / "llvm-mingw" / "bin", "libc++.dll", "libunwind.dll")
        self.assertEqual(at.check_libraries(play, self.root, NT), [])

    def test_missing_ones_named(self):
        play = self.root / "Play"
        touch(play, "webgpu_dawn.dll")
        self.assertEqual(at.check_libraries(play, self.root, NT), ["SDL3.dll", "libc++.dll", "libunwind.dll"])

    def test_linux_check(self):
        play = self.root / "Play"
        play.mkdir()
        self.assertEqual(at.check_libraries(play, self.root, POSIX), ["libSDL3.so*"])
        touch(play, "libSDL3.so.0")
        self.assertEqual(at.check_libraries(play, self.root, POSIX), [])


class CommandTest(Tmp):
    def test_configure_windows(self):
        cmd = at.configure_cmd("cmake", self.root, "B", NT)
        self.assertEqual(cmd[:6], ["cmake", "-S", str(self.root / "wiicompiled" / "aurora-main"), "-B", "B", "-G"])
        self.assertIn("-DAURORA_BUILD_GPU_TESTS=ON", cmd)
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", cmd)
        self.assertNotIn("-DCMAKE_CXX_COMPILER=clang++", cmd)
        self.assertNotIn("tests", Path(cmd[2]).parts)

    def test_configure_linux_names_compilers(self):
        cmd = at.configure_cmd("cmake", self.root, "B", POSIX)
        self.assertIn("-DCMAKE_CXX_COMPILER=clang++", cmd)

    def test_build_has_both_targets(self):
        cmd = at.build_cmd("cmake", "B")
        self.assertEqual(cmd[-3:], ["--target", "gx_fifo_tests", "gx_readback_tests"])
        self.assertIn("--parallel", cmd)
        self.assertEqual(at.build_cmd("cmake", "B", 4)[3:5], ["--parallel", "4"])

    def test_ctest_default_runs_all_filter_adds_r(self):
        cmd = at.ctest_cmd("ctest", "B", "")
        self.assertEqual(cmd, ["ctest", "--test-dir", "B", "--output-on-failure", "--timeout", "300"])
        self.assertEqual(at.ctest_cmd("ctest", "B", "gx_readback")[-2:], ["-R", "gx_readback"])

    def test_find_tool_prefers_toolchain(self):
        touch(self.root / "toolchain" / "CMake" / "bin", "cmake.exe")
        self.assertEqual(at.find_tool("cmake", self.root, NT, which=lambda n: "C:/other/cmake"),
                         str(self.root / "toolchain" / "CMake" / "bin" / "cmake.exe"))
        self.assertEqual(at.find_tool("ctest", self.root, NT, which=lambda n: "C:/other/ctest"), "C:/other/ctest")


class EnvTest(Tmp):
    def test_windows_path_first_and_original_untouched(self):
        touch(self.root / "toolchain" / "CMake" / "bin", "cmake.exe")
        base = {"Path": "C:\\Windows", "X": "1"}
        env = at.run_env(base, "P", self.root, NT)
        parts = env["Path"].split(";") if ";" in env["Path"] else env["Path"].split(":")
        self.assertEqual(parts[0], "P")
        self.assertTrue(env["Path"].endswith("C:\\Windows"))
        self.assertEqual(base, {"Path": "C:\\Windows", "X": "1"})
        self.assertEqual(env["X"], "1")

    def test_linux_library_path(self):
        env = at.run_env({"LD_LIBRARY_PATH": "/old", "PATH": "/bin"}, "/p", self.root, POSIX)
        self.assertEqual(env["LD_LIBRARY_PATH"], "/p" + at.os.pathsep + "/old")
        self.assertEqual(env["PATH"], "/bin")
        self.assertEqual(at.run_env({}, "/p", self.root, POSIX)["LD_LIBRARY_PATH"], "/p")


class ResultsTest(unittest.TestCase):
    def test_all_passed(self):
        r = at.parse_ctest(PASS_RUN)
        self.assertEqual(len(r["passed"]), 3)
        self.assertEqual(at.verdict(r, 0), (True, "ok"))

    def test_not_built_is_not_run_and_not_a_failure(self):
        r = at.parse_ctest(NOT_BUILT)
        self.assertEqual(r["passed"], ["gx_fifo_tests"])
        self.assertEqual(r["not_run"], ["os_alloc_tests_NOT_BUILT"])
        self.assertEqual(r["failed"], [])
        self.assertTrue(at.verdict(r, 8)[0])   # ctest itself exits nonzero for "Not Run"
        text = at.summary(r, True, "ok", "P", "B")
        self.assertIn("not run: os_alloc_tests_NOT_BUILT", text)
        self.assertIn("not run 1", text)

    def test_failures_and_timeouts(self):
        r = at.parse_ctest(FAILED)
        self.assertEqual([n for n, _ in r["failed"]], ["gx_readback_tests", "gx_readback_tests_validate"])
        self.assertFalse(at.verdict(r, 8)[0])
        self.assertIn("FAILED:  gx_readback_tests_validate (Timeout)", at.summary(r, False, "2 failed", "P", "B"))

    def test_other_not_run_is_failure(self):
        r = at.parse_ctest("1/1 Test #1: gx_readback_tests ...***Not Run   0.00 sec\n")
        self.assertEqual(r["failed"], [("gx_readback_tests", "Not Run")])

    def test_nothing_ran_fails(self):
        r = at.parse_ctest("No tests were found!!!\n")
        self.assertEqual(at.verdict(r, 0), (False, "no test ran (does the filter match anything?)"))

    def test_nonzero_exit_with_clean_parse_fails(self):
        self.assertFalse(at.verdict(at.parse_ctest(PASS_RUN), 1)[0])


class MainTest(Tmp):
    """main() with fake cmake/ctest: nothing is built or run."""

    def setUp(self):
        super().setUp()
        self.play = self.root / "Play"
        touch(self.play, *at.WINDOWS_LIBS)
        self.build = self.root / "b"
        self.calls = []
        self.old_root = at.ROOT
        at.ROOT = self.root
        self.addCleanup(setattr, at, "ROOT", self.old_root)

    def go(self, argv, ctest_out=PASS_RUN, ctest_rc=0, step_rc=0):
        class Done:
            returncode = step_rc

        def run(cmd, env=None, cwd=None):
            self.calls.append(("run", cmd, env))
            return Done()

        class P:
            returncode = ctest_rc

            def __init__(s, cmd, env=None, **kw):
                self.calls.append(("ctest", cmd, env))
                s.stdout = io.StringIO(ctest_out)

            def wait(s):
                pass

        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = at.main(["--build-dir", str(self.build)] + argv, base_env={"Path": "C:\\x"}, run=run, popen=P,
                         which=lambda n: n)
        return rc, out.getvalue(), err.getvalue()

    @unittest.skipUnless(at.is_windows(), "uses the Windows library list")
    def test_fresh_folder_configures_builds_tests(self):
        rc, out, _ = self.go([])
        self.assertEqual(rc, 0)
        self.assertEqual([c[0] for c in self.calls], ["run", "run", "ctest"])
        self.assertIn("PASS", out)

    @unittest.skipUnless(at.is_windows(), "uses the Windows library list")
    def test_configured_folder_skips_configure(self):
        touch(self.build, "build.ninja")
        rc, out, _ = self.go([])
        self.assertEqual(rc, 0)
        self.assertEqual([c[0] for c in self.calls], ["run", "ctest"])
        self.assertIn("skipped", out)
        self.go(["--reconfigure"])
        self.assertEqual([c[0] for c in self.calls][2:], ["run", "run", "ctest"])

    @unittest.skipUnless(at.is_windows(), "uses the Windows library list")
    def test_missing_library_runs_nothing(self):
        (self.play / "libc++.dll").unlink()
        (self.play / "libunwind.dll").unlink()
        rc, _, err = self.go([])
        self.assertEqual(rc, 2)
        self.assertEqual(self.calls, [])
        self.assertIn("libc++.dll", err)

    @unittest.skipUnless(at.is_windows(), "uses the Windows library list")
    def test_failed_test_exits_nonzero(self):
        rc, out, _ = self.go(["--filter", "gx_readback"], ctest_out=FAILED, ctest_rc=8)
        self.assertEqual(rc, 1)
        self.assertIn("FAIL", out)
        self.assertEqual(self.calls[-1][1][-2:], ["-R", "gx_readback"])

    @unittest.skipUnless(at.is_windows(), "uses the Windows library list")
    def test_build_failure_stops(self):
        rc, _, err = self.go([], step_rc=1)
        self.assertEqual(rc, 1)
        self.assertEqual(len(self.calls), 1)
        self.assertIn("configure failed", err)


if __name__ == "__main__":
    unittest.main()
