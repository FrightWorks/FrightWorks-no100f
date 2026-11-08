"""The one launch helper for automated test runs (Python 3.8+, Windows and Linux, standard library only).

    game = TestGame("h001", work_dir, boot="h001", hd=True)
    game.prepare()      # an own copy of Play\\ in work_dir, its own Config.toml and boot mod
    game.start()
    ...                 # game.devmenu, game.check_crash(), game.screenshot(png), game.wait_console("h001.HIP", 120)
    game.stop()

Every run gets, with nothing left to the caller:
  * its own copy of the freshly built Play\\ (exe, DLLs, data; the HD pack and the button icons are linked, never copied)
    in a folder of the caller's, so the player's Play\\, Config.toml and saves are never touched and need no putting back;
  * a Config.toml of its own: windowed, rumble off (`[controller] rumble = false`), sound muted;
  * the environment of the game process scrubbed of every test switch the caller's shell may have left behind
    (NO100F_*, AX_*, NSMBW_*, AUDIO_*, AURORA_*, SPM_*, BUTTON_ICONS) and of SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
    which must never be set (SDL then refuses the game's own hint call, and the game treats that as an error);
  * controller isolation: NO100F_INPUT_SCRIPT set (the game then reports pad 1 connected every frame) and
    SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT set to a device ID that does not exist, so no real controller reaches the
    game. The process gets both; the player's own game is unaffected;
  * a build-ID check: the exe must have been built from the code under test: the same commit, or an older one with only
    test and doc files changed since (check_build / expected_build_id).

`game_cmd` replaces the exe with another command (the tests use a windowless stand-in script).
"""
import os
import re
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
EXE_NAMES = ("FrightWorks Game.exe", "FrightWorks Game")
IGNORE_PADS_EXCEPT = "0xDEAD/0xBEEF"   # no controller has this vendor/product ID
DUMMY_PAD_SCRIPT = "none"              # any non-empty text without events: pad 1 reports connected, nothing is pressed
SCRUB_PREFIXES = ("NO100F_", "AX_", "NSMBW_", "AUDIO_", "AURORA_", "SPM_")
SCRUB_NAMES = ("BUTTON_ICONS", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "SDL_GAMECONTROLLER_IGNORE_DEVICES",
               "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "SDL_WINDOW_ACTIVATE_WHEN_SHOWN")
CRASH_PATTERN = re.compile(r"AccessViolation|OS panic|missing_target|Unhandled|FATAL|Fatal Error")
BUILD_ID_PATTERN = re.compile(r"\d+\.\d+\.\d+[^\s\x00]* [\w./-]+@[0-9a-f]{7}(?:\+[0-9a-f]{4})?")  # NULs sit between strings in an exe


class GameCrashed(Exception):
    """The game exited by itself, or logged a fault. Fails a run at once; never retried."""


class BuildMismatch(Exception):
    """The game under test was not built from the commit under test."""


# ---- links and deleting ---------------------------------------------------------------------------------------------
def is_link(path) -> bool:
    """A symlink, or on Windows a junction (a reparse point)."""
    if os.path.islink(path):
        return True
    if os.name == "nt":
        try:
            return bool(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
        except OSError:
            return False
    return False


def make_dir_link(link, target) -> None:
    """A directory junction (Windows, no admin needed) or symlink (elsewhere) at `link` pointing at `target`."""
    link, target = Path(link), Path(target)
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True, text=True)
        if r.returncode:
            raise OSError(f"mklink /J {link} -> {target} failed: {r.stdout}{r.stderr}")
    else:
        os.symlink(str(target), str(link), target_is_directory=True)


def remove_link(path) -> None:
    """Remove a link without touching what it points at."""
    if os.path.isdir(path) and not os.path.islink(path):
        os.rmdir(path)  # a junction: rmdir removes the junction only
    else:
        os.unlink(path)


def safe_rmtree(path) -> None:
    """Delete a folder tree, taking out every link in it first so nothing is ever deleted through one."""
    path = Path(path)
    if not os.path.lexists(path):
        return
    if is_link(path):
        remove_link(path)
        return
    for dirpath, dirs, _files in os.walk(path, followlinks=False):
        for d in list(dirs):
            p = os.path.join(dirpath, d)
            if is_link(p):
                remove_link(p)
                dirs.remove(d)

    def onerror(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=onerror)


# ---- build ID -------------------------------------------------------------------------------------------------------
class BuildId:
    """What runtime/cmake/build_info.cmake would write for this checkout right now."""

    def __init__(self, version, branch, commit, changes):
        self.version, self.branch, self.commit, self.changes = version, branch, commit, changes

    @property
    def base(self):
        return f"{self.version} {self.branch}@{self.commit}"

    @property
    def full(self):
        return self.base + (f"+{self.changes}" if self.changes else "")

    def __str__(self):
        return self.full


def _git(repo, *args):
    r = subprocess.run(["git", *args], cwd=str(repo), capture_output=True, text=True)
    return r.stdout.rstrip() if r.returncode == 0 else ""


def expected_build_id(repo=REPO) -> BuildId:
    """Mirrors build_info.cmake: the csproj <Version>, branch, 7-digit commit, and 4 hex digits of the SHA-1 of
    `git diff HEAD` when tracked files differ (trailing whitespace of the diff is stripped, as cmake does)."""
    version = "0.0.0"
    m = re.search(r"<Version>([^<]+)</Version>",
                  (Path(repo) / "launcher" / "FrightWorks.Launcher.csproj").read_text(encoding="utf-8"))
    if m:
        version = m.group(1)
    branch = _git(repo, "rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        branch = "detached"
    commit = _git(repo, "rev-parse", "--short=7", "HEAD")
    changes = ""
    if _git(repo, "status", "--porcelain", "--untracked-files=no"):
        import hashlib
        changes = hashlib.sha1(_git(repo, "diff", "HEAD").encode("utf-8")).hexdigest()[:4]
    return BuildId(version, branch, commit, changes)


def find_build_id(text) -> str:
    """The build ID in a block of text (the exe's bytes decoded as latin-1, or console.log), or ''."""
    m = BUILD_ID_PATTERN.search(text)
    return m.group() if m else ""


# Paths that are not inputs of the game build: a change to them since the build does not make the build stale.
TEST_ONLY_PREFIXES = ("tools/testkit/", "docs/", "autotest/")
TEST_ONLY_FILES = ("tools/smoke-test.py",)


def is_test_only(path: str) -> bool:
    return path.startswith(TEST_ONLY_PREFIXES) or path in TEST_ONLY_FILES or path.lower().endswith(".md")


def check_build(found, expected: BuildId, repo=None):
    """(ok, message): was the game built from the code under test?
    Identical ID: yes. Otherwise, for a clean build of the same branch, `git diff --name-only <built commit>` (that commit
    against the working tree, committed and uncommitted tracked changes) must name only files outside the build's
    inputs (tools/testkit/, tools/smoke-test.py, docs/, *.md, autotest/): then the run is accepted and says which files
    differ. Anything else, a build made from uncommitted changes that are gone or different now included, fails:
    rebuild. Without `repo` only the exact ID passes."""
    if not found:
        return False, f"no build ID found (expected {expected.base})"
    if found == expected.full:
        return True, f"build ID {found}"
    built, _, built_changes = found.partition("+")
    parts = built.rsplit("@", 1)
    head = f"HEAD {expected.commit}" + (f" + uncommitted ({expected.changes})" if expected.changes else "")
    stale = f"build ID {found} is not the code under test ({expected.full}): rebuild"
    if repo is None or len(parts) != 2 or built_changes:
        return False, stale
    built_branch = parts[0].split(" ", 1)[1] if " " in parts[0] else ""
    if built_branch != expected.branch:
        return False, stale
    if not _git(repo, "rev-parse", "--verify", "--quiet", parts[1] + "^{commit}"):
        return False, f"build commit {parts[1]} is not in this repository: rebuild"
    names = [n for n in _git(repo, "diff", "--name-only", parts[1]).splitlines() if n]
    game = [n for n in names if not is_test_only(n)]
    if game:
        return False, f"{stale}; changed since the build: {', '.join(game[:6])}{' ...' if len(game) > 6 else ''}"
    shown = ", ".join(names[:4]) + (f" and {len(names) - 4} more" if len(names) > 4 else "")
    return True, f"build {parts[1]}, {head}, only test/doc files differ: {shown or 'none'}"


def exe_build_id(exe) -> str:
    return find_build_id(Path(exe).read_bytes().decode("latin-1"))


# ---- config ---------------------------------------------------------------------------------------------------------
def test_config(dvd_root, hd=False, window=(1280, 720), extra="") -> str:
    """The Config.toml of a test run: windowed, MSAA off, 60 fps, HD pack per `hd`, muted at volume 0.05, rumble off.
    `extra` is appended as is: it must not repeat any table above."""
    dvd = str(dvd_root).replace("\\", "/")
    return (
        f"[paths]\ndvd_root = '{dvd}'\n\n"
        f"[video]\ndisplay_mode = \"windowed\"\nwindow_width = {window[0]}\nwindow_height = {window[1]}\n"
        f"resolution_multiplier = 0\naspect_mode = 1\nskip_intro = true\nfps60 = true\nmsaa = 1\n"
        f"texture_replacements = {'true' if hd else 'false'}\n\n"
        "[audio]\nvolume = 0.05\nmuted = true\n\n"
        "[controller]\nrumble = false\n" + (("\n" + extra) if extra else ""))


def test_env(base=None, extra=None, dev_tools=True) -> dict:
    """The environment of a test game: `base` (default os.environ) without any test switch, plus the isolation."""
    env = {k: v for k, v in (os.environ if base is None else base).items()
           if not k.startswith(SCRUB_PREFIXES) and k.upper() not in SCRUB_NAMES}
    if dev_tools:
        env["NO100F_DEV_TOOLS"] = "1"   # the F8 dev menu and its devmenu.txt
    env["NO100F_NO_POPUP"] = "1"        # a crash is logged, not shown in a window
    env["NO100F_INPUT_SCRIPT"] = DUMMY_PAD_SCRIPT
    env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"] = IGNORE_PADS_EXCEPT
    env.update(extra or {})
    env.pop("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", None)  # whatever `extra` says
    return env


def boot_ini(disc_data, level) -> str:
    ini = (Path(disc_data) / "files" / "sd2.ini").read_text(encoding="latin-1")
    ini = re.sub(r"(?m)^BOOT=.*$", f"BOOT={level}", ini)
    return re.sub(r"(?m)^ShowMenuOnBoot=1", "ShowMenuOnBoot=0", ini)


# ---- the game -------------------------------------------------------------------------------------------------------
class TestGame:
    def __init__(self, name, work_dir, boot=None, hd=False, repo=REPO, src_play=None, disc_data=None, game_cmd=None,
                 extra_env=None, config_extra="", window=(1280, 720), activate_when_shown=None, dev_tools=True,
                 build_check=True):
        self.name, self.repo = name, Path(repo)
        self.play = Path(work_dir) / "Play"
        self.src_play = Path(src_play) if src_play else self.repo / "Play"
        self.disc_data = Path(disc_data) if disc_data else self.repo / "Disc" / "DATA"
        self.boot, self.hd, self.window = boot, hd, window
        self.game_cmd, self.extra_env, self.config_extra = game_cmd, dict(extra_env or {}), config_extra
        self.activate_when_shown, self.dev_tools, self.build_check = activate_when_shown, dev_tools, build_check
        self.proc = None
        self.launches = 0
        self.start_time = 0.0
        self._log_dirs_before = set()
        self._err_scanned = 0
        self.devmenu = None  # set by start(); see devmenu.DevMenu

    # -- paths
    @property
    def err_log(self) -> Path:
        return self.play.parent / f"{self.name}-stderr-{max(self.launches, 1)}.log"

    def console_log(self):
        """The console.log of the newest launch, or None."""
        logs = self.play / "UserData" / "Logs"
        if not logs.is_dir():
            return None
        dirs = [d for d in logs.iterdir() if d.is_dir() and d.name not in self._log_dirs_before]
        dirs.sort(key=lambda d: d.stat().st_mtime, reverse=True)
        for d in dirs:
            if (d / "console.log").exists():
                return d / "console.log"
        return None

    def console_text(self) -> str:
        p = self.console_log()
        try:
            return p.read_text(encoding="utf-8", errors="replace") if p else ""
        except OSError:
            return ""

    # -- preparing
    def exe_path(self) -> Path:
        for n in EXE_NAMES:
            if (self.play / n).exists():
                return self.play / n
        return self.play / EXE_NAMES[0]

    def prepare(self):
        """Make the Play copy (first time) and write Config.toml and the boot mod for the current settings."""
        self.play.mkdir(parents=True, exist_ok=True)
        if not self.game_cmd:
            if not any((self.src_play / n).exists() for n in EXE_NAMES):
                raise FileNotFoundError(f"no game in {self.src_play}: build it first (tools\\build-no100f.ps1)")
        if self.src_play.is_dir():
            for f in self.src_play.iterdir():
                if f.is_file() and not (self.play / f.name).exists():
                    shutil.copy2(str(f), str(self.play / f.name))
            for sub in ("button_icons",) + (("texture_replacements",) if self.hd else ()):
                src, dst = self.src_play / sub, self.play / sub
                if src.is_dir() and not os.path.lexists(dst):
                    make_dir_link(dst, src)
        if not (self.play / "portable.txt").exists():
            (self.play / "portable.txt").write_text("portable\n", encoding="utf-8")
        self.set_boot(self.boot)

    def set_boot(self, level):
        """Boot straight into `level` (a mod's sd2.ini), or the game's own title menu when None. Saves are kept."""
        self.boot = level
        ud = self.play / "UserData"
        ud.mkdir(parents=True, exist_ok=True)
        (ud / "Config.toml").write_text(
            test_config(self.disc_data, self.hd, self.window, self.config_extra), encoding="utf-8")
        mod = ud / "mods" / "zz-smoke"
        if mod.exists():
            safe_rmtree(mod)
        if level:
            (mod / "files").mkdir(parents=True)
            (mod / "files" / "sd2.ini").write_text(boot_ini(self.disc_data, level), encoding="latin-1")

    def check_build(self):
        """Raises BuildMismatch unless the exe is from the commit under test; returns the message (maybe a warning)."""
        if self.game_cmd:
            return "stand-in game: build ID not checked"
        if not self.build_check:
            return "build ID check SKIPPED (manual override)"
        expected = expected_build_id(self.repo)
        exe = self.exe_path()
        if not exe.exists():  # before prepare(): the build the copy will be made from
            exe = next((self.src_play / n for n in EXE_NAMES if (self.src_play / n).exists()), exe)
        ok, msg = check_build(exe_build_id(exe), expected, self.repo)
        if not ok:
            raise BuildMismatch(msg)
        return msg

    def check_console_build(self):
        """After the game has started: the build ID in its own console.log (None while it has not printed it yet)."""
        found = find_build_id(self.console_text())
        if not found:
            return None
        if not self.build_check:
            return f"build ID {found} (check skipped)"
        ok, msg = check_build(found, expected_build_id(self.repo), self.repo)
        if not ok:
            raise BuildMismatch(msg)
        return msg

    # -- running
    def start(self):
        from .devmenu import DevMenu
        if self.proc and self.proc.poll() is None:
            raise RuntimeError(f"{self.name}: already running")
        logs = self.play / "UserData" / "Logs"
        self._log_dirs_before = {d.name for d in logs.iterdir()} if logs.is_dir() else set()
        for f in ("devmenu.txt", "devmenu.log", "devmenu_press.txt"):
            if (self.play / f).exists():
                (self.play / f).unlink()
        extra = dict(self.extra_env)
        if self.activate_when_shown is not None:
            extra["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] = "1" if self.activate_when_shown else "0"
        env = test_env(extra=extra, dev_tools=self.dev_tools)
        cmd = list(self.game_cmd) if self.game_cmd else [str(self.exe_path())]
        self.launches += 1
        self._err_scanned = 0
        with open(self.err_log, "w", encoding="utf-8", errors="replace") as err:
            self.proc = subprocess.Popen(cmd, cwd=str(self.play), env=env, stdout=subprocess.DEVNULL, stderr=err)
        self.start_time = time.time()
        self.devmenu = DevMenu(self.play, crash_check=self.check_crash)
        return self

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def check_crash(self):
        """Raises GameCrashed if the game exited, or wrote a fault into its stderr since the last call."""
        if self.proc is None:
            raise GameCrashed(f"{self.name}: not started")
        try:
            data = self.err_log.read_bytes()
        except OSError:
            data = b""
        new = data[self._err_scanned:].decode("utf-8", errors="replace")
        self._err_scanned = len(data)
        m = CRASH_PATTERN.search(new)
        if m:
            line = next((l for l in new.splitlines() if m.group() in l), m.group())
            raise GameCrashed(f"{self.name}: {line.strip()}")
        code = self.proc.poll()
        if code is not None:
            raise GameCrashed(f"{self.name}: the game exited with code {code}")

    def wait_console(self, text, timeout, poll=0.5):
        """Wait until console.log has `text`. False on timeout; GameCrashed if the game dies first."""
        end = time.time() + timeout
        while time.time() < end:
            self.check_crash()
            if text in self.console_text():
                return True
            time.sleep(poll)
        return False

    def screenshot(self, png) -> bool:
        """Save a picture of the game window (Windows only for now). False when it cannot."""
        if self.proc is None or os.name != "nt":
            return False
        Path(png).parent.mkdir(parents=True, exist_ok=True)
        script = self.repo / "tools" / "shot-pid.ps1"
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                        "-ProcessId", str(self.proc.pid), "-Out", str(png)], capture_output=True)
        return Path(png).exists()

    def stop(self):
        """End this game process (only the one this object started)."""
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def cleanup(self):
        """Stop the game and delete the Play copy (links are removed first, never followed)."""
        self.stop()
        safe_rmtree(self.play)
