"""Shared game-run slots for automated test runs (Windows and Linux, Python 3.8+, standard library only).

A fixed number of slots (3 unless told otherwise) lets that many automated game runs, from any checkout, share the machine.
`exclusive=True` is for the human check and performance checks: it waits for every slot to be given back, and while it
waits or holds, nobody else gets one. Builds never take a slot.

    pool = SlotPool(slots=3)
    with pool.acquire("smoke h001") as lease:
        ...run the game...
    with pool.acquire("fps measurement", exclusive=True):
        ...the machine to yourself...

A holder that dies leaves a slot file that nobody refreshes: it is taken over once its process is gone or its heartbeat is
older than STALE_SECONDS. All changes of the state folder happen under a short-lived mutex (an atomic mkdir).

State folder: %LOCALAPPDATA%\\FrightWorks-kit (the same folder as the old lock), on Linux $XDG_STATE_HOME/FrightWorks-kit or
~/.local/state/FrightWorks-kit; FRIGHTWORKS_KIT_DIR overrides it (tests do).

    python tools/testkit/slots.py status
"""
import atexit
import contextlib
import datetime
import json
import os
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

DEFAULT_SLOTS = 3
SLOTS_ENV = "FRIGHTWORKS_TEST_SLOTS"
HEARTBEAT_SECONDS = 15
STALE_SECONDS = 90          # a slot file nobody has refreshed for this long is abandoned
MUTEX_STALE_SECONDS = 10


def state_dir() -> Path:
    override = os.environ.get("FRIGHTWORKS_KIT_DIR")
    if override:
        return Path(override)
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
    else:
        base = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(base) / "FrightWorks-kit"


def slot_count(requested=None) -> int:
    """--slots value, else FRIGHTWORKS_TEST_SLOTS, else 3 (at least 1)."""
    for value in (requested, os.environ.get(SLOTS_ENV)):
        if value not in (None, ""):
            try:
                return max(1, int(value))
            except ValueError:
                raise SystemExit(f"slots: not a number: {value!r}")
    return DEFAULT_SLOTS


def pid_alive(pid) -> bool:
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.GetLastError() == 5  # access denied: it exists
        try:
            code = ctypes.c_ulong()
            return bool(kernel32.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(code))) and code.value == 259
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(pid, 0)  # POSIX only: signal 0 checks, it never delivers (on Windows it would kill, hence the branch above)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _read_json(path: Path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _create_json(path: Path, data: dict) -> bool:
    """Create the file only if it does not exist yet. False: somebody else has it."""
    try:
        fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return False
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f)
    return True


def _write_json(path: Path, data: dict) -> None:
    tmp = path.with_name(path.name + ".tmp%d" % os.getpid())
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(str(tmp), str(path))


def _same_path(a, b) -> bool:
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


def holders(state=None):
    """Live slot records ({slot, kind, pid, why, root, since, ...}), read without the mutex and without removing
    anything: for tools that only look (worktree.py list). A file being written this moment may be missed."""
    slot_dir = (Path(state) if state else state_dir()) / "slots"
    out = []
    for p in sorted(slot_dir.glob("slot-*.json")):
        data = _read_json(p)
        if data is not None and pid_alive(data.get("pid")) and _age(p) <= STALE_SECONDS:
            out.append(data)
    return out


def _age(path: Path) -> float:
    try:
        return time.time() - path.stat().st_mtime
    except OSError:
        return 1e9


def _now_iso() -> str:
    return datetime.datetime.now().astimezone().isoformat()


class Lease:
    """A held slot (or the whole machine, when exclusive). Use as a context manager or call release()."""

    def __init__(self, pool, index, exclusive, why):
        self.pool, self.index, self.exclusive, self.why = pool, index, exclusive, why
        self.waited = 0.0
        self._stop = threading.Event()
        self._released = False
        self._thread = threading.Thread(target=self._beat, name="slot-heartbeat", daemon=True)
        self._thread.start()
        atexit.register(self.release)

    def _beat(self):
        while not self._stop.wait(HEARTBEAT_SECONDS):
            try:
                self.pool._heartbeat(self)
            except OSError:
                pass

    def release(self):
        if self._released:
            return
        self._released = True
        self._stop.set()
        atexit.unregister(self.release)
        try:
            self.pool._release(self)
        except (OSError, TimeoutError):
            pass  # the state folder is gone or locked: nothing left to give back

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


class SlotPool:
    def __init__(self, slots=None, state=None, poll=1.0):
        self.slots = slot_count(slots)
        self.dir = Path(state) if state else state_dir()
        self.slot_dir = self.dir / "slots"
        self.poll = poll
        self.exclusive_path = self.dir / "exclusive.lock"
        self.slot_dir.mkdir(parents=True, exist_ok=True)

    # ---- mutex -------------------------------------------------------------------------------------------------
    @contextlib.contextmanager
    def _mutex(self, timeout=30.0):
        path = self.dir / "mutex.d"
        deadline = time.time() + timeout
        while True:
            try:
                os.mkdir(str(path))
                break
            except FileExistsError:
                if _age(path) > MUTEX_STALE_SECONDS:
                    with contextlib.suppress(OSError):
                        os.rmdir(str(path))
                    continue
                if time.time() > deadline:
                    raise TimeoutError(f"slots: could not take the state mutex {path}")
                time.sleep(0.05)
        try:
            yield
        finally:
            with contextlib.suppress(OSError):
                os.rmdir(str(path))

    # ---- state -------------------------------------------------------------------------------------------------
    def _holder_live(self, path: Path):
        """The parsed holder file if its holder still lives, else None (the file is left for the caller to remove)."""
        data = _read_json(path)
        if data is None:
            return {"unreadable": True} if _age(path) < 5 else None  # just created, not written yet
        if not pid_alive(data.get("pid")) or _age(path) > STALE_SECONDS:
            return None
        return data

    def _live_slots(self):
        """{index: data} of the slot files whose holders live; abandoned ones are removed (call under the mutex)."""
        live = {}
        for p in self.slot_dir.glob("slot-*.json"):
            data = self._holder_live(p)
            if data is None:
                with contextlib.suppress(OSError):
                    p.unlink()
                continue
            try:
                live[int(p.stem.split("-")[1])] = data
            except ValueError:
                pass
        return live

    def _exclusive_holder(self):
        data = self._holder_live(self.exclusive_path) if self.exclusive_path.exists() else None
        if data is None and self.exclusive_path.exists():
            with contextlib.suppress(OSError):
                self.exclusive_path.unlink()
        return data

    # ---- acquire / release -------------------------------------------------------------------------------------
    def acquire(self, why, exclusive=False, timeout=None, log=print, root=None, extra=None) -> Lease:
        """Wait for a slot (the whole machine when exclusive) and take it. TimeoutError after `timeout` seconds.
        `root`: the checkout the run is for (worktree.py list shows it; default: the working folder). `extra`: more
        fields for the slot file."""
        start = time.time()
        told = None
        mine_marker = False
        token = uuid.uuid4().hex  # which acquire() call owns the exclusive marker (two in one process must not mix)
        try:
            while True:
                blocker = None
                with self._mutex():
                    live = self._live_slots()
                    marker = self._exclusive_holder()
                    mine = exclusive and bool(marker) and marker.get("token") == token
                    if marker and not mine:
                        blocker = f"exclusive run by {marker.get('why')}"
                    elif exclusive:
                        if not marker:
                            if _create_json(self.exclusive_path, self._record(why, "exclusive", None, token, root)):
                                mine_marker = True
                        elif mine:
                            mine_marker = True
                        if mine_marker and not live:
                            return self._take(0, why, True, start, root, extra)
                        blocker = f"{len(live)} run(s) in progress, waiting for them to finish ({self._describe(live)})"
                    elif len(live) >= self.slots:
                        blocker = f"all {self.slots} slots busy ({self._describe(live)})"
                    else:
                        index = next(i for i in range(len(live) + 1) if i not in live)
                        return self._take(index, why, False, start, root, extra)
                if timeout is not None and time.time() - start > timeout:
                    raise TimeoutError(f"slots: waited {timeout:.0f} s for {why}: {blocker}")
                if blocker != told:
                    log(f"slots: {why}: waiting, {blocker}")
                    told = blocker
                time.sleep(self.poll)
        except BaseException:
            if mine_marker:
                with self._mutex():
                    marker = _read_json(self.exclusive_path)
                    if marker and marker.get("token") == token:
                        with contextlib.suppress(OSError):
                            self.exclusive_path.unlink()
            raise

    def _record(self, why, kind, index, token=None, root=None):
        return {"pid": os.getpid(), "why": why, "kind": kind, "slot": index, "since": _now_iso(),
                "root": str(root) if root else os.getcwd(), "token": token}

    @staticmethod
    def _describe(live):
        return ", ".join(sorted(str(d.get("why", "?")) for d in live.values())) or "none"

    def _take(self, index, why, exclusive, start, root=None, extra=None):
        """Under the mutex: write the slot file, return the Lease."""
        path = self.slot_dir / f"slot-{index}.json"
        record = self._record(why, "exclusive" if exclusive else "shared", index, None, root)
        record.update(extra or {})
        if not _create_json(path, record):
            raise RuntimeError(f"slots: {path} appeared under the mutex")
        lease = Lease(self, index, exclusive, why)
        lease.waited = time.time() - start
        return lease

    def _heartbeat(self, lease):
        now = time.time()
        for p in (self.slot_dir / f"slot-{lease.index}.json", self.exclusive_path if lease.exclusive else None):
            if p is not None and p.exists():
                os.utime(str(p), (now, now))

    def _release(self, lease):
        with self._mutex():
            with contextlib.suppress(OSError):
                (self.slot_dir / f"slot-{lease.index}.json").unlink()
            if lease.exclusive:
                marker = _read_json(self.exclusive_path)
                if marker and marker.get("pid") == os.getpid():
                    with contextlib.suppress(OSError):
                        self.exclusive_path.unlink()

    # ---- holders by checkout (hold / release) ------------------------------------------------------------------
    def holder_for(self, root):
        """(index, record) of the live slot held for this checkout, else None (call under the mutex)."""
        for index, data in sorted(self._live_slots().items()):
            if data.get("root") and _same_path(data["root"], root):
                return index, data
        return None

    def update_holder(self, index, **fields):
        """Under the mutex: change fields of a slot file (pushes back a detached holder's deadline)."""
        path = self.slot_dir / f"slot-{index}.json"
        data = _read_json(path)
        if data is not None:
            data.update(fields)
            _write_json(path, data)

    # ---- inspection --------------------------------------------------------------------------------------------
    def status(self):
        with self._mutex():
            live = self._live_slots()
            marker = self._exclusive_holder()
        return {"slots": self.slots, "live": live, "exclusive": marker}


DETACHED_MINUTES = 240   # hold --detach: the longest a background holder lives if release never comes


def _say(text):
    print(text, flush=True)


def _hold_foreground(pool, args):
    """hold: wait for the slot, print HELD, keep it until stdin closes or says `release` (--keep: until the release
    marker or the deadline), then give it back."""
    root = args.root or os.getcwd()
    keep = args.keep  # the background process: stdin is nothing, the end is `release` or the deadline
    with pool._mutex():
        found = pool.holder_for(root)
        if found:
            index, data = found
            if data.get("kind") == "exclusive" or args.shared:
                if data.get("detached"):  # a later prepare in the same root takes it back: the deadline starts again
                    pool.update_holder(index, until=time.time() + args.max_minutes * 60)
                _say("REUSED")
                return 0
            _say(f"FAILED {root} already holds a shared slot ({data.get('why')}); it cannot be made exclusive")
            return 4
    extra = {"detached": True, "until": time.time() + args.max_minutes * 60} if keep else None
    try:
        lease = pool.acquire(args.why, exclusive=not args.shared, root=root, extra=extra, log=_say,
                             timeout=args.wait_minutes * 60 if args.wait_minutes else None)
    except TimeoutError as e:
        _say(f"FAILED {e}")
        return 3
    _say(f"HELD {lease.index} {'shared' if args.shared else 'exclusive'}")
    done = threading.Event()
    if not keep:
        def watch_stdin():
            for line in sys.stdin:  # ends at EOF: the script that started us has gone
                if line.strip() == "release":
                    break
            done.set()
        threading.Thread(target=watch_stdin, daemon=True).start()
    slot_file = pool.slot_dir / f"slot-{lease.index}.json"
    marker = pool.slot_dir / f"slot-{lease.index}.release"
    try:
        while not done.wait(1.0):
            data = _read_json(slot_file)
            if data is None or data.get("pid") != os.getpid():
                break  # taken over as abandoned (the machine slept for minutes): nothing left to hold
            if keep and (marker.exists() or time.time() > data.get("until", 0)):
                break
    finally:
        lease.release()
        with contextlib.suppress(OSError):
            marker.unlink()
    return 0


def _hold_detached(args):
    """hold --detach: start the background holder, return once it has the slot (or failed)."""
    cmd = [sys.executable, "-I", "-X", "utf8", str(Path(__file__).resolve()), "hold", "--keep", "--why", args.why,
           "--max-minutes", str(args.max_minutes), "--wait-minutes", str(args.wait_minutes)]
    for flag, value in (("--root", args.root), ("--slots", args.slots)):
        if value is not None:
            cmd += [flag, str(value)]
    if args.shared:
        cmd.append("--shared")
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             universal_newlines=True, encoding="utf-8", **kwargs)
    for line in child.stdout:
        line = line.rstrip("\r\n")
        _say(line)
        if line.startswith(("HELD", "REUSED")):
            return 0
        if line.startswith("FAILED"):
            child.wait()
            return child.returncode or 3
    _say("FAILED the background holder ended before it had the slot")
    return 3


def _release_root(pool, args):
    """release: give back what `hold --detach` holds for this checkout (nothing held: fine)."""
    root = args.root or os.getcwd()
    with pool._mutex():
        found = pool.holder_for(root)
        if not found or not found[1].get("detached"):
            return 0  # nothing of that kind; an attached holder ends with its script
        index, data = found
        marker = pool.slot_dir / f"slot-{index}.release"
        marker.write_text("release", encoding="utf-8")
    deadline = time.time() + 10
    while time.time() < deadline:
        if not (pool.slot_dir / f"slot-{index}.json").exists():
            return 0
        time.sleep(0.1)
    print(f"slots: the holder for {root} (pid {data.get('pid')}) did not let go; removing its slot", file=sys.stderr)
    with pool._mutex():
        for p in (pool.slot_dir / f"slot-{index}.json", pool.exclusive_path, marker):
            with contextlib.suppress(OSError):
                p.unlink()
    return 0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="slots.py", description="Shared game-run slots (see the module docstring).")
    sub = ap.add_subparsers(dest="command")
    sub.add_parser("status", help="who holds what")
    hold = sub.add_parser("hold", help="take a slot and keep it (exclusive unless --shared)")
    hold.add_argument("--why", required=True)
    hold.add_argument("--root", help="the checkout this run is for (default: the working folder)")
    hold.add_argument("--shared", action="store_true", help="a shared slot (default: exclusive, the whole machine)")
    hold.add_argument("--slots", type=int, default=None)
    hold.add_argument("--wait-minutes", type=float, default=60, help="give up waiting after this (0: wait forever)")
    hold.add_argument("--detach", action="store_true", help="keep the slot in a background process until `release`")
    hold.add_argument("--keep", action="store_true", help=argparse.SUPPRESS)  # the background process itself
    hold.add_argument("--max-minutes", type=float, default=DETACHED_MINUTES, help="detached: the longest it holds")
    rel = sub.add_parser("release", help="give back what `hold --detach` holds for a checkout")
    rel.add_argument("--root")
    args = ap.parse_args(argv[1:])
    if args.command == "status":
        pool = SlotPool()
        st = pool.status()
        print(f"state folder: {pool.dir}")
        print(f"slots: {len(st['live'])} of {st['slots']} in use")
        for index, d in sorted(st["live"].items()):
            print(f"  slot {index}: {d.get('kind')} pid {d.get('pid')} since {d.get('since')}: {d.get('why')} ({d.get('root')})")
        if st["exclusive"]:
            print(f"exclusive: pid {st['exclusive'].get('pid')}: {st['exclusive'].get('why')}")
        return 0
    if args.command == "hold":
        if args.detach:
            return _hold_detached(args)
        return _hold_foreground(SlotPool(slots=args.slots), args)
    if args.command == "release":
        return _release_root(SlotPool(), args)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
