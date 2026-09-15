"""e2e_aoe3.py - self-driving test harness for the aoe3rate-mod overlay.

Launches the REAL game (Program Files install with the mod DLL), walks the menu
to a skirmish with keyboard navigation, waits in-match, captures the game
monitor via dxcam/mss, asks the vision model whether the overlay panel is
VISIBLE and reads its numbers, then compares with rates.log.

Modes:
    probe  - launch game, wait for main menu, capture + vision-report the menu
             screen (used to calibrate navigation). Does NOT start a match.
    run    - full flow: probe + navigate to skirmish + start match + wait
             --ingame-sec + overlay check + rates.log compare + report JSON.

CLI:
    python e2e_aoe3.py probe
    python e2e_aoe3.py run [--iter N] [--ingame-sec 120] [--timeout 300] [--no-vision]

Output: screens/<mode>-<ts>...png + report-<ts>.json printed + saved.
Exit code 0 => PASS, 1 => FAIL (or aborted step), 2 => unrecoverable.
"""
import argparse
import json
import os
import subprocess
import sys
import time

import pyautogui

pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.15
HERE = os.path.dirname(os.path.abspath(__file__))
GAME_DIR = r"C:\Program Files (x86)\Age of Empires III - Complete Collection"
EXE = os.path.join(GAME_DIR, "age3y.exe")
RATES_LOG = os.path.join(GAME_DIR, "rates.log")
SHOT_DIR = os.path.join(HERE, "screens")
REC_DIR = os.path.join(HERE, "recordings")
DEFAULT_MACRO = os.path.join(REC_DIR, "skirmish.json")
WIN_TITLES = ["Age of Empires 3", "Age of Empires III", "age3y"]

os.makedirs(SHOT_DIR, exist_ok=True)
os.makedirs(REC_DIR, exist_ok=True)

# ---------------------------------------------------------------------------
# window / capture helpers
# ---------------------------------------------------------------------------
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
GetWindowTextW = user32.GetWindowTextW
GetWindowRect = user32.GetWindowRect
IsWindowVisible = user32.IsWindowVisible
GetWindowThreadProcessId = user32.GetWindowThreadProcessId
SW_RESTORE = 9


def proc_pid(proc_name="age3y.exe"):
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {proc_name}", "/FO", "CSV", "/NH"],
            text=True,
        ).strip()
    except subprocess.CalledProcessError:
        return None
    if not out or proc_name.lower() not in out.lower():
        return None
    pid = out.split(",")[1].strip('"')
    return int(pid)


def find_game_window():
    """Return (hwnd, title, rect) of the visible top-level age3y window."""
    import collections
    Win = collections.namedtuple("Win", "hwnd title rect pid")

    found = []
    for pid in {proc_pid(), proc_pid("age3.exe")}:
        if pid is None:
            continue
        hwnds = []

        @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
        def cb(h, lp, _pid=pid, _hwnds=hwnds):
            p = wintypes.DWORD()
            GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value == _pid and IsWindowVisible(h):
                _hwnds.append(h)
            return True

        EnumWindows(cb, 0)
        for h in hwnds:
            buf = ctypes.create_unicode_buffer(512)
            GetWindowTextW(h, buf, 512)
            r = wintypes.RECT()
            GetWindowRect(h, ctypes.byref(r))
            found.append(Win(h, buf.value, (r.left, r.top, r.right, r.bottom), pid))
    if found:
        return max(found, key=lambda w: (w.rect[2] - w.rect[0]) * (w.rect[3] - w.rect[1]))
    return None


def window_center(rect):
    return ((rect[0] + rect[2]) // 2, (rect[1] + rect[3]) // 2)


def game_monitor_index(rect):
    """Which mss monitor index contains the window center? (1-based list.)"""
    try:
        import mss

        with mss.mss() as s:
            for i, mon in enumerate(s.monitors[1:], start=1):
                if (mon["left"] <= rect[0] < mon["left"] + mon["width"] and
                        mon["top"] <= rect[1] < mon["top"] + mon["height"]):
                    return i
    except Exception:
        pass
    return 1


def capture_monitor(path, mss_index):
    """dxcam first (real game frames), fallback mss. Returns method used."""
    try:
        import dxcam

        cam = dxcam.create(output_color="RGB")
        if cam is not None:
            frame = cam.grab()
            if frame is not None and frame.size > 0:
                from PIL import Image

                Image.fromarray(frame).save(path)
                cam.release()
                return "dxcam"
    except Exception as e:
        print(f"  [capture] dxcam failed: {e}", file=sys.stderr)
    try:
        import mss

        with mss.mss() as s:
            mon = s.monitors[mss_index]
            shot = s.grab(mon)
        from PIL import Image

        Image.frombytes("RGB", shot.size, shot.rgb).save(path)
        return "mss"
    except Exception as e:
        print(f"  [capture] mss failed: {e}", file=sys.stderr)
    return None


def capture_best(tag):
    """Capture the monitor the game window is on. Returns (path, method)."""
    w = find_game_window()
    idx = game_monitor_index(w.rect) if w else 1
    ts = time.strftime("%H%M%S")
    path = os.path.join(SHOT_DIR, f"{tag}-{ts}-m{idx}.png")
    method = capture_monitor(path, idx)
    if method is None:
        return None, None
    return path, method


def vision(screenshot, prompt):
    """Ask gemma-3-27b-it about a screenshot. Returns reply text or None."""
    try:
        r = subprocess.run(
            [sys.executable, os.path.join(HERE, "viseye.py"), screenshot, prompt],
            capture_output=True, text=True, timeout=150,
        )
        if r.returncode != 0:
            print(f"  [vision] err: {r.stderr[:200]}", file=sys.stderr)
            return None
        return r.stdout.strip()
    except Exception as e:
        print(f"  [vision] exception: {e}", file=sys.stderr)
        return None


def kill_game():
    pid = proc_pid()
    if pid is not None:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                       capture_output=True)
        time.sleep(2)


def launch_game():
    if proc_pid() is not None:
        kill_game()
    # rates.log marker (DLL truncates on thread start)
    try:
        with open(RATES_LOG, "w") as f:
            f.write("")
    except OSError as e:
        print(f"  [warn] cannot clear rates.log: {e}", file=sys.stderr)
    subprocess.Popen([EXE], cwd=GAME_DIR)
    t0 = time.time()
    while time.time() - t0 < 90:
        if game_window := find_game_window():
            return game_window, time.time() - t0
        time.sleep(1)
    return None, None


# ---------------------------------------------------------------------------
# keyboard navigation (AoE3 menus accept arrow keys + Enter)
# ---------------------------------------------------------------------------
def send_keys(*keys):
    for k in keys:
        pyautogui.keyDown(k)
        time.sleep(0.05)
        pyautogui.keyUp(k)
        time.sleep(0.2)


def wait_ingame(seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(1)


# ---------------------------------------------------------------------------
# macro recording / playback (Dali's macro_recorder App, vendored GamePlayer)
# ---------------------------------------------------------------------------
def record(args):
    """Launch the game, let Dali record his menu->skirmish->play macro with
    F6 (start) / F6 (stop), saving to recordings/skirmish.json.

    The recording IS the navigation plan: every future test run replays it.
    """
    from recorder_dali import Recorder

    win, dt = launch_game()
    if win is None:
        print("FAIL: game did not produce a window within 90s")
        return 1
    print(f"[record] window '{win.title}' after {dt:.0f}s")
    print("[record] >>> Dali: play the game now. Press F6 to START recording,")
    print("[record]     navigate: Single Player -> Skirmish -> start a match,")
    print("[record]     play ~60-90s (queue a couple of villagers/units),")
    print("[record]     then press F6 again to STOP. ESC aborts playback later.")
    rec = Recorder(record_hotkey="f6")
    rec.start_listeners()
    try:
        print("[record] Recording... (F6 = stop)")
        while True:
            time.sleep(0.5)
            if not rec.recording:
                if rec.events:
                    break
                time.sleep(1)
    except KeyboardInterrupt:
        print("\n[record] interrupted")
    finally:
        rec.stop_listeners()
    out = args.output or DEFAULT_MACRO
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rec.save(out)
    n = len(rec.events)
    print(f"[record] saved {n} events -> {out}")
    if n == 0:
        print("WARN: no events recorded (F6 never pressed?)")
        return 1
    print("NEXT: python e2e_aoe3.py run  (replays your recording every test)")
    return 0


def play_macro(path=DEFAULT_MACRO, loop=1, stop_key="esc"):
    """Replay a recorded macro with the vendored GamePlayer on a bg thread."""
    from gameplayer import GamePlayer

    if not os.path.isfile(path):
        print(f"[play] macro not found: {path}")
        return None
    with open(path, "r", encoding="utf-8") as f:
        events = json.load(f)
    print(f"[play] loaded {len(events)} events from {os.path.basename(path)} "
          f"(loop={loop}, stop={stop_key})")
    player = GamePlayer(stop_key=stop_key)
    player.play(events, loop=loop)
    return player


# ---------------------------------------------------------------------------
# probe mode
# ---------------------------------------------------------------------------
def probe(args):
    print("[probe] launching game ...")
    win, dt = launch_game()
    if win is None:
        print("FAIL: game did not produce a window within 90s")
        return 1
    print(f"[probe] window '{win.title}' rect={win.rect} after {dt:.0f}s (pid {win.pid})")
    time.sleep(4)
    path, method = capture_best("probe")
    if not path:
        print("FAIL: could not capture game monitor")
        return 1
    print(f"[probe] captured {os.path.basename(path)} via {method}")
    if not args.no_vision:
        reply = vision(path, (
            "This is a screenshot of a computer screen. What is visible? "
            "Is it the Age of Empires 3 main menu (buttons like Single Player, "
            "Multiplayer, Options)? A loading screen? An intro video? In-game? "
            "Desktop? Quote any readable text/buttons and describe their layout "
            "with approximate screen percentages (e.g. 'Single Player button at "
            "~45% width, ~40% height'). Be precise."
        ))
        if reply:
            print("[probe] VISION:", reply)
        else:
            print("[probe] no vision reply")
    return 0


# ---------------------------------------------------------------------------
# run mode
# ---------------------------------------------------------------------------
def run(args):
    print(f"[run] iteration {args.iter}: launching game ...")
    win, dt = launch_game()
    if win is None:
        print("FAIL: no game window within 90s")
        return 1
    print(f"[run] window '{win.title}' after {dt:.0f}s")
    time.sleep(4)
    screen0, m0 = capture_best("run0")
    if screen0:
        print(f"[run] step0 screen: {os.path.basename(screen0)} ({m0})")

    # The navigation IS Dali's recorded macro: replay it now to drive the
    # game from wherever it is (title screen -> menu -> skirmish -> match).
    if not args.no_macro:
        player = play_macro(args.macro, loop=args.macro_loop,
                            stop_key=args.stop_key)
        if player is None:
            print("FAIL: no macro to replay. Run: python e2e_aoe3.py record")
            return 1

        # The macro drives the game through menus + the start of a match
        # (in Dali's recordings the in-match segment runs ~60-90s). Wait for
        # it to finish or abort via hotkey; then keep gathering for the rest.
        t0 = time.time()
        while (time.time() - t0 < args.macro_timeout
               and player.play_thread and player.play_thread.is_alive()):
            time.sleep(1)
        if player.play_thread and player.play_thread.is_alive():
            print(f"[run] macro still running after {args.macro_timeout}s - "
                  "continuing with capture anyway (hotkey esc to stop it)")
        print(f"[run] macro phase done ({(time.time() - t0):.0f}s), "
              f"waiting in-game {args.ingame_sec}s ...")
        time.sleep(args.ingame_sec)
    else:
        print("[run] --no-macro: waiting directly (game must already be in match)")
        time.sleep(args.ingame_sec)

    shot, mth = capture_best("run-match")
    if not shot:
        print("FAIL: no capture at match end")
        return 1
    print(f"[run] match shot: {os.path.basename(shot)} ({mth})")

    overlay_visible = None
    overlay_text = ""
    if not args.no_vision:
        reply = vision(shot, (
            "You are looking at an Age of Empires 3 screenshot taken IN A MATCH. "
            "CRITICAL QUESTION: is there a small SEMI-TRANSPARENT OVERLAY PANEL "
            "drawn ON TOP of the game showing resource RATES - typically a box in "
            "a corner with lines like 'Food +12/min' or 'food rate x' etc - that "
            "is NOT standard AoE3 UI? Answer: OVERLAY-VISIBLE or NO-OVERLAY. "
            "Then: 1) describe the top-right resource counters if visible and "
            "quote exact numbers; 2) if an overlay panel exists, quote ALL text/"
            "numbers in it EXACTLY; 3) describe where in the screen it is."
        ))
        if reply:
            overlay_text = reply
            overlay_visible = "OVERLAY-VISIBLE" in reply.upper()
            print(f"[run] OVERLAY VISION: {reply}")

    # rates.log compare
    log_data = ""
    try:
        with open(RATES_LOG, "r") as f:
            lines = [l.strip() for l in f if l.strip()]
        log_data = lines[-5:] if lines else []
    except OSError as e:
        print(f"[run] rates.log unreadable: {e}", file=sys.stderr)

    report = {
        "iter": args.iter,
        "overlay_visible": overlay_visible,
        "overlay_text": overlay_text,
        "rates_log_tail": log_data,
        "shot": os.path.basename(shot),
        "macro": os.path.basename(args.macro) if not args.no_macro else None,
        "game_pid": proc_pid(),
    }
    rp = os.path.join(SHOT_DIR, f"report-{time.strftime('%H%M%S')}.json")
    with open(rp, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print("[run] REPORT:", json.dumps(report, indent=2, ensure_ascii=False))
    if overlay_visible is True:
        print("RESULT: PASS (overlay visible)")
        return 0
    print("RESULT: FAIL (no overlay seen or no vision)")
    return 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("probe")
    p.add_argument("--no-vision", action="store_true")
    rc = sub.add_parser("record")
    rc.add_argument("-o", "--output", default=None,
                    help="macro output path (default recordings/skirmish.json)")
    r = sub.add_parser("run")
    r.add_argument("--iter", type=int, default=1)
    r.add_argument("--ingame-sec", type=int, default=120)
    r.add_argument("--timeout", type=int, default=300)
    r.add_argument("--no-vision", action="store_true")
    r.add_argument("--no-macro", action="store_true",
                   help="skip replaying the recorded macro (game must be ready)")
    r.add_argument("--macro", default=DEFAULT_MACRO,
                   help="macro JSON to replay (default recordings/skirmish.json)")
    r.add_argument("--macro-loop", type=int, default=1)
    r.add_argument("--macro-timeout", type=int, default=240,
                   help="seconds to wait for the macro to finish")
    r.add_argument("--stop-key", default="esc")
    args = ap.parse_args()
    try:
        if args.mode == "probe":
            code = probe(args)
        elif args.mode == "record":
            code = record(args)
        else:
            code = run(args)
    except KeyboardInterrupt:
        print("ABORTED by user")
        code = 2
    finally:
        # leave the game running for Dali, don't kill
        pass
    sys.exit(code)


if __name__ == "__main__":
    main()