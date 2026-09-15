"""gameplayer.py - vendored from Dali's own app: C:\\Users\\Dali\\Documents\\Projects\\macro_recorder_app

GamePlayer drives games via low-level Win32 user32.mouse_event / keybd_event
(SendInput-level injection that reaches DirectX games where pyautogui/pynput
controller clicks get ignored), with precise loop_start-relative timestamps and
configurable keystroke retries. Recording format = macro_recorder_app's JSON:
events [{type: move|click|scroll|key, timestamp, ...}].

Kept byte-for-byte compatible with the original (imports are self-contained,
config constants inlined at the bottom).
"""
import threading
import time
import ctypes
from pynput import keyboard

user32 = ctypes.windll.user32

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_WHEEL = 0x0800

MOUSE_BUTTONS = {
    'left':    (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
    'right':   (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
    'middle':  (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
}

VK = {
    'backspace': 0x08, 'tab': 0x09, 'clear': 0x0C,
    'enter': 0x0D, 'return': 0x0D,
    'shift': 0x10, 'ctrl': 0x11, 'alt': 0x12,
    'pause': 0x13, 'caps_lock': 0x14,
    'esc': 0x1B, 'escape': 0x1B,
    'space': 0x20, 'page_up': 0x21, 'page_down': 0x22,
    'end': 0x23, 'home': 0x24,
    'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28,
    'print_screen': 0x2C, 'insert': 0x2D, 'delete': 0x2E, 'del': 0x2E,
    'multiply': 0x6A, 'add': 0x6B, 'separator': 0x6C,
    'subtract': 0x6D, 'decimal': 0x6E, 'divide': 0x6F,
    'f1': 0x70, 'f2': 0x71, 'f3': 0x72, 'f4': 0x73,
    'f5': 0x74, 'f6': 0x75, 'f7': 0x76, 'f8': 0x77,
    'f9': 0x78, 'f10': 0x79, 'f11': 0x7A, 'f12': 0x7B,
    'f13': 0x7C, 'f14': 0x7D, 'f15': 0x7E, 'f16': 0x7F,
    'f17': 0x80, 'f18': 0x81, 'f19': 0x82, 'f20': 0x83,
    'scroll_lock': 0x91,
    'shift_r': 0xA0, 'ctrl_r': 0xA1, 'alt_r': 0xA2,
    'shift_l': 0xA0, 'ctrl_l': 0xA1, 'alt_l': 0xA2,
    'numpad0': 0x60, 'numpad1': 0x61, 'numpad2': 0x62,
    'numpad3': 0x63, 'numpad4': 0x64, 'numpad5': 0x65,
    'numpad6': 0x66, 'numpad7': 0x67, 'numpad8': 0x68, 'numpad9': 0x69,
}

DEFAULT_PLAYBACK_STOP_KEY = 'esc'
GAME_KEYBOARD_RETRY_COUNT = 3
GAME_KEYBOARD_RETRY_DELAY = 0.015


def _precise_wait(target_ts, loop_start):
    target = loop_start + target_ts
    remaining = target - time.perf_counter()
    if remaining > 0.008:
        time.sleep(remaining - 0.003)
    while time.perf_counter() < target:
        pass


def _get_vk(key_str):
    if len(key_str) == 1:
        if 'a' <= key_str <= 'z':
            return ord(key_str.upper())
        return ord(key_str)
    return VK.get(key_str, 0)


def _find_game_rect(proc="age3y.exe"):
    """Current visible game window rect (l, t, r, b) or None - used to map
    recorded absolute clicks into the window wherever it is NOW."""
    import subprocess
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {proc}", "/FO", "CSV", "/NH"],
            text=True,
        ).strip()
    except Exception:
        return None
    if not out or proc.lower() not in out.lower():
        return None
    pid = int(out.split(",")[1].strip('"'))
    hwnds = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(h, lp):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            hwnds.append(h)
        return True

    user32.EnumWindows(cb, 0)
    if not hwnds:
        return None
    r = wintypes.RECT()
    user32.GetWindowRect(hwnds[0], ctypes.byref(r))
    return (r.left, r.top, r.right, r.bottom)


def map_point(x, y, rec_rect, cur_rect):
    """Map a recorded ABSOLUTE screen coordinate into the CURRENT game window
    frame. rec_rect/cur_rect = (left, top, right, bottom).

    The recording is made with the game at SOME geometry on SOME screen
    (Dali: 'the game is not consistent, you are not in the same place each
    time' - so save the location and click there). Replay normalizes:
    fraction inside the recorded window -> fraction inside the current
    window -> current absolute coordinate. Missing rects => input unchanged.
    """
    if not rec_rect or not cur_rect:
        return x, y
    rl, rt, rr, rb = rec_rect
    cl, ct, cr, cb = cur_rect
    rw, rh = max(1, rr - rl), max(1, rb - rt)
    cw, ch = max(1, cr - cl), max(1, cb - ct)
    fx = (x - rl) / rw
    fy = (y - rt) / rh
    # clamp into the target window so clicks can never land on another screen
    fx = max(0.0, min(1.0, fx))
    fy = max(0.0, min(1.0, fy))
    return int(cl + fx * cw), int(ct + fy * ch)


class GamePlayer:
    def __init__(self, stop_key=None, callbacks=None):
        self.stop_key = (stop_key or DEFAULT_PLAYBACK_STOP_KEY).lower()
        self.callbacks = callbacks or {}
        self.stop_flag = False
        self.play_thread = None
        self.listener = None
        self.retry_count = GAME_KEYBOARD_RETRY_COUNT
        self.retry_delay = GAME_KEYBOARD_RETRY_DELAY
        self._last_x = None
        self._last_y = None
        self.rec_rect = None      # geometry the recording was made at
        self.cur_rect = None      # geometry at replay time (mapped target)

    def set_stop_key(self, key):
        self.stop_key = key.lower()

    def _map(self, x, y):
        """Apply geometry normalization if the recording stamped its rect."""
        return map_point(x, y, self.rec_rect, self.cur_rect)

    def _on_press(self, key):
        try:
            ch = key.char
            if ch is not None:
                key_str = ch.lower()
            else:
                key_str = str(key).replace('Key.', '').lower()
        except AttributeError:
            key_str = str(key).replace('Key.', '').lower()
        if key_str == self.stop_key:
            self.stop_flag = True
            return False

    def _move_relative(self, x, y):
        if self._last_x is not None:
            dx = int(x - self._last_x)
            dy = int(y - self._last_y)
            if dx != 0 or dy != 0:
                user32.mouse_event(MOUSEEVENTF_MOVE, dx, dy, 0, 0)
        self._last_x = x
        self._last_y = y

    def _replay_event(self, event):
        t = event.get('type')
        x = event.get('x')
        y = event.get('y')
        if x is not None and y is not None:
            x, y = self._map(x, y)

        if t == 'move':
            self._move_relative(x, y)

        elif t == 'click':
            if x is not None and y is not None:
                self._move_relative(x, y)
            btn = event.get('button', 'left')
            pair = MOUSE_BUTTONS.get(btn)
            if pair:
                if event['action'] == 'press':
                    user32.mouse_event(pair[0], 0, 0, 0, 0)
                else:
                    user32.mouse_event(pair[1], 0, 0, 0, 0)

        elif t == 'scroll':
            dy = event.get('dy', 0)
            if dy:
                user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, int(dy * 120), 0)

        elif t == 'key':
            vk = _get_vk(event.get('key', ''))
            if vk == 0:
                return
            if event['action'] == 'press':
                for _ in range(self.retry_count):
                    user32.keybd_event(vk, 0, 0, 0)
                    if self.retry_count > 1:
                        time.sleep(self.retry_delay)
            else:
                user32.keybd_event(vk, 0, 2, 0)

    def _load_meta(self, events):
        """Grab the geometry stamp event (type=meta) if the recording has one.
        Also snapshot where the game window is RIGHT NOW so clicks get mapped."""
        self.rec_rect = None
        self.cur_rect = _find_game_rect()
        for e in events:
            if e.get('type') == 'meta' and e.get('window_rect'):
                self.rec_rect = tuple(int(v) for v in e['window_rect'])
                break

    def _play_loop(self, events, loop):
        self.stop_flag = False
        self.listener = keyboard.Listener(on_press=self._on_press)
        self.listener.start()
        cb_start = self.callbacks.get('on_play_start')
        cb_progress = self.callbacks.get('on_play_progress')
        cb_complete = self.callbacks.get('on_play_complete')

        self._load_meta(events)

        if cb_start:
            cb_start(loop, len(events))

        try:
            for i in range(loop):
                if self.stop_flag:
                    break
                self._last_x = None
                self._last_y = None
                loop_start = time.perf_counter()
                for idx, event in enumerate(events):
                    if self.stop_flag:
                        break
                    ts = event.get('timestamp', event.get('delay', 0))
                    _precise_wait(ts, loop_start)
                    self._replay_event(event)
                    if cb_progress:
                        cb_progress(i + 1, loop, idx + 1, len(events))
        finally:
            if self.listener:
                self.listener.stop()
            if cb_complete:
                cb_complete(not self.stop_flag)

    def play(self, events, loop=1):
        """Start playback on a background thread (non-blocking)."""
        if self.play_thread and self.play_thread.is_alive():
            return
        self.play_thread = threading.Thread(
            target=self._play_loop, args=(events, loop), daemon=True
        )
        self.play_thread.start()

    def stop(self):
        self.stop_flag = True