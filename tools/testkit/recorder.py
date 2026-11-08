"""The per-job (smoke test) or per-level (sweep) record of checks: what was checked, how it came out, how long it took.
Python 3.8+, standard library only.

    rec = Recorder("h001", out_dir)
    rec.passed("build ID", "0.0.0 x@abc1234")
    rec.require(drawn >= 20, "HD textures drawn", f"{drawn} drawn")   # CheckFailed (and a FAILED line) when false
    rec.not_run("loop check", "no numpy")                              # never a pass
    rec.check_picture(game, "h001")                                    # a screenshot in out_dir/shots, not black

`rec.checks` is the list of (check, status, detail) the reports print; `rec.times` holds the numbers (seconds, fps).
"""
import threading

from . import checks as checkmod

print_lock = threading.Lock()


class CheckFailed(Exception):
    """A check of a job or level failed. `check` names it."""

    def __init__(self, message, check=""):
        super().__init__(message)
        self.check = check


class Recorder:
    def __init__(self, name, out):
        self.name, self.out = name, out
        self.checks = []        # (check, status, detail)
        self.times = {}
        self.notes = []

    def log(self, text):
        with print_lock:
            print(f"[{self.name}] {text}", flush=True)

    def passed(self, check, detail=""):
        self.checks.append((check, "passed", detail))
        self.log(f"ok    {check}{': ' + detail if detail else ''}")

    def not_run(self, check, why):
        self.checks.append((check, "not run", why))
        self.log(f"NOT RUN {check}: {why}")

    def fail(self, check, detail=""):
        self.checks.append((check, "FAILED", detail))
        raise CheckFailed(f"{check}: {detail}", check)

    def require(self, ok, check, detail=""):
        if not ok:
            self.fail(check, detail)
        self.passed(check, detail)

    def shot(self, game, label=""):
        """Save a picture of the game window as shots/<name>-<label>.png (shots/<name>.png without a label); None when the
        OS has no screenshot tool."""
        png = self.out / "shots" / (f"{self.name}-{label}.png" if label else f"{self.name}.png")
        return png if game.screenshot(png) else None

    def check_picture(self, game, label=""):
        png = self.shot(game, label)
        name = f"picture ({label})" if label else "picture"
        if png is None:
            self.not_run(name, "no screenshot tool on this OS")
            return
        lit, total = checkmod.png_pixels_lit(png)
        blank = checkmod.png_is_blank(png)
        self.require(not blank, name, f"{png.name}: {lit / total:.1%} of the pixels lit" + (" (black: only the dev overlay?)" if blank else ""))
