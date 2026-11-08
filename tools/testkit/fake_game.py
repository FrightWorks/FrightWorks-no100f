"""A windowless stand-in for the game, for the test kit's own tests (never a real run). It speaks the same files as the
game's dev menu: devmenu.txt -> devmenu.log, devmenu_press.txt, UserData/Logs/<time>/console.log, stderr.

Behaviour comes from the environment, FAKE_MODE:
  normal      a short camera pan (no control), then a cutscene that ends a moment after the first `a` press, then control
  hang        never in play: devmenu.txt is left alone, as the game does during the intro hang
  hang_once   like hang the first time (marker file in FAKE_STATE_DIR), normal afterwards
  crash       exits with code 3 after a moment
  fault       prints AccessViolation on stderr, keeps running
Other: FAKE_FPS (a frame rate: prints a `[vi] fps N:` line to stderr every half second once in play, as NO100F_LOG_FPS does),
FAKE_BOOT_DELAY (seconds before the game is in play), FAKE_BUILD_ID (printed to console.log), FAKE_CUTSCENE (0 = none),
FAKE_DIALOGUE (N: after the pan, no cutscene and no control until N plain `a` presses, as a "PRESS A TO CONTINUE" box),
FAKE_DEAD (1: `player` answers "dead/dying", as while Scooby is killed and the screen fades).
"""
import os
import re
import sys
import time
from pathlib import Path

mode = os.environ.get("FAKE_MODE", "normal")
state_dir = os.environ.get("FAKE_STATE_DIR")
if mode == "hang_once":
    marker = Path(state_dir) / "hung-once"
    if marker.exists():
        mode = "normal"
    else:
        marker.write_text("x")
        mode = "hang"

start = time.time()
boot = "title"
ini = Path("UserData/mods/zz-smoke/files/sd2.ini")
if ini.exists():
    m = re.search(r"(?m)^BOOT=(\w+)", ini.read_text(encoding="latin-1"))
    if m:
        boot = m.group(1)
logdir = Path("UserData/Logs") / time.strftime("%Y%m%d-%H%M%S")
logdir.mkdir(parents=True, exist_ok=True)
console = open(logdir / "console.log", "w", encoding="utf-8")
console.write(f"FrightWorks {os.environ.get('FAKE_BUILD_ID', '0.0.0 fake@0000000')}\n")
console.write(f"[dvdread] DATA\\files\\XX\\{boot}.HIP\n")
console.flush()
assert os.environ.get("NO100F_INPUT_SCRIPT"), "dummy pad script missing"
assert os.environ.get("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"), "controller isolation missing"
assert "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS" not in os.environ

boot_delay = float(os.environ.get("FAKE_BOOT_DELAY", "0.3"))
cutscene_wanted = os.environ.get("FAKE_CUTSCENE", "1") != "0"
scene, pos, snacks = boot if boot != "title" else "title", [0.0, 0.121, 0.0, 0.0], 0
pressed_a_at = None
presses = []
dialogue_needed = int(os.environ.get("FAKE_DIALOGUE", "0"))
dialogue_presses = 0
fake_fps = os.environ.get("FAKE_FPS")
last_fps_line = 0.0


def append(path, text):
    with open(path, "a", encoding="utf-8") as f:
        f.write(text)


def phase():
    t = time.time() - start - boot_delay
    if t < 0:
        return "loading"
    if t < 0.8:
        return "pan"
    if dialogue_presses < dialogue_needed:
        return "dialogue"
    if cutscene_wanted and (pressed_a_at is None or time.time() - pressed_a_at < 0.6):
        return "cutscene"
    return "play"


def in_play():
    return mode != "hang" and phase() != "loading"


while True:
    now = time.time() - start
    if mode == "crash" and now > 1.0:
        sys.exit(3)
    if mode == "fault" and now > 1.0 and not os.path.exists("fault-printed"):
        print("AccessViolation at 0x80001234", file=sys.stderr, flush=True)
        open("fault-printed", "w").close()
    if fake_fps and in_play() and time.time() - last_fps_line >= 0.5:
        print(f"[vi] fps {float(fake_fps):.1f}: game 1.00 ms (stand-in)", file=sys.stderr, flush=True)
        last_fps_line = time.time()
    if in_play():
        if os.path.exists("devmenu_press.txt"):
            for line in Path("devmenu_press.txt").read_text().splitlines():
                parts = line.split()
                if not parts:
                    continue
                if len(parts) >= 3 and parts[2] == "cutscene" and phase() != "cutscene":
                    answer = "skipped (no cutscene)"
                else:
                    answer = "queued"
                    if parts[0] == "a" and pressed_a_at is None:
                        pressed_a_at = time.time()
                    presses.append(parts[0])
                    if parts[0] == "a" and len(parts) < 3:
                        dialogue_presses += 1
                append("devmenu.log", f"press {line} -> {answer}\n")
            os.remove("devmenu_press.txt")
        if os.path.exists("devmenu.txt"):
            lines = Path("devmenu.txt").read_text().splitlines()
            os.remove("devmenu.txt")
            for l in lines:
                cmd, _, arg = l.partition(" ")
                if cmd == "where":
                    status = f"{scene} {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f} yaw {pos[3]:.1f}"
                elif cmd == "cutscene":
                    p = phase()
                    status = f"cutscene {'yes' if p == 'cutscene' else 'no'} control {'yes' if p == 'play' else 'no'}"
                elif cmd == "scene":
                    scene = arg.strip().lower()
                    status = f"travel to {scene}"
                elif cmd == "tp":
                    v = [float(x) for x in arg.split()]
                    pos[:len(v)] = v
                    status = "teleport"
                elif cmd == "snacks":
                    snacks = int(arg)
                    status = f"spendable snacks set to {snacks}"
                elif cmd == "player" and os.environ.get("FAKE_DEAD"):
                    status = "player unavailable: Player is dead/dying; resurrection is not supported"
                elif cmd == "player":
                    status = f"player health=3/5 snacks={snacks}\n  second line"
                else:
                    status = "unknown command"
                append("devmenu.log", f"{l} -> {status}\n")
    time.sleep(0.05)
