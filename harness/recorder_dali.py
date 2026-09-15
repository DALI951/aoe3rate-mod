"""recorder_dali.py - vendored from Dali's app macro_recorder_app (recorder.py).

Records mouse/keyboard into macro_recorder JSON format (timestamp-relative
events). Same callbacks/lock API as the original; kept self-contained.

Hotkey: F6 toggles recording on/off; use start_listeners()/stop_listeners()
for embedding (as the harness does).
"""
import json
import threading
import time
from pynput import mouse, keyboard

DEFAULT_RECORD_HOTKEY = 'f6'
MOVE_THROTTLE_INTERVAL = 0.02


class Recorder:
    def __init__(self, record_hotkey=None, callbacks=None):
        self.record_hotkey = (record_hotkey or DEFAULT_RECORD_HOTKEY).lower()
        self.callbacks = callbacks or {}
        self.recording = False
        self.entry_focused = False
        self.events = []
        self.start_time = 0
        self.last_time = 0
        self.last_move_time = 0
        self.last_pos = None
        self.mouse_listener = None
        self.keyboard_listener = None
        self.listeners_running = False
        self._lock = threading.Lock()

    def set_hotkey(self, hotkey):
        self.record_hotkey = hotkey.lower()

    def _key_to_str(self, key):
        try:
            ch = key.char
            if ch is not None:
                return ch.lower()
        except AttributeError:
            pass
        return str(key).replace('Key.', '').lower()

    def _key_repr(self, key):
        try:
            ch = key.char
            if ch is not None:
                return ch
        except AttributeError:
            pass
        return str(key).replace('Key.', '')

    def _record_event(self, event):
        now = time.perf_counter()
        event['timestamp'] = round(now - self.start_time, 4)
        self.last_time = now
        self.events.append(event)

    def _schedule_cb(self, name, *args):
        cb = self.callbacks.get(name)
        if cb:
            cb(*args)

    def on_move(self, x, y):
        if not self.recording:
            return
        now = time.perf_counter()
        if now - self.last_move_time < MOVE_THROTTLE_INTERVAL:
            return
        if self.last_pos == (x, y):
            return
        self.last_move_time = now
        self.last_pos = (x, y)
        self._record_event({'type': 'move', 'x': x, 'y': y})

    def on_click(self, x, y, button, pressed):
        if not self.recording:
            return
        button_name = str(button).replace('Button.', '')
        self._record_event({
            'type': 'click', 'x': x, 'y': y,
            'button': button_name,
            'action': 'press' if pressed else 'release'
        })

    def on_scroll(self, x, y, dx, dy):
        if not self.recording:
            return
        self._record_event({
            'type': 'scroll', 'x': x, 'y': y, 'dx': dx, 'dy': dy
        })

    def on_press(self, key):
        key_str = self._key_to_str(key)
        if key_str == self.record_hotkey and not self.entry_focused:
            self.toggle()
            return
        if self.recording:
            self._record_event({
                'type': 'key', 'key': self._key_repr(key), 'action': 'press'
            })

    def on_release(self, key):
        key_str = self._key_to_str(key)
        if key_str == self.record_hotkey and not self.entry_focused:
            return
        if self.recording:
            self._record_event({
                'type': 'key', 'key': self._key_repr(key), 'action': 'release'
            })

    def start_recording(self):
        with self._lock:
            if self.recording:
                return
            self.recording = True
            self.events = []
            self.start_time = time.perf_counter()
            self.last_time = self.start_time
            self.last_move_time = 0
            self.last_pos = None
        self._schedule_cb('on_record_start')

    def stop_recording(self):
        with self._lock:
            if not self.recording:
                return
            self.recording = False
        self._schedule_cb('on_record_stop', len(self.events))

    def toggle(self):
        if self.recording:
            self.stop_recording()
        else:
            self.start_recording()

    def start_listeners(self):
        if self.listeners_running:
            return
        self.mouse_listener = mouse.Listener(
            on_move=self.on_move,
            on_click=self.on_click,
            on_scroll=self.on_scroll
        )
        self.keyboard_listener = keyboard.Listener(
            on_press=self.on_press,
            on_release=self.on_release
        )
        self.mouse_listener.start()
        self.keyboard_listener.start()
        self.listeners_running = True
        self._schedule_cb('on_listeners_ready')

    def stop_listeners(self):
        self.listeners_running = False
        if self.recording:
            self.stop_recording()
        if self.mouse_listener:
            self.mouse_listener.stop()
        if self.keyboard_listener:
            self.keyboard_listener.stop()

    def save(self, filepath):
        with open(filepath, 'w') as f:
            json.dump(self.events, f, indent=2)