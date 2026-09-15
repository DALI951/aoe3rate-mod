"""harness smoke tests (no game launches, no network) - run cheaply."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import e2e_aoe3 as e2e  # noqa: E402


def test_proc_pid_returns_none_or_int():
    pid = e2e.proc_pid("definitely_not_a_real_process_xyz.exe")
    assert pid is None, f"expected None, got {pid!r}"
    print("PASS proc_pid: unknown process -> None")


def test_rates_log_constants():
    assert e2e.GAME_DIR.endswith("Age of Empires III - Complete Collection")
    assert e2e.EXE.lower().endswith("age3y.exe")
    assert os.path.isdir(e2e.GAME_DIR), "real game dir missing"
    assert os.path.exists(e2e.EXE), "age3y.exe missing"
    print("PASS constants: real game dir + exe exist")


def test_window_center():
    c = e2e.window_center((0, 0, 1600, 900))
    assert c == (800, 450), c
    print("PASS window_center: (800,450)")


def test_capture_monitor_fallback_shape():
    import time
    p = os.path.join(SHOT_DIR := os.path.join(HERE, "screens"), "smoke-t.png")
    os.makedirs(SHOT_DIR, exist_ok=True)
    if os.path.exists(p):
        os.remove(p)
    m = e2e.capture_monitor(p, 1)
    assert m in ("dxcam", "mss", None), m
    if m:
        from PIL import Image
        img = Image.open(p)
        assert img.size[0] > 0 and img.size[1] > 0
        for _ in range(5):  # dxcam may briefly hold the file
            try:
                os.remove(p)
                break
            except PermissionError:
                time.sleep(1)
        print(f"PASS capture_monitor: {m} {img.size}")
    else:
        print("SKIP capture_monitor: both backends unavailable")


def test_macro_roundtrip():
    import json as _json
    import tempfile
    from gameplayer import GamePlayer, _get_vk, map_point
    from recorder_dali import Recorder

    assert _get_vk("a") == ord("A")
    assert _get_vk("enter") == 0x0D
    assert _get_vk("f6") == 0x75
    print("PASS _get_vk: chars + named keys")

    # geometry mapping: recorded window -> moved window keeps relative spot
    # record window 1920x1080 at (0,0); click at (960,540) = dead center
    m = map_point(960, 540, (0, 0, 1920, 1080), (640, 288, 1280, 768))
    assert m == (960, 528), f"window-center maps to window-center, got {m}"
    # top-left of the recorded window -> top-left of the new window
    m = map_point(0, 0, (0, 0, 1920, 1080), (640, 288, 1280, 768))
    assert m == (640, 288), f"top-left maps to top-left, got {m}"
    # bottom-right maps into (not past) the new window
    m = map_point(1919, 1079, (0, 0, 1920, 1080), (640, 288, 1280, 768))
    assert m[0] < 1280 and m[1] < 768, f"clamped inside window, got {m}"
    # missing rects => passthrough
    assert map_point(100, 200, None, (0, 0, 100, 100)) == (100, 200)
    print("PASS map_point: geometry normalization + clamp + passthrough")

    rec = Recorder(record_hotkey="f6")
    assert rec.record_hotkey == "f6"
    rec.start_recording()  # F6 press in the real flow -> stamps geometry meta
    assert rec.recording and rec.meta, "recording start stamps geometry meta"
    # simulate a synthetic event directly (listeners can't be fed easily)
    rec.events.append({"type": "key", "key": "enter", "action": "press",
                       "timestamp": 0.1})
    rec.events.append({"type": "click", "x": 100, "y": 200, "button": "left",
                       "action": "press", "timestamp": 0.2})
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.json")
        rec.save(p)
        with open(p) as f:
            ev = _json.load(f)
        assert len(ev) == 3 and ev[0]["type"] == "meta", \
            "recording stamps geometry first (meta event)"
        assert "window_rect" in ev[0], "meta carries the window_rect field"
        gp = GamePlayer(stop_key="esc")
        assert gp.stop_key == "esc"
        gp._load_meta(ev)
        # no game running here -> rect None -> map_point passthrough (old coords)
        assert gp.rec_rect is None, f"rec_rect None without game, got {gp.rec_rect}"
        assert gp._map(100, 200) == (100, 200), "passthrough without geometry"
    print("PASS macro roundtrip: meta-stamped record -> JSON -> GamePlayer")


if __name__ == "__main__":
    test_proc_pid_returns_none_or_int()
    test_rates_log_constants()
    test_window_center()
    test_capture_monitor_fallback_shape()
    test_macro_roundtrip()
    print("HARNESS-SMOKE: PASS")