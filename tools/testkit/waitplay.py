"""Wait until the game is playable: the one routine for every test run (ported from tools/debug/texhunt/look.ps1, which
had its own copy). Replaces fixed waits and blind button presses. Python 3.8+, standard library only.

Uses the dev menu command `cutscene` ("cutscene yes|no control yes|no"):
  * cutscene playing        -> press skip (A) every 2 s; the game takes a skip only after a few seconds
  * no cutscene, no control -> a camera pan or a dialogue box. Nothing is pressed for the first DIALOGUE_AFTER seconds (a pan
    ends by itself); after that A is pressed every DIALOGUE_EVERY seconds, at most DIALOGUE_MAX times in all (a dialogue box
    such as f003's "PRESS A TO CONTINUE" waits for it), and each press is logged and counted in the result
  * no cutscene, control    -> ready; start the checks at once
  * time limit              -> PlayTimeout with the last state and a screenshot
  * the game exits or logs a fault at any time -> GameCrashed at once (never retried)

Only a timeout gets a retry: `retry_on_timeout` runs the job a second time from a fresh launch and reports "passed on
retry" (h001 has a known intermittent intro hang). A failed check or a crash is final.
"""
import time

from .devmenu import DevMenuTimeout
from .launch import GameCrashed

SKIP_EVERY = 2.0
DIALOGUE_AFTER = 4.0   # seconds of "no cutscene, no control" before A is pressed for a dialogue box
DIALOGUE_EVERY = 2.0
DIALOGUE_MAX = 12      # presses in all: a stuck level still ends in a timeout


class PlayTimeout(Exception):
    def __init__(self, message, state="", screenshot=None):
        super().__init__(message)
        self.state, self.screenshot = state, screenshot


def wait_until_playable(game, boot_level=None, timeout=150.0, skip_button="a", screenshot=None, log=print,
                        boot_timeout=None, dialogue_after=DIALOGUE_AFTER, dialogue_max=DIALOGUE_MAX):
    """Returns {'seconds', 'presses', 'dialogue_presses', 'where'}. `dialogue_max` 0 = never press for a dialogue box. `timeout` is the whole time limit from now (boot included).
    `skip_button` None = never press (look.ps1 -SkipButton none). Raises PlayTimeout / GameCrashed."""
    t0 = time.time()
    deadline = t0 + timeout
    state = "waiting for the game to start"
    presses = 0
    dialogue = 0

    def fail(why):
        shot = None
        if screenshot:
            try:
                shot = str(screenshot) if game.screenshot(screenshot) else None
            except Exception:  # a failed picture must not hide the timeout
                shot = None
        raise PlayTimeout(f"{game.name}: not playable after {time.time() - t0:.0f} s: {why} (last state: {state})",
                          state, shot)

    try:
        if boot_level:
            state = f"waiting for {boot_level}.HIP to load"
            limit = min(deadline - time.time(), boot_timeout or timeout)
            if not game.wait_console(f"{boot_level}.HIP", limit):
                fail("the level never loaded")
        # in play at all? (the dev menu answers only in play; a camera pan can come first)
        state = "waiting for the dev menu to answer (not in play yet)"
        where = None
        while where is None:
            try:
                where = game.devmenu.command("where", timeout=min(10.0, max(deadline - time.time(), 0.5)))
            except DevMenuTimeout:
                if time.time() > deadline:
                    fail("the dev menu never answered")
        last_press = last_dialogue = 0.0
        no_control_since = None
        while True:
            game.check_crash()
            if time.time() > deadline:
                fail("no player control")
            try:
                playing, control = game.devmenu.cutscene(timeout=min(10.0, max(deadline - time.time(), 0.5)))
            except DevMenuTimeout:
                state = "the dev menu stopped answering"
                continue
            state = f"cutscene {'yes' if playing else 'no'}, control {'yes' if control else 'no'}"
            if not playing and control:
                break
            no_control_since = (no_control_since or time.time()) if not playing else None
            if (no_control_since and skip_button and dialogue < dialogue_max and time.time() - no_control_since >= dialogue_after
                    and time.time() - last_dialogue >= DIALOGUE_EVERY):
                game.devmenu.press(f"{skip_button} 10", timeout=10)
                dialogue += 1
                last_dialogue = time.time()
                log(f"{game.name}: no cutscene and no control for {time.time() - no_control_since:.0f} s: "
                    f"pressed {skip_button.upper()} for a dialogue box ({dialogue}/{dialogue_max})")
            if playing and skip_button and time.time() - last_press >= SKIP_EVERY:
                game.devmenu.press(f"{skip_button} 10 cutscene", timeout=10)
                presses += 1
                last_press = time.time()
            time.sleep(0.5 if playing else 1.0)
        where = game.devmenu.command("where", timeout=10)
    except DevMenuTimeout as e:
        fail(str(e))
    seconds = time.time() - t0
    log(f"{game.name}: playable after {seconds:.1f} s ({presses} skip press(es), {dialogue} dialogue press(es)): {where}")
    return {"seconds": seconds, "presses": presses, "dialogue_presses": dialogue, "where": where}


def wait_for_scene(game, scene, timeout=60.0, screenshot=None):
    """After a `scene X` command: wait until `where` names that scene (the old scene answers until the travel is done).
    Returns the where tuple; PlayTimeout / GameCrashed like wait_until_playable."""
    t0 = time.time()
    last = "no answer"
    while time.time() - t0 < timeout:
        game.check_crash()
        try:
            where = game.devmenu.where(timeout=5)
        except DevMenuTimeout:
            where = None
        if where:
            last = where[0]
            if where[0].lower() == scene.lower():
                return where
        time.sleep(0.5)
    shot = str(screenshot) if screenshot and game.screenshot(screenshot) else None
    raise PlayTimeout(f"{game.name}: still not in {scene} after {timeout:.0f} s (scene: {last})", last, shot)


def retry_on_timeout(run, log=print):
    """run(attempt) -> result, for attempt 1 and, after a PlayTimeout only, attempt 2 (it must launch afresh).
    Returns (outcome, result, notes): outcome 'passed' or 'passed on retry'. A second timeout raises the PlayTimeout;
    GameCrashed and every other exception pass through at once."""
    try:
        return "passed", run(1), []
    except PlayTimeout as first:
        log(f"retrying once after a timeout: {first}")
        result = run(2)
        return "passed on retry", result, [f"first attempt timed out: {first}"]
