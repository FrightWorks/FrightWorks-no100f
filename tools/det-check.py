"""Determinism check: run the game twice on the virtual clock and compare guest memory frame by frame.
Python 3.8+, Windows and Linux, standard library only.

    python tools/det-check.py [--boot i004|title] [--retraces 1200] [--every 30] [--input "8000:A:200"] [--input-b S] [--dump R,...] [--aspect-mode N] [--fps30] [--whole-memory]
                              [--play-a DIR] [--play-b DIR] [--disc DIR] [--keep] [--skip-build-check] [--timeout 180]

Each run is the deterministic mode (NO100F_DETERMINISTIC=1, wiicompiled/runtime/include/det_clock.h) with the guest
memory fingerprinted every --every retraces (NO100F_DET_HASH, det_hash.h; each thread's stack below its stack pointer
is left out, see --whole-memory), from its own copy of the game (testkit/launch.py:
rumble off, muted, controller isolation, own config), booted straight into --boot, with --input as the scripted pad
(NO100F_INPUT_SCRIPT, times in ms of virtual time from boot). Both runs go at once, each in a shared run slot.

  same build twice (default)      the game itself is deterministic: the base the other use needs
  --input-b SCRIPT                a control: run b gets other input, so the result must be "different"
  --play-a DIR --play-b DIR       two builds (each a built Play folder) that should behave the same, such as a change
                                  behind an option that is off: any difference is a change in behaviour

Output: autotest/det-<time>/ with REPORT.md, the two hash files and the logs.
Exit code 0 identical, 1 different, 2 wrong build, 3 not run (a run crashed or never got far enough).
"""
import argparse
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from testkit import dethash, launch, slots  # noqa: E402
from testkit.launch import BuildMismatch, GameCrashed, REPO  # noqa: E402

DEFAULT_INPUT = "none"  # pad 1 connected, nothing pressed


class Run:
    def __init__(self, name, args, out, play, pad_input):
        self.name, self.args, self.out = name, args, out
        self.pad_input = pad_input
        # Memory images go next to the hash file under fixed names: one folder per run keeps a and b apart.
        self.hash_file = (out / name if args.dump else out) / f"{name}.hash"
        self.game = launch.TestGame(
            name, out / "work" / name, boot=None if args.boot == "title" else args.boot, hd=False, src_play=play, disc_data=args.disc, dev_tools=False,
            build_check=not args.skip_build_check and play is None, fps60=not args.fps30,
            extra_env={"NO100F_DETERMINISTIC": "1",
                       "NO100F_DET_HASH": f"{self.hash_file}:{args.every}",
                       "NO100F_DET_HASH_SKIP_DEAD_STACK": "0" if args.whole_memory else "1",
                       "NO100F_INPUT_SCRIPT": pad_input,
                       **({"NO100F_DET_DUMP": args.dump} if args.dump else {})})
        self.error = None
        self.reached = -1

    def run(self, pool):
        try:
            with pool.acquire(f"det-check {self.name}", root=str(REPO)):
                self.game.check_build()
                self.game.prepare()
                if self.args.aspect_mode is not None:
                    set_aspect_mode(self.game.play / "UserData" / "Config.toml", self.args.aspect_mode)
                self.game.start()
                end = time.time() + self.args.timeout
                while time.time() < end:
                    self.game.check_crash()
                    self.reached = dethash.last_retrace(self.hash_file)
                    if self.reached >= self.args.retraces:
                        break
                    time.sleep(0.5)
                self.reached = dethash.last_retrace(self.hash_file)
                if self.reached < self.args.retraces:
                    self.error = f"timed out at retrace {self.reached} of {self.args.retraces}"
        except (GameCrashed, BuildMismatch) as e:
            self.error = e
        except Exception as e:  # report, never hang the other run
            self.error = f"{type(e).__name__}: {e}"
        finally:
            self.game.stop()


def set_aspect_mode(cfg, mode):
    """The kit's test config is 16:9 (aspect_mode = 1); --aspect-mode replaces that line."""
    text = cfg.read_text(encoding="utf-8")
    if "aspect_mode = 1\n" not in text:
        raise RuntimeError(f"{cfg}: no 'aspect_mode = 1' line to replace")
    cfg.write_text(text.replace("aspect_mode = 1\n", f"aspect_mode = {mode}\n", 1), encoding="utf-8")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--boot", default="i004", help="level to boot straight into, or 'title' for the game's own title menu")
    p.add_argument("--retraces", type=int, default=1200, help="compare up to this retrace (60 a second)")
    p.add_argument("--every", type=int, default=30, help="fingerprint every N retraces")
    p.add_argument("--input", default=DEFAULT_INPUT, help="NO100F_INPUT_SCRIPT for both runs")
    p.add_argument("--input-b", help="a different script for run b: a control that must come out different")
    p.add_argument("--dump", metavar="RETRACES",
                   help="also write each run's mem1/mem2 images at these retraces (comma list), with the hash files in a/ and b/")
    p.add_argument("--aspect-mode", type=int, choices=(0, 1, 2, 3),
                   help="[video] aspect_mode for both runs (0 4:3, 1 16:9 = the kit's default, 2 fill, 3 cinematic)")
    p.add_argument("--fps30", action="store_true", help="[video] fps60 = false")
    p.add_argument("--whole-memory", action="store_true",
                   help="hash each thread's unused stack too (left out by default: builds leave different bytes there)")
    p.add_argument("--play-a", type=Path, help="built Play folder of run a (default: this checkout's Play)")
    p.add_argument("--play-b", type=Path, help="built Play folder of run b (default: same as a)")
    p.add_argument("--disc", type=Path, help="extracted disc (default: this checkout's Disc/DATA)")
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--slots", type=int)
    p.add_argument("--keep", action="store_true", help="keep the Play copies")
    p.add_argument("--skip-build-check", action="store_true")
    args = p.parse_args(argv)

    out = REPO / "autotest" / time.strftime("det-%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    play_b = args.play_b or args.play_a
    runs = [Run("a", args, out, args.play_a, args.input), Run("b", args, out, play_b, args.input_b or args.input)]
    pool = slots.SlotPool(slots=slots.slot_count(args.slots))
    threads = [threading.Thread(target=r.run, args=(pool,)) for r in runs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    report = [f"# Determinism check {out.name}", "",
              f"boot `{args.boot}`, {'30' if args.fps30 else '60'} fps, "
              f"up to retrace {args.retraces}, every {args.every}", ""]
    for r in runs:
        src = (r.game.src_play if r.game.src_play else REPO / "Play")
        report.append(f"- run {r.name}: {src}, input `{r.pad_input}`, reached retrace {r.reached}"
                      + (f", **{r.error}**" if r.error else ""))
    a, b = (dethash.read(r.hash_file) for r in runs)
    a = [l for l in a if l.retrace <= args.retraces]
    b = [l for l in b if l.retrace <= args.retraces]
    cmp = dethash.compare(a, b)
    if any(isinstance(r.error, BuildMismatch) for r in runs):
        code, verdict = 2, "wrong build"
    elif cmp.first_diff is not None:
        code, verdict = 1, "DIFFERENT: " + cmp.describe()
    elif any(r.error for r in runs) or not cmp.identical:
        code, verdict = 3, "not run: " + cmp.describe()
    else:
        code, verdict = 0, "identical: " + cmp.describe()
    report += ["", f"**Result: {verdict}**", ""]
    (out / "REPORT.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    print(f"output: {out}")
    if not args.keep:
        for r in runs:
            launch.safe_rmtree(r.game.play)
    return code


if __name__ == "__main__":
    sys.exit(main())
