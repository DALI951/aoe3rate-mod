"""e2e_aoe3.py - self-driving test harness for the aoe3rate-mod overlay.

Launches the REAL game (Program Files install with the mod DLL), replays Dali's
recorded macro (menu -> skirmish -> match), then LOOKS at the screen with
vision to confirm we are actually IN a match (the game does NOT always land in
the same menus - so we classify the screen and recover with Enter presses
instead of blind waiting), captures the game monitor via dxcam/mss, asks the
vision model whether the overlay panel is VISIBLE and reads its numbers + the
native top-right resource COUNTS (ground truth - the game calculates them
automatically), and compares with rates.log (must be LIVE, "count directly").

Modes:
    probe  - launch game, wait for main menu, capture + vision-report the menu
             screen (used to calibrate navigation). Does NOT start a match.
    record - launch game, record Dali's clicks/keys to recordings/skirmish.json
             (F6 = start/stop recording). THE RECORDING IS THE NAVIGATION PLAN.
    run    - full flow: launch + replay macro + vision-adaptive in-game check
             + SILENT GATHER wait (workers produce) + overlay verify + direct
             rates.log resource count + report JSON.

CLI:
    python e2e_aoe3.py probe
    python e2e_aoe3.py record
    python e2e_aoe3.py run [--iter N] [--ingame-sec 30] [--gather-sec 60]
                           [--timeout 300] [--no-vision] [--no-macro]
                           [--macro PATH]

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


def classify_screen(path):
    """Ask vision whether we are in a match (resource counters visible).
    Returns (in_game: bool, reply: str)."""

    def _ask(p):
        return vision(path, p)

    reply = _ask(
        "This is a screenshot of Age of Empires 3. Is the game IN A MATCH right "
        "now - meaning a real game screen with the resource counters top-right "
        "(Food/Wood/Coin/Export numbers), a minimap, and/or units/buildings on "
        "the map? Answer exactly: IN-GAME or NOT-IN-GAME. Then in one short "
        "line say what screen you actually see (main menu / skirmish setup / "
        "loading / victory or defeat / intro / desktop / other)."
    )
    if not reply:
        return None, ""
    in_game = "IN-GAME" in reply.upper()
    return in_game, reply


def recovery_press(first=False):
    """Menu-advance recovery: Enter often confirms/advances AoE3 menus.
    Returns nothing. Caller re-classifies after each press."""
    import ctrl

    try:
        ctrl.bring("enter")
    except SystemExit:
        print("  [recover] no game window to focus", file=sys.stderr)
    time.sleep(3)


def verify_log_alive(tail=5.0):
    """True if rates.log was written within `tail` seconds (DLL alive +
    export mode). Returns (alive, mtime_age, last_line)."""
    try:
        st = os.stat(RATES_LOG)
    except OSError:
        return False, None, ""
    age = time.time() - st.st_mtime
    last = ""
    try:
        with open(RATES_LOG, "r", errors="replace") as f:
            lines = [l for l in f.read().splitlines() if l.strip()]
        last = lines[-1] if lines else ""
    except OSError:
        pass
    return age <= tail, age, last


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


def pre_click_game(path=DEFAULT_MACRO):
    """Dali: 'add a click in the same 1st click before the space bar twice
    clicking so the mouse knows you are in the game'.

    The macro starts with SPACE presses before any mouse event - if the game
    window is not focused, those spaces go nowhere and everything after drifts
    (Dali: 'the game is not consistent, you are not in the same place each
    time'). So BEFORE replaying: focus the game window and inject a click at
    the recording's FIRST CLICK position (window-center if that spot is
    outside the current window rect - window position changes between runs).
    Injection via SetCursorPos + mouse_event (same low-level class the
    GamePlayer uses, reaches DirectX games).
    """
    if not os.path.isfile(path):
        print("[preclick] macro not found - skipping")
        return False
    try:
        with open(path, "r", encoding="utf-8") as f:
            events = json.load(f)
    except (OSError, ValueError) as e:
        print(f"[preclick] cannot read macro: {e}", file=sys.stderr)
        return False

    first_click = None
    for e in events:
        if e.get("type") == "click" and "x" in e and "y" in e:
            first_click = (int(e["x"]), int(e["y"]))
            break
    if first_click is None:
        print("[preclick] no click events in macro - skipping")
        return False

    w = find_game_window()
    if w is None:
        print("[preclick] no game window - skipping")
        return False
    l, t, r, b = w.rect
    cx, cy = window_center(w.rect)
    px, py = first_click
    inside = (l <= px < r) and (t <= py < b)
    tx, ty = (px, py) if inside else (cx, cy)
    print(f"[preclick] first click in macro at {first_click} "
          f"({'inside' if inside else 'OUTSIDE window -> center'}) "
          f"-> clicking ({tx},{ty})")

    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    win = w.hwnd
    user32.ShowWindow(win, 9)          # SW_RESTORE
    user32.SetForegroundWindow(win)
    time.sleep(0.5)
    user32.SetCursorPos(tx, ty)
    time.sleep(0.15)
    user32.mouse_event(0x0002, 0, 0, 0, 0)   # LEFT DOWN
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)   # LEFT UP
    time.sleep(0.5)
    print("[preclick] game window focused + click injected")
    return True


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
        # Dali: the macro starts with space-bar presses - focus the game and
        # inject a click at the recording's first-click spot FIRST so the
        # game knows the mouse is in it before any key is pressed.
        if not args.no_preclick:
            pre_click_game(args.macro)
        else:
            print("[run] --no-preclick: skipping focus+click warm-up")
        player = play_macro(args.macro, loop=args.macro_loop,
                            stop_key=args.stop_key)
        if player is None:
            print("FAIL: no macro to replay. Run: python e2e_aoe3.py record")
            return 1

        # The macro drives the game through menus + the start of a match.
        # Wait for it to finish or abort via hotkey.
        t0 = time.time()
        while (time.time() - t0 < args.macro_timeout
               and player.play_thread and player.play_thread.is_alive()):
            time.sleep(1)
        if player.play_thread and player.play_thread.is_alive():
            print(f"[run] macro still running after {args.macro_timeout}s - "
                  "continuing with capture anyway (hotkey esc to stop it)")
        print(f"[run] macro phase done ({(time.time() - t0):.0f}s)")
    else:
        print("[run] --no-macro: game must already be in match")

    # ---- vision-guided verification (Dali: the game does NOT always land
    # ---- in the same place - so LOOK and recover instead of blind waiting)
    deadline = time.time() + args.ingame_sec
    in_game = False
    attempts = 0
    while time.time() < deadline:
        shot, mth = capture_best("run-check")
        if not shot:
            print("FAIL: no capture while checking")
            return 1
        attempts += 1
        if not args.no_vision:
            in_game, cls = classify_screen(shot)
            print(f"[run] check#{attempts}: IN-GAME={in_game} | {cls[:160]}")
            if in_game:
                break
            if cls and ("VICTORY" in cls.upper() or "DEFEAT" in cls.upper()
                        or "MAIN MENU" in cls.upper()):
                print("[run] reached an end state, recovery not useful")
                break
        else:
            # blind mode: trust the macro + elapsed time
            in_game = True
            break
        # not in game yet -> nudge the menu forward, re-look
        recovery_press()
    if not in_game:
        print(f"[run] never confirmed in-game after {attempts} checks "
              f"({args.ingame_sec}s)")
        report_fail(args, shot, "NOT-IN-GAME", cls, macro_tail_check())
        return 1

    # ---- SILENT GATHER (Dali: workers need silence to gather) ----
    # No input at all: let the settlers/units produce so the direct counts
    # and instant rates have real movement before the visibility snapshot.
    if args.gather_sec > 0:
        print(f"[run] in-game confirmed - silent gather {args.gather_sec}s "
              f"(no input, workers produce...)")
        time.sleep(args.gather_sec)

    # ---- overlay report: rates.log must be LIVE and count directly (Dali)
    alive, age, last_line = verify_log_alive()
    if not alive:
        print(f"[run] WARNING: rates.log stale ({age:.0f}s) - DLL not writing?")
    else:
        print(f"[run] rates.log LIVE (age {age:.0f}s): {last_line[:80]}")

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
            "The game itself shows Live Resource COUNTS in the top-right corner "
            "(Food / Wood / Coin / Export numbers). "
            "QUESTION 1: quote the EXACT current numbers of those native top-right "
            "counters (they are the ground truth - the game calculates them "
            "automatically). "
            "QUESTION 2: is there a small SEMI-TRANSPARENT OVERLAY PANEL drawn ON "
            "TOP of the game showing resource RATES - a box in a corner with lines "
            "like 'Food +12/min' or similar that is NOT standard AoE3 UI? Answer "
            "OVERLAY-VISIBLE or NO-OVERLAY. If an overlay exists, quote ALL its "
            "text/numbers EXACTLY and say where it is on screen."
        ))
        if reply:
            overlay_text = reply
            overlay_visible = "OVERLAY-VISIBLE" in reply.upper()
            print(f"[run] OVERLAY VISION: {reply}")

    # rates.log: direct current values (count them directly - Dali)
    log_data = ""
    cur_values = {}
    try:
        with open(RATES_LOG, "r", errors="replace") as f:
            lines = [l.strip() for l in f if l.strip()]
        log_data = lines[-5:] if lines else []
        if lines:
            last = lines[-1]
            for part in last.split(","):
                if "=" in part:
                    k, v = part.split("=", 1)
                    if k in ("food", "wood", "coin", "export"):
                        try:
                            cur_values[k] = int(v)
                        except ValueError:
                            pass
    except OSError as e:
        print(f"[run] rates.log unreadable: {e}", file=sys.stderr)

    report = {
        "iter": args.iter,
        "in_game_confirmed": True,
        "overlay_visible": overlay_visible,
        "overlay_text": overlay_text,
        "resources_direct": cur_values,
        "rates_log_live": alive,
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


def report_fail(args, shot, reason, vision_reply, log_note):
    """Write a FAIL report JSON for the not-in-game / stuck paths."""
    report = {
        "iter": args.iter,
        "in_game_confirmed": False,
        "reason": reason,
        "vision_reply": vision_reply,
        "shot": os.path.basename(shot) if shot else None,
        "macro": os.path.basename(args.macro) if not args.no_macro else None,
        "log_note": log_note,
        "game_pid": proc_pid(),
    }
    rp = os.path.join(SHOT_DIR, f"report-{time.strftime('%H%M%S')}.json")
    with open(rp, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print("[run] FAIL REPORT:", json.dumps(report, indent=2, ensure_ascii=False))


def macro_tail_check():
    """Snapshot of the last rates.log line for fail reports."""
    try:
        with open(RATES_LOG, "r", errors="replace") as f:
            lines = [l for l in f.read().splitlines() if l.strip()]
        return lines[-1] if lines else "no lines"
    except OSError as e:
        return f"unreadable: {e}"


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
    r.add_argument("--ingame-sec", type=int, default=30,
                   help="max seconds spent LOOKING for in-game (direct mode "
                        "shows numbers immediately - no 90s warmup wait)")
    r.add_argument("--gather-sec", type=int, default=60,
                   help="silent no-input wait AFTER in-game is confirmed so "
                        "workers gather real resources (Dali)")
    r.add_argument("--timeout", type=int, default=300)
    r.add_argument("--no-vision", action="store_true")
    r.add_argument("--no-macro", action="store_true",
                   help="skip replaying the recorded macro (game must be ready)")
    r.add_argument("--no-preclick", action="store_true",
                   help="skip the focus+first-click pre-click (normally done "
                        "before the macro so spaces land in the game)")
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