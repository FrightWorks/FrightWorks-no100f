"""Level sweep: boot every level of the game for real, wait until it is playable, run it for a few seconds and check it
(Python 3.8+, Windows and Linux, standard library only). Built on the test kit in tools/testkit/, like tools/smoke-test.py.

    python tools/level-sweep.py [--levels h001,c003] [--workers 7] [--seconds 8] [--timeout 150] [--fps30] [--keep]
                                [--slots 7] [--exclusive] [--min-fps N] [--skip-build-check] [--out DIR]

Run it after a fresh build (tools\\build-no100f.ps1), before a release. Each worker runs its own copy of Play\\ (testkit/launch.py:
rumble off, muted, controller isolation, own config and boot mod); a worker takes a shared run slot (testkit/slots.py) for each
level, so other automated runs (the smoke test, another checkout's sweep) share the machine instead of waiting for a whole sweep.
Up to --workers (default 7) game windows at once. The build ID of the exe must match the commit under test.

Per level: boot straight into it (sd2.ini BOOT=<level>, saves cleared), wait until it is playable (testkit/waitplay.py: skips a
cutscene, waits out a camera pan; no fixed 30 s, no blind presses), then --seconds more while watching the game, then:
  * alive, no AccessViolation, no OS panic / missing_target / Unhandled / FATAL on stderr (a fault fails the level at once);
  * a picture: the window is not black (screenshot, Windows only; "not run" elsewhere). A black one is shot again up to 4 times,
    3 s apart, before it fails: with nobody at the pad an enemy can kill Scooby at the spawn (o005, p002), and the screen
    is black during the death fade until he respawns. A retake that was lit is noted in REPORT.md ("picture retaken");
  * frame rate: the game's own two-second fps lines (NO100F_LOG_FPS) after it was playable; the median must reach --min-fps
    (default: half the target, 30 at 60 fps, 15 at 30 fps).
While the game says "no cutscene, no control" for more than 4 s (a dialogue box such as f003's "PRESS A TO CONTINUE") the wait presses
A every 2 s, at most 12 times; every press is logged and counted in the report ("dialogue press(es)").
A scene in KNOWN_EXCEPTIONS (s006: the ending cutscene, not a playable level) is not run: REPORT.md lists it as "skipped (known)" with the reason,
it does not count against the run (exit code 0 when everything else passed). --include-known runs it anyway.
A level that does not become playable in --timeout gets one more try from a fresh launch ("passed on retry"); a crash or a failed
check is final. A check that could not run is "not run", never a pass.

Settings (testkit/launch.py's test config, the same as the smoke test, on top of the game's defaults): windowed 1280x720, render
size = window, 16:9, skip_intro, MSAA off, HD textures off (the smoke test covers HD), fps60 = true (--fps30: false, 30 fps),
frame interpolation not set (off), sound muted, rumble off. The old script also wrote an interpolation target of 120: dropped.

Output: autotest/level-sweep-<time>/ with REPORT.md, results.json, shots/<level>.png and logs/<level>-<attempt>.{err,console}.log.
Exit code 0 passed (known exceptions skipped), 1 a level failed or is missing, 2 wrong build, 3 no failure but a check could not run.
"""
import argparse
import json
import re
import shutil
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

DISC = REPO / "Disc" / "DATA" / "files"
NOT_LEVELS = ("MN", "fmv")      # menu screens and movies: no player to wait for
DEFAULT_WORKERS = 7
PICTURE_RETAKES = 4             # a black picture is shot again this many times, PICTURE_RETAKE_WAIT apart, before it fails
PICTURE_RETAKE_WAIT = 3.0

# Levels where an unattended Scooby is caught at the spawn within seconds. Measured 2026-11-08 (dev menu `player` next to a shot every
# ~3 s): health 3, 2 (, 1), then "dead/dying" with the picture black (the game's own death fade), then back at 3 and lit; about every
# 13 s, in both levels. Their monsters: o005 HEADY1/2, WOLFMAN/2, WITCH ...; p002 ZOMBIE1/2, HEADLESS1/2, GARGOYLE1, BAT2 ...
DEADLY_SPAWN = ("o005", "p002")
DEATH_FADE = "Scooby caught by the level's monsters at the spawn (the game's own death fade, then he respawns)"

# Scenes on the disc that are not playable levels, so the sweep does not run them (REPORT.md lists them as "skipped (known)", never as
# passed; --include-known runs them anyway). The rest of the disc's scenes are all in the level table of tools/debug/game/levels.md
# (checked 2026-11-08; the menus mnu3/mnu4 and the movies are left out by NOT_LEVELS).
KNOWN_EXCEPTIONS = {
    # levels.md: "Ending: OUTTRO cutscene, BACK TO HUB portal; not in the level table". Booted anyway (2026-11-08): the cutscene manager
    # stays set, no player is placed (`where` = 0 0 0) and no press (A, Start, B, X, the dev menu's `skip`) ended it in 40 s, so the wait
    # for player control never ends.
    "s006": "the ending (OUTTRO cutscene), not a playable level; see tools/debug/game/levels.md",
}


def all_levels(disc=None):
    disc = Path(disc) if disc else DISC
    out = []
    for d in sorted(disc.iterdir()):
        if d.is_dir() and d.name not in NOT_LEVELS:
            out += [p.stem.lower() for p in sorted(d.glob("*.HIP"))]
    return out


def crash_kind(message):
    """What a GameCrashed message says happened: 'access violation', 'panic' or 'exited'."""
    if "AccessViolation" in message:
        return "access violation"
    if "exited with code" in message:
        return "exited"
    return "panic"


class Level(Recorder):
    """One level's record on one worker (its checks, its numbers)."""

    def __init__(self, name, out):
        super().__init__(name, out)
        self.attempts = 0


def new_game(worker, args, out):
    kw = dict(getattr(args, "game_kwargs", {}))      # the tests hand in a stand-in game
    extra = dict(kw.pop("extra_env", {}))
    extra.setdefault("NO100F_LOG_FPS", "1")           # the game's own fps lines on stderr
    g = launch.TestGame(f"w{worker}", out / "work" / f"w{worker}", hd=False, fps60=not args.fps30, extra_env=extra,
                        build_check=not args.skip_build_check,
                        activate_when_shown=getattr(args, "activate_when_shown", None), **kw)
    g.prepare()
    return g


def player_note(game):
    """Why a picture may be black: the dev menu says the player is dead/dying (the death fade). '' when it does not."""
    try:
        text = game.devmenu.command("player", 5)
    except DevMenuTimeout:
        return ""
    return "player dead/dying" if "dead/dying" in text else ""


def check_picture(rec, game, retakes=None, wait=None):
    """The picture check with retakes for a black frame. A retake that is lit passes and says so (rec.notes, REPORT.md)."""
    retakes = PICTURE_RETAKES if retakes is None else retakes
    wait = PICTURE_RETAKE_WAIT if wait is None else wait
    png = rec.shot(game)
    if png is None:
        rec.not_run("picture", "no screenshot tool on this OS")
        return
    blank = 0            # black shots so far
    why = set()          # what the dev menu said at those moments (the death fade), if anything
    for n in range(retakes + 1):
        lit, total = checks.png_pixels_lit(png)
        if not checks.png_is_blank(png):
            if blank:
                note = f"black frame retaken {n}x ({n * wait:g} s)"
                if why or rec.name in DEADLY_SPAWN:
                    note += ": " + DEATH_FADE + (" [dev menu: player dead/dying at the black shot]" if why else "")
                rec.notes.append(note)
                rec.passed("picture", f"{png.name}: {lit / total:.1%} lit after {n} retake(s); {note}")
            else:
                rec.passed("picture", f"{png.name}: {lit / total:.1%} of the pixels lit")
            return
        blank += 1
        note = player_note(game)
        if note:
            why.add(note)
        if n == retakes:
            break
        time.sleep(wait)
        game.check_crash()
        png = rec.shot(game, f"retake{n + 1}")
    rec.fail("picture", f"{png.name}: {lit / total:.1%} of the pixels lit, black in all {retakes + 1} shots over "
                        f"{retakes * wait:g} s" + (f" ({', '.join(sorted(why))})" if why else ""))


def check_fps(rec, args, summary):
    target = 30 if args.fps30 else 60
    minimum = args.min_fps if args.min_fps is not None else target / 2
    if summary is None:
        rec.not_run("frame rate", "the game logged no fps line while the level ran")
        return
    rec.times["fps"] = summary
    detail = f"median {summary['median']}, min {summary['min']}, last {summary['last']} ({summary['n']} samples, need >= {minimum:g})"
    rec.require(not checks.fps_too_slow(summary, minimum), "frame rate", detail)


def play_level(rec, game, level, args, attempt):
    """One launch of `level`: playable, watched for args.seconds, then the checks. Raises GameCrashed, PlayTimeout,
    BuildMismatch (never retried but a timeout) or CheckFailed."""
    launch.safe_rmtree(game.play / "UserData" / "Saves")
    game.set_boot(level)
    t0 = time.time()
    game.start()
    result = waitplay.wait_until_playable(
        game, level, timeout=args.timeout, screenshot=rec.out / "shots" / f"{level}-timeout-{attempt}.png", log=rec.log)
    rec.times["playable"] = round(time.time() - t0, 1)
    dialogue = result.get("dialogue_presses", 0)
    rec.passed("playable", f"after {result['seconds']:.1f} s, {result['presses']} skip press(es), {dialogue} dialogue press(es)")
    if dialogue:
        rec.notes.append(f"{dialogue} A press(es) for a dialogue box before control")
    msg = game.check_console_build()
    if msg is None:
        rec.not_run("build ID in console.log", "the game printed none")
    else:
        rec.passed("build ID in console.log", msg)
    scene = result["where"].split(" ")[0].lower() if result.get("where") else ""
    rec.require(scene == level, f"in {level}", f"where: {result.get('where')}")
    try:
        mark = game.err_log.stat().st_size
    except OSError:
        mark = 0
    end = time.time() + args.seconds
    while time.time() < end:        # watch it run: a fault or an exit fails the level at once
        game.check_crash()
        time.sleep(0.5)
    try:
        text = game.err_log.read_bytes()[mark:].decode("utf-8", errors="replace")
    except OSError:
        text = ""
    summary = checks.fps_summary(checks.fps_samples(text))
    game.check_crash()
    rec.passed("alive, no crash or fault", f"still running {args.seconds:g} s after it was playable")
    problems = []
    for check in (lambda: check_picture(rec, game), lambda: check_fps(rec, args, summary)):
        try:
            check()
        except CheckFailed as e:
            problems.append(e)
    game.check_crash()              # a fault while the screenshot was taken
    if problems:
        raise CheckFailed("; ".join(str(p) for p in problems), problems[0].check)


def keep_logs(game, out, level, attempt):
    (out / "logs").mkdir(parents=True, exist_ok=True)
    for src, tail in ((game.err_log, "err.log"), (game.console_log(), "console.log")):
        if src and Path(src).exists():
            try:
                shutil.copyfile(str(src), str(out / "logs" / f"{level}-{attempt}.{tail}"))
            except OSError:
                pass


def run_level(level, game, args, out, pool, lease_held):
    """Run one level on a worker's game: slot, up to two attempts, result dict (outcome 'passed', 'passed on retry' or 'failed: ...')."""
    rec = Level(level, out)
    started = time.time()
    if level in KNOWN_EXCEPTIONS and not getattr(args, "include_known", False):
        rec.log(f"SKIPPED (known): {KNOWN_EXCEPTIONS[level]}")
        return {"level": level, "outcome": "skipped (known)", "detail": KNOWN_EXCEPTIONS[level], "checks": [], "times": {},
                "seconds": 0, "attempts": 0}
    result = {"level": level, "outcome": "failed", "detail": "", "checks": rec.checks, "times": rec.times}

    def attempt_fn(attempt):
        rec.attempts = attempt
        del rec.checks[:]
        rec.checks.append(("attempt", "info", f"attempt {attempt}"))
        try:
            return play_level(rec, game, level, args, attempt)
        finally:
            game.stop()
            keep_logs(game, out, level, attempt)

    lease = None
    try:
        if not lease_held:
            lease = pool.acquire(f"sweep {level}", log=rec.log)
            rec.times["slot wait"] = round(lease.waited, 1)
        outcome, _, notes = waitplay.retry_on_timeout(attempt_fn, log=rec.log)
        result["outcome"] = outcome
        result["detail"] = "; ".join(notes + rec.notes)
        if any(status == "not run" for _c, status, _d in rec.checks):
            result["outcome"] += " (some checks not run)"
    except BuildMismatch as e:
        result.update(outcome="failed: wrong build", detail=str(e))
    except GameCrashed as e:
        result.update(outcome=f"failed: crash ({crash_kind(str(e))})", detail=str(e))
    except PlayTimeout as e:
        result.update(outcome="failed: timeout", detail=str(e))
    except CheckFailed as e:
        result.update(outcome=f"failed: {e.check or 'check'}", detail=str(e))
    except DevMenuTimeout as e:
        result.update(outcome="failed: timeout", detail=f"the dev menu stopped answering: {e}")
    except Exception as e:  # a bug in the test itself must not stop the other levels
        result.update(outcome="failed: error", detail=f"{type(e).__name__}: {e}")
    finally:
        if lease:
            lease.release()
        game.stop()
    result["attempts"] = rec.attempts
    result["seconds"] = round(time.time() - started, 1)
    rec.log(f"{result['outcome'].upper()} in {result['seconds']} s{': ' + result['detail'] if result['detail'] else ''}")
    return result


def sweep(levels, args, out, pool, lease_held=False, stop=None):
    """All `levels` on args.workers workers; returns the results in level order. `stop` (an Event) ends it early."""
    stop = stop or threading.Event()
    queue, results, lock, games = list(levels), [], threading.Lock(), []

    def worker(n):
        game = None
        while not stop.is_set():
            with lock:
                if not queue:
                    return
                level = queue.pop(0)
            try:
                if game is None:
                    game = new_game(n, args, out)
                    with lock:
                        games.append(game)
                r = run_level(level, game, args, out, pool, lease_held)
            except Exception as e:     # no game to run (no Play\ copy): the level is not tested, not passed
                r = {"level": level, "outcome": "failed: error", "detail": f"{type(e).__name__}: {e}", "checks": [],
                     "times": {}, "seconds": 0, "attempts": 0}
            with lock:
                results.append(r)
                print(f"{len(results):3}/{len(levels)} {level}: {r['outcome']}", flush=True)

    threads = [threading.Thread(target=worker, args=(i,), name=f"w{i}") for i in range(min(args.workers, len(levels)))]
    try:
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.5)
    except KeyboardInterrupt:
        stop.set()
        print("interrupted: stopping the games, writing what is done", file=sys.stderr)
        for g in games:
            g.stop()
        for t in threads:
            t.join(30)
    results.sort(key=lambda r: levels.index(r["level"]))
    return results


def is_problem(r):
    return r["outcome"].startswith("failed")


def is_skipped(r):
    return r["outcome"].startswith("skipped")


def exit_code(results, planned):
    """0 everything passed (known exceptions are expected: skipped, not counted against the run), 1 a level failed or is missing,
    3 nothing failed but a check could not run."""
    if any(is_problem(r) for r in results) or len(results) < planned:
        return 1
    return 3 if any("not run" in r["outcome"] for r in results) else 0


def write_report(out, results, args, build_msg, wall, planned=None):
    (out / "results.json").write_text(json.dumps(results, indent=1, default=str), encoding="utf-8")
    bad = [r for r in results if is_problem(r)]
    skipped = [r for r in results if is_skipped(r)]
    not_run = [r for r in results if "not run" in r["outcome"]]
    fps = "fps60 off, 30 fps" if args.fps30 else "fps60 on, 60 fps"
    lines = [f"# Level sweep {out.name}", "", f"Build: {build_msg}", "",
             f"{len(results)} of {planned or len(results)} level(s), {args.workers} worker(s), {args.slots} slot(s), {fps}, "
             f"{args.seconds:g} s watched after playable, {wall:.0f} s wall: "
             f"{len(results) - len(bad) - len(skipped)} passed, {len(bad)} failed, {len(skipped)} skipped (known), "
             f"{len(not_run)} with checks not run.", ""]
    if bad:
        lines += ["## Problems", "", "| Level | Outcome | Detail |", "|---|---|---|"]
        lines += [f"| {r['level']} | {r['outcome']} | {r['detail'].replace('|', '/')} |" for r in bad]
        lines.append("")
    if skipped:
        lines += ["## Skipped (known exceptions: not tested, not passed)", "", "| Level | Why |", "|---|---|"]
        lines += [f"| {r['level']} | {r['detail'].replace('|', '/')} |" for r in skipped]
        lines.append("")
    lines += ["## All levels", "", "| Level | Outcome | Seconds | Playable (s) | FPS median | FPS min | Detail |",
              "|---|---|---|---|---|---|---|"]
    for r in results:
        f = r["times"].get("fps") or {}
        lines.append(f"| {r['level']} | {r['outcome']} | {r['seconds']} | {r['times'].get('playable', '')} | "
                     f"{f.get('median', '')} | {f.get('min', '')} | {r['detail'].replace('|', '/')} |")
    for r in results:
        if (is_problem(r) or r["outcome"] != "passed") and not is_skipped(r):
            lines += ["", f"## {r['level']}", ""]
            lines += [f"- {c}: {s}" + (f" ({d})" if d else "") for c, s, d in r["checks"]]
            lines += [f"- time {k}: {v}" for k, v in r["times"].items()]
    (out / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_levels(text, known):
    wanted = [x.strip().lower() for x in text.split(",") if x.strip()]
    unknown = [x for x in wanted if not re.fullmatch(r"[a-z]\d{3}", x) or x not in known]
    return wanted, unknown


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--levels", default="", help="only these levels, e.g. h001,c003 (default: every level on the disc)")
    ap.add_argument("--workers", type=int, default=DEFAULT_WORKERS, help="game windows at once (default 7)")
    ap.add_argument("--slots", type=int, default=None, help="shared run slots (default: as many as workers)")
    ap.add_argument("--seconds", type=float, default=8, help="seconds to keep watching a level once it is playable")
    ap.add_argument("--timeout", type=float, default=150, help="seconds a level may take to become playable (per attempt)")
    ap.add_argument("--fps30", "--no-fps60", dest="fps30", action="store_true",
                    help="run with [video] fps60 off (30 fps); default fps60 on")
    ap.add_argument("--min-fps", type=float, default=None, help="median fps a level must reach (default: half of 60, or of 30)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--include-known", action="store_true",
                    help="also run the levels in KNOWN_EXCEPTIONS (default: skipped, listed as 'skipped (known)')")
    ap.add_argument("--keep", action="store_true", help="keep the Play copies (default: delete them at the end)")
    ap.add_argument("--exclusive", action="store_true", help="take the machine alone (timing measurements)")
    ap.add_argument("--activate-when-shown", type=int, choices=(0, 1), default=None,
                    help="set SDL_WINDOW_ACTIVATE_WHEN_SHOWN for the games (0: new windows do not take focus)")
    ap.add_argument("--skip-build-check", action="store_true", help="manual override: do not check the build ID (reported)")
    args = ap.parse_args(argv)
    args.workers = max(1, args.workers)
    args.slots = slots.slot_count(args.slots if args.slots is not None else args.workers)
    known = all_levels()
    levels, unknown = parse_levels(args.levels, known) if args.levels else (known, [])
    if unknown:
        ap.error(f"unknown level(s) {unknown}; the disc has: {', '.join(known)}")
    out = Path(args.out).resolve() if args.out else REPO / "autotest" / time.strftime("level-sweep-%Y%m%d-%H%M%S")
    for sub in ("shots", "logs", "work"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    try:
        build_msg = launch.TestGame("check", out / "work" / "check", build_check=not args.skip_build_check).check_build()
    except (BuildMismatch, FileNotFoundError) as e:
        print(f"level sweep not started: {e}", file=sys.stderr)
        return 2
    print(build_msg)
    print(f"{len(levels)} level(s), {args.workers} worker(s), {'30' if args.fps30 else '60'} fps; output {out}")

    pool = slots.SlotPool(slots=args.slots)
    t0 = time.time()
    exclusive = pool.acquire("level sweep (exclusive)", exclusive=True) if args.exclusive else None
    try:
        results = sweep(levels, args, out, pool, exclusive is not None)
    finally:
        if exclusive:
            exclusive.release()
    wall = time.time() - t0
    write_report(out, results, args, build_msg, wall, planned=len(levels))
    bad = [r for r in results if is_problem(r)]
    skipped = [r for r in results if is_skipped(r)]
    print(f"level sweep: {len(results) - len(bad) - len(skipped)}/{len(results)} passed, {len(skipped)} skipped (known), "
          f"in {wall:.0f} s; report {out / 'REPORT.md'}")
    for r in bad:
        print(f"  {r['level']}: {r['outcome']}: {r['detail']}")
    if not args.keep:
        launch.safe_rmtree(out / "work")
    return exit_code(results, len(levels))


if __name__ == "__main__":
    sys.exit(main())
