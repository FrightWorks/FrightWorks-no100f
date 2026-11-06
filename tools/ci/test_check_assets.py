"""Tests for check_assets.py, in throwaway git repositories. Run: python -m unittest discover -s tools/ci -p "test_*.py"."""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_assets  # noqa: E402


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
        return check_assets.check("base", "HEAD", 8 * 1048576)[1]

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
        self.assertEqual(check_assets.check("base", "HEAD", 8 * 1048576)[1], [])


if __name__ == "__main__":
    unittest.main()
