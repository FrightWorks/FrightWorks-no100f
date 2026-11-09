"""Smoke test: boot the game for real, 3 jobs at once (Python 3.8+, Windows and Linux, standard library; numpy for one check).

    python tools/smoke-test.py [--jobs h001,i004,g001] [--scenes e005,f001] [--slots 3] [--timeout 150] [--keep] [--exclusive] [--skip-build-check]

Run it after a fresh build (tools\\build-no100f.ps1), before a PR. Each job runs from its own copy of Play\\ (testkit/launch.py:
rumble off, muted, controller isolation, own config), takes a shared run slot (testkit/slots.py) and waits until the game is
playable (testkit/waitplay.py). All fail on a crash, a logged fault, a black picture or a wrong build.

  h001  boot, HD on: skips the intro; HD textures really drawn (>= 20 of the pack's textures; `textures` dev command); the audio
        dump is not silent and not stuck on a loop (tools/debug/audio/loopcheck.py, needs numpy); picture not black.
  i004  HD off (0 pack textures drawn): a scripted A press moves Scooby; `scene e001` travels there and it is playable again.
  g001  save and load: a known snack count, onto the save point, saved with scripted presses, the file in Saves\\MemoryCard;
        restart from the same copy, load from the title menu; Scooby is back at the save point and the count matches.

--scenes boots each extra scene (the level you changed): playable, a picture, still running.

Only a timeout is retried (once, from a fresh launch, reported "passed on retry"); a crash or a failed check fails at once.
A check that could not run is "not run", never a pass (exit code 3 when that is all there is).
Output: autotest/smoke-<time>/ with REPORT.md, results.json, shots/, logs/. Exit code 0 passed, 1 failed, 2 wrong build, 3 not run.
"""
import argparse
import json
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from testkit import checks, launch, slots, waitplay  # noqa: E402
from testkit.devmenu import DevMenuTimeout  # noqa: E402
from testkit.launch import BuildMismatch, GameCrashed, REPO  # noqa: E402
from testkit.recorder import CheckFailed, Recorder  # noqa: E402
from testkit.waitplay import PlayTimeout  # noqa: E402

TOOLS = Path(__file__).resolve().parent
HD_MIN_DRAWN = 20            # of the pack's 1670 textures, seen drawn in h001 (123 in the first real run)
AUDIO_MIN_SECONDS = 19       # loopcheck looks at 8 s .. 18 s of the dump
AUDIO_SILENT_PEAK = 200      # loopcheck.py's own limit for "silent"
LOOP_STUCK = 0.9
SAVE_POINT = (-25.618, 1.186, -1.515)   # g001
LOADED_AT = (-27.0, 0.121, 0.0)         # g001: where a load puts Scooby (the level's start)
SNACKS = 123

JobFailure = CheckFailed


class Job(Recorder):
    """One job's context: its Play copies, its checks and its numbers."""

    def __init__(self, name, args, out, pool):
        super().__init__(name, out)
        self.args, self.pool = args, pool
        self.games = []

    def new_game(self, attempt, **kw):
        g = launch.TestGame(self.name, self.out / "work" / f"{self.name}-{attempt}",
                            build_check=not self.args.skip_build_check, release=getattr(self.args, "release", None),
                            activate_when_shown=getattr(self.args, "activate_when_shown", None), **kw)
        g.prepare()
        self.games.append(g)
        return g

    def record_perf(self, game, label):
        try:
            text = game.devmenu.command("perf", 10)
        except DevMenuTimeout:
            return
        self.times[f"perf {label}"] = text
        self.log(f"info  {text}")

    def finish_attempt(self, game):
        game.check_crash()
        self.passed("no crash or fault", "stderr clean, game still running")


def common_start(job, game, boot, attempt):
    """Start, wait until playable, check the build ID the game itself prints. Returns waitplay's result."""
    t0 = time.time()
    game.start()
    result = waitplay.wait_until_playable(
        game, boot, timeout=job.args.timeout, screenshot=job.out / "shots" / f"{job.name}-timeout-{attempt}.png",
        log=job.log)
    job.times[f"playable{attempt}"] = round(time.time() - t0, 1)
    msg = game.check_console_build()
    if msg is None:
        job.not_run("build ID in console.log", "the game printed none")
    else:
        job.passed("build ID in console.log", msg)
    return result


# ---- the jobs ---------------------------------------------------------------------------------------------------------
def job_h001(job, attempt):
    raw = job.out / "logs" / f"h001-{attempt}.raw"
    raw.parent.mkdir(parents=True, exist_ok=True)
    game = job.new_game(attempt, boot="h001", hd=True, extra_env={"AUDIO_RAW_DUMP": str(raw)})
    pack = game.src_play / "texture_replacements" / "ScoobyHD"
    job.passed("HD probe written", f"{checks.write_hd_probe(game.play, pack)} pack textures registered")
    common_start(job, game, "h001", attempt)
    time.sleep(1.5)  # the game updates the drawn list every 30 frames
    drawn = checks.hd_drawn(game.devmenu)
    job.require(drawn >= HD_MIN_DRAWN, "HD textures drawn", f"{drawn} of the pack drawn in h001 (need {HD_MIN_DRAWN})")
    # audio: wait until the dump has enough seconds
    end = time.time() + 40
    while checks.audio_seconds(raw) < AUDIO_MIN_SECONDS and time.time() < end:
        game.check_crash()
        time.sleep(0.5)
    seconds = checks.audio_seconds(raw)
    job.require(seconds >= AUDIO_MIN_SECONDS, "audio dump", f"{seconds:.1f} s written")
    peak = checks.audio_peak(raw)
    job.require(peak > AUDIO_SILENT_PEAK, "audio not silent", f"peak {peak}")
    try:
        corr = checks.loopcheck(raw, TOOLS)
        job.require(corr < LOOP_STUCK, "audio not stuck on a loop", f"repeat correlation {corr:.3f} (stuck at {LOOP_STUCK})")
    except checks.CheckNotRun as e:
        job.not_run("audio not stuck on a loop", str(e))
    job.check_picture(game, "h001")
    job.record_perf(game, "h001")
    job.finish_attempt(game)


def job_i004(job, attempt):
    game = job.new_game(attempt, boot="i004", hd=False)
    pack = game.src_play / "texture_replacements" / "ScoobyHD"
    try:
        checks.write_hd_probe(game.play, pack)
        probe = True
    except checks.CheckNotRun:
        probe = False
    common_start(job, game, "i004", attempt)
    dm = game.devmenu
    time.sleep(1.5)
    if probe:
        drawn = checks.hd_drawn(dm)
        job.require(drawn == 0, "HD off draws no pack texture", f"{drawn} drawn")
    where0 = dm.where()
    job.require(where0 is not None and where0[0] == "i004", "in i004", str(where0))
    best = 0.0
    for _try in range(2):  # a press can be taken between two frames of a pause; once more before failing
        dm.press("a 30", 10)  # 30 frames: a long jump (y +1.9 in the first real run; `a 10` only hops 0.01)
        t = time.time()
        while time.time() - t < 3.0 and best <= 0.5:
            w = dm.where()
            if w:
                best = max(best, max(abs(w[i] - where0[i]) for i in (1, 2, 3)))
        if best > 0.5:
            break
    job.require(best > 0.5, "a scripted A press moves Scooby", f"largest move {best:.2f} units")
    job.check_picture(game, "i004")
    dm.command("scene e001", 10)
    where = waitplay.wait_for_scene(game, "e001", timeout=60, screenshot=job.out / "shots" / f"i004-scene-{attempt}.png")
    job.passed("travelled to e001", f"where {where}")
    r = waitplay.wait_until_playable(game, None, timeout=job.args.timeout, log=job.log)
    job.passed("e001 playable again", f"after {r['seconds']:.1f} s")
    job.check_picture(game, "e001")
    job.record_perf(game, "e001")
    job.finish_attempt(game)


def readiness_mode(dm, timeout=10):
    """(ready, mode, state) from the `readiness` dev command, which answers in menus too."""
    text = dm.command("readiness", timeout)
    m = re.search(r"mode=(\d+) state=(\d+)", text)
    return text.startswith("interaction readiness: ready"), int(m.group(1)) if m else -1, int(m.group(2)) if m else -1


def wait_readiness(job, game, want, what, timeout, shot):
    """Poll `readiness` until want(ready, mode, state); a screenshot and a failed check if it never comes."""
    end = time.time() + timeout
    last = None
    while time.time() < end:
        game.check_crash()
        try:
            last = readiness_mode(game.devmenu, 5)
        except DevMenuTimeout:
            last = None
        if last and want(*last):
            return last
        time.sleep(0.5)
    job.shot(game, shot)
    job.require(False, what, f"not reached in {timeout:.0f} s (last readiness {last})")


def job_g001(job, attempt):
    """Save at the save point with menu presses, restart from the title menu, load, read position and snacks back.
    The presses were worked out by hand: Save Game = A (card slot A), A (first of three slots), no confirmation; main menu
    = Start, Down, A (Load Game), A (card slot A), A (first save). `readiness` tells play (mode 11) from menus."""
    game = job.new_game(attempt, boot="g001", hd=False)
    common_start(job, game, "g001", attempt)
    dm = game.devmenu
    saves = game.play / "UserData" / "Saves" / "MemoryCard"
    job.require(not list(saves.rglob("*.gci")), "starts without a save", "fresh copy")
    dm.command(f"snacks {SNACKS}", 10)
    job.require(f"snacks={SNACKS}" in dm.command("player", 10), "snack count set", str(SNACKS))
    dm.command("tp %s %s %s" % SAVE_POINT, 10)
    wait_readiness(job, game, lambda ready, mode, state: not ready and mode != 2, "save menu opened", 15, "g001-no-save-menu")
    job.passed("save menu opened by the save point")
    dm.press("a 10", 10)       # memory card slot A
    time.sleep(1.5)
    dm.press("a 10", 10)       # the first of the three save slots: saves, no confirmation
    wait_readiness(job, game, lambda ready, mode, state: ready, "back in play after saving", 20, "g001-save-stuck")
    dm.command("tp %s %s %s" % LOADED_AT, 10)  # off the save point, which reopens its menu a few seconds after it closes
    time.sleep(0.5)
    files = list(saves.rglob("*.gci"))
    job.require(files and files[0].stat().st_size > 0, "save file written",
                ", ".join(f"{f.relative_to(saves)} ({f.stat().st_size} bytes)" for f in files) or "none")
    game.stop()

    # restart from the same copy: no boot mod, so the game's own title menu; the saves stay
    game.set_boot(None)
    game.start()
    wait_readiness(job, game, lambda ready, mode, state: mode == 2, "title screen", job.args.timeout, "g001-no-title")
    time.sleep(3)
    for spec, pause in (("start 10", 3), ("down 10", 1.5), ("a 10", 3), ("a 10", 3), ("a 10", 0)):
        dm.press(spec, 10)
        time.sleep(pause)
    wait_readiness(job, game, lambda ready, mode, state: ready, "save loaded, in play", 40, "g001-load-stuck")
    where = dm.where(10)
    near = where and where[0] == "g001" and all(abs(where[i + 1] - LOADED_AT[i]) < 1.0 for i in range(3))
    job.require(near, "Scooby is where a load puts him", f"{where} (expected g001 {LOADED_AT})")
    player = dm.command("player", 10)
    job.require(f"snacks={SNACKS}" in player, "snack count survived the save and load", player.split(";")[0])
    job.check_picture(game, "g001-loaded")
    job.record_perf(game, "g001")
    job.finish_attempt(game)


def job_scene(job, attempt):
    """An extra scene (--scenes): boots, becomes playable, shows a picture, stays up."""
    game = job.new_game(attempt, boot=job.name, hd=False)
    common_start(job, game, job.name, attempt)
    where = game.devmenu.where()
    job.require(where is not None and where[0] == job.name, f"in {job.name}", str(where))
    time.sleep(2)
    job.check_picture(game, job.name)
    job.record_perf(game, job.name)
    job.finish_attempt(game)


JOBS = {"h001": job_h001, "i004": job_i004, "g001": job_g001}


# ---- running ------------------------------------------------------------------------------------------------------------
def run_job(name, args, out, pool, lease_held):
    job = Job(name, args, out, pool)
    started = time.time()
    result = {"job": name, "outcome": "failed", "detail": "", "checks": job.checks, "times": job.times}

    def attempt_fn(attempt):
        del job.checks[:]
        job.checks.append(("attempt", "info", f"attempt {attempt}"))
        try:
            return JOBS.get(name, job_scene)(job, attempt)
        finally:
            for g in job.games:
                g.stop()

    lease = None
    try:
        if not lease_held:
            lease = pool.acquire(f"smoke {name}", log=job.log)
            job.times["slot wait"] = round(lease.waited, 1)
            if lease.waited > 1:
                job.log(f"got slot {lease.index} after {lease.waited:.0f} s")
        job.times["start"] = round(time.time() - started, 1)
        outcome, _, notes = waitplay.retry_on_timeout(attempt_fn, log=job.log)
        result["outcome"] = outcome
        result["detail"] = "; ".join(notes)
        if any(status == "not run" for _c, status, _d in job.checks):
            result["outcome"] += " (some checks not run)"
    except BuildMismatch as e:
        result.update(outcome="failed: wrong build", detail=str(e))
    except GameCrashed as e:
        result.update(outcome="failed: crash", detail=str(e))
    except PlayTimeout as e:
        result.update(outcome="failed: timeout", detail=str(e))
    except (JobFailure, DevMenuTimeout) as e:
        result.update(outcome="failed: check", detail=str(e))
    except Exception as e:  # a bug in the test itself must not hide the other jobs
        result.update(outcome="failed: error", detail=f"{type(e).__name__}: {e}")
    finally:
        if lease:
            lease.release()
        for g in job.games:
            g.stop()
            (job.out / "logs").mkdir(parents=True, exist_ok=True)
            for f in (g.err_log, g.console_log()):
                if f and Path(f).exists():
                    dest = job.out / "logs" / f"{g.play.parent.name}-{Path(f).name}"
                    try:
                        dest.write_bytes(Path(f).read_bytes())
                    except OSError:
                        pass
    result["seconds"] = round(time.time() - started, 1)
    job.log(f"{result['outcome'].upper()} in {result['seconds']} s{': ' + result['detail'] if result['detail'] else ''}")
    return result


def write_report(out, results, args, build_msg, wall):
    (out / "results.json").write_text(json.dumps(results, indent=1, default=str), encoding="utf-8")
    lines = [f"# Smoke test {out.name}", "", f"Build: {build_msg}", "",
             f"{len(results)} job(s), {args.slots} slot(s), {wall:.0f} s wall.", "",
             "| Job | Outcome | Seconds | Detail |", "|---|---|---|---|"]
    for r in results:
        lines.append(f"| {r['job']} | {r['outcome']} | {r['seconds']} | {r['detail'].replace('|', '/')} |")
    for r in results:
        lines += ["", f"## {r['job']}", ""]
        lines += [f"- {c}: {s}" + (f" ({d})" if d else "") for c, s, d in r["checks"]]
        lines += [f"- time {k}: {v}" for k, v in r["times"].items()]
    (out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--jobs", default="h001,i004,g001")
    ap.add_argument("--scenes", default="", help="also boot these scenes (e.g. the level you changed): e005,f001")
    ap.add_argument("--slots", type=int, default=None, help="shared run slots (default: FRIGHTWORKS_TEST_SLOTS or 3)")
    ap.add_argument("--timeout", type=float, default=150, help="seconds each job may take to become playable")
    ap.add_argument("--out", default=None)
    ap.add_argument("--keep", action="store_true", help="keep the Play copies (default: delete them when the run passed)")
    ap.add_argument("--exclusive", action="store_true", help="take the machine alone (timing measurements)")
    ap.add_argument("--activate-when-shown", type=int, choices=(0, 1), default=None,
                    help="set SDL_WINDOW_ACTIVATE_WHEN_SHOWN for the games (0: new windows do not take focus); default: SDL's")
    ap.add_argument("--sequential", action="store_true", help="one job after another (solo timings; with --exclusive)")
    ap.add_argument("--skip-build-check", action="store_true", help="manual override: do not check the build ID (reported)")
    ap.add_argument("--release", metavar="VER", help="check a release build: its ID (exe and console.log) must be exactly the plain version VER")
    args = ap.parse_args()
    if args.release and args.skip_build_check:
        ap.error("--release and --skip-build-check exclude each other")
    args.slots = slots.slot_count(args.slots)
    names = [j for j in args.jobs.split(",") if j]
    unknown = [j for j in names if j not in JOBS]
    if unknown:
        ap.error(f"unknown job(s) {unknown}; known: {', '.join(JOBS)}")
    scenes = [x.lower() for x in args.scenes.split(",") if x]
    bad = [x for x in scenes if not re.fullmatch(r"[a-z]\d{3}", x) or x in JOBS]
    if bad:
        ap.error(f"--scenes takes scene IDs like e005 (not one of the jobs): {bad}")
    names += [x for x in scenes if x not in names]
    out = Path(args.out).resolve() if args.out else REPO / "autotest" / time.strftime("smoke-%Y%m%d-%H%M%S")
    for sub in ("shots", "logs", "work"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    try:
        build_msg = launch.TestGame("check", out / "work" / "check", build_check=not args.skip_build_check, release=args.release).check_build()
    except (BuildMismatch, FileNotFoundError) as e:
        print(f"smoke test not started: {e}", file=sys.stderr)
        return 2
    print(build_msg)

    pool = slots.SlotPool(slots=args.slots)
    t0 = time.time()
    results, lock = [], threading.Lock()
    exclusive = pool.acquire("smoke test (exclusive)", exclusive=True) if args.exclusive else None
    try:
        def work(name):
            r = run_job(name, args, out, pool, exclusive is not None)
            with lock:
                results.append(r)
        if args.sequential:
            for n in names:
                work(n)
        else:
            threads = [threading.Thread(target=work, args=(n,), name=n) for n in names]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
    finally:
        if exclusive:
            exclusive.release()
    results.sort(key=lambda r: names.index(r["job"]))
    wall = time.time() - t0
    write_report(out, results, args, build_msg, wall)
    failed = [r for r in results if r["outcome"].startswith("failed")]
    not_run = [r for r in results if "not run" in r["outcome"]]
    for r in results:
        print(f"{r['job']:6} {r['outcome']} ({r['seconds']} s)")
    print(f"smoke test: {len(results) - len(failed)}/{len(results)} passed in {wall:.0f} s; report {out / 'REPORT.md'}")
    if not failed and not args.keep:
        launch.safe_rmtree(out / "work")
    return 1 if failed else 3 if not_run else 0


if __name__ == "__main__":
    sys.exit(main())
