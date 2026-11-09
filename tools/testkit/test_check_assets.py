"""Tests of tools/check-assets.py, in throwaway git repositories (no game): python -m unittest discover -s tools/testkit -p "test_*.py" """
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "check-assets.py"
spec = importlib.util.spec_from_file_location("check_assets", SCRIPT)
check_assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_assets)


def run(cwd, *args):
    subprocess.run(["git"] + list(args), cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


class CheckAssetsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        run(self.dir, "init", "-q")
        run(self.dir, "config", "user.name", "t")
        run(self.dir, "config", "user.email", "t@example.invalid")
        run(self.dir, "config", "commit.gpgsign", "false")
        self.write("README.md", b"hello\n")
        self.commit("base")
        run(self.dir, "branch", "-M", "base")
        run(self.dir, "checkout", "-q", "-b", "change")
        self.previous = os.getcwd()
        os.chdir(self.dir)

    def tearDown(self):
        os.chdir(self.previous)
        self.tmp.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)

    def commit(self, message):
        run(self.dir, "add", "-A", "-f")
        run(self.dir, "commit", "-q", "-m", message)

    def problems(self):
        return check_assets.check("base", "HEAD", 8 * 1048576, staged=False)[1]

    def test_ordinary_change_is_clean(self):
        self.write("src/main.cpp", b"int main() {}\n")
        self.write("docs/launcher.png", b"\x89PNG\r\n" + b"x" * 5000)
        self.commit("code")
        self.assertEqual(self.problems(), [])

    def test_game_file_extension_is_refused(self):
        self.write("tools/x/level.HIP", b"not really")
        self.commit("hip")
        self.assertEqual(len(self.problems()), 1)
        self.assertIn(".hip", self.problems()[0])

    def test_renamed_game_file_is_caught_by_its_header(self):
        self.write("notes.txt", b"HIPA\0\0\0\0PACK" + b"\0" * 40)
        self.commit("disguised")
        self.assertIn("game archive", self.problems()[0])

    def test_disc_image_header_is_caught(self):
        header = bytearray(64)
        header[0x1C:0x20] = bytes.fromhex("C2339F3D")
        self.write("data.bin", bytes(header))
        self.commit("disc")
        self.assertIn("GameCube disc image", self.problems()[0])

    def test_big_file_is_refused(self):
        self.write("blob.dat", b"a" * (9 * 1048576))
        self.commit("big")
        self.assertIn("MiB is over", self.problems()[0])

    def test_data_folders_are_refused(self):
        self.write("Disc/DATA/readme.txt", b"x")
        self.write("wiicompiled/generated/f.cpp", b"x")
        self.write("Play/FrightWorks Game.exe", b"MZ")
        self.write("Play/portable.txt", b"")
        self.commit("folders")
        found = " ".join(self.problems())
        self.assertIn("Disc/DATA/readme.txt", found)
        self.assertIn("wiicompiled/generated/f.cpp", found)
        self.assertIn("Play/FrightWorks Game.exe", found)
        self.assertNotIn("portable.txt", found)

    def test_archive_is_refused_even_when_renamed(self):
        self.write("pack.zip", b"PK\x03\x04" + b"0" * 100)
        self.write("pack.dat", b"PK\x03\x04" + b"0" * 100)
        self.commit("zip")
        self.assertEqual(len(self.problems()), 3)

    def test_hd_pack_additions_are_exempt_from_size_and_type(self):
        self.write("tools/hd-pack-additions/ScoobyHD/big.png", b"\x89PNG" + b"x" * (9 * 1048576))
        self.commit("pack")
        self.assertEqual(self.problems(), [])

    def test_hd_pack_additions_are_not_exempt_from_the_header_check(self):
        self.write("tools/hd-pack-additions/ScoobyHD/a.png", b"HIPA\0\0\0\0PACK" + b"\0" * 40)
        self.commit("pack")
        self.assertIn("game archive", self.problems()[0])

    def test_file_added_and_removed_in_the_range_is_still_found(self):
        self.write("temp.iso", b"x" * 100)
        self.commit("add")
        os.remove(os.path.join(self.dir, "temp.iso"))
        self.commit("remove")
        problems = self.problems()
        self.assertEqual(len(problems), 1)
        self.assertIn("earlier commit", problems[0])

    def test_files_already_in_base_are_ignored(self):
        run(self.dir, "checkout", "-q", "base")
        self.write("old.hip", b"x")
        self.commit("old")
        run(self.dir, "checkout", "-q", "-B", "change")
        self.write("new.txt", b"fine")
        self.commit("new")
        self.assertEqual(self.problems(), [])

    def test_staged_file_is_found_before_it_is_committed(self):
        self.write("level.hip", b"x")
        run(self.dir, "add", "-f", "level.hip")
        self.assertEqual(self.problems(), [])  # commits only: nothing yet
        staged = check_assets.check("base", "HEAD", 8 * 1048576, commits=False)[1]
        self.assertEqual(len(staged), 1)
        self.assertIn("(staged)", staged[0])

    def test_unstaged_and_untracked_files_are_not_looked_at(self):
        self.write("level.hip", b"x")
        self.assertEqual(check_assets.check("base", "HEAD", 8 * 1048576)[1], [])

    def test_link_to_a_data_folder_is_refused_by_its_bare_name(self):
        # git stores a symlink as a file named like the folder (a worktree's Disc link)
        self.write("Disc", b"D:/somewhere")
        self.write("tools/debug/game/levels.md", b"ours")
        self.commit("link")
        found = self.problems()
        self.assertEqual(len(found), 1)
        self.assertIn("Disc:", found[0])

    def test_local_files_logs_and_saves_are_refused(self):
        self.write("AGENTS.md", b"mine")
        self.write("run.log", b"x")
        self.write("docs/AGENTS.md", b"fine, not the root one")
        self.write("slot.gci", b"x")
        self.commit("local")
        found = " ".join(self.problems())
        for name in ("AGENTS.md:", "run.log", "slot.gci"):
            self.assertIn(name, found)
        self.assertNotIn("docs/AGENTS.md", found)

    def test_windows_spellings_do_not_slip_through(self):
        self.assertTrue(check_assets.judge("DISC\DATA\a.txt", "", 1, b"", 100))
        self.assertTrue(check_assets.judge("x/Level.HIP.", "", 1, b"", 100))

    def test_change_on_a_moved_main_does_not_blame_us_for_mains_files(self):
        run(self.dir, "checkout", "-q", "base")
        self.write("new.hip", b"x")  # main moved on (a maintainer's change)
        self.commit("main moved")
        run(self.dir, "checkout", "-q", "change")
        self.write("fine.txt", b"ok")
        self.commit("ours")
        run(self.dir, "branch", "-f", "main2", "base")
        self.assertEqual(self.problems(), [])

    def test_command_line_exit_codes(self):
        def cli(*args):
            return subprocess.run([sys.executable, str(SCRIPT)] + list(args), cwd=self.dir, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, universal_newlines=True)
        self.write("fine.txt", b"ok")
        self.commit("fine")
        self.assertEqual(cli("--base", "base").returncode, 0)
        self.write("bad.iso", b"x")
        self.commit("bad")
        result = cli("--base", "base")
        self.assertEqual(result.returncode, 1)
        self.assertIn("bad.iso", result.stdout)
        self.assertEqual(cli("--base", "nosuchbranch").returncode, 2)

    def test_many_files_and_a_big_one_are_read_without_stalling(self):
        for number in range(300):
            self.write("src/f%d.txt" % number, b"text %d" % number)
        self.write("big.dat", b"a" * (20 * 1048576))
        self.commit("many")
        found = self.problems()
        self.assertEqual(len(found), 1)
        self.assertIn("big.dat", found[0])


if __name__ == "__main__":
    unittest.main()
