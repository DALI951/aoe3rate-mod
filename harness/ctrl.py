"""ctrl.py - focus the aoE3 window and send a key/click. Usage:
    python ctrl.py bring [key]     - foreground the game window, optional key
    python ctrl.py click X Y       - click at absolute screen coords
"""
import ctypes
import sys
import time
from ctypes import wintypes

import pyautogui

user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
GetWindowTextW = user32.GetWindowTextW
GetWindowThreadProcessId = user32.GetWindowThreadProcessId
IsWindowVisible = user32.IsWindowVisible
SetForegroundWindow = user32.SetForegroundWindow
ShowWindow = user32.ShowWindow
SW_RESTORE = 9

import subprocess


def proc_pid():
    out = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq age3y.exe", "/FO", "CSV", "/NH"],
        text=True,
    ).strip()
    if not out or "age3y.exe" not in out.lower():
        return None
    return int(out.split(",")[1].strip('"'))


def find_game_window():
    pid = proc_pid()
    if pid is None:
        return None
    hwnds = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(h, lp):
        p = wintypes.DWORD()
        GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and IsWindowVisible(h):
            hwnds.append(h)
        return True

    EnumWindows(cb, 0)
    if not hwnds:
        return None
    hwnd = hwnds[0]
    buf = ctypes.create_unicode_buffer(512)
    GetWindowTextW(hwnd, buf, 512)
    return hwnd, buf.value


def bring(key=None):
    w = find_game_window()
    if w is None:
        print("no game window")
        sys.exit(1)
    hwnd, title = w
    ShowWindow(hwnd, SW_RESTORE)
    SetForegroundWindow(hwnd)
    time.sleep(0.6)
    print(f"focused: {title} (hwnd={hwnd})")
    if key:
        pyautogui.keyDown(key)
        time.sleep(0.05)
        pyautogui.keyUp(key)
        time.sleep(0.2)
        print(f"sent key: {key}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "bring":
        bring(sys.argv[2] if len(sys.argv) > 2 else None)
    elif cmd == "click":
        x, y = int(sys.argv[2]), int(sys.argv[3])
        pyautogui.click(x, y)
        print(f"clicked {x},{y}")
    else:
        print("usage: ctrl.py bring [key] | click X Y")
        sys.exit(1)