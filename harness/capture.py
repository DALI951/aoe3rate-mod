"""capture.py - screen capture helper for the aoe3rate-mod harness.

Uses dxcam (Desktop Duplication -> sees the ACTUAL GPU frames, works even for
fullscreen games that GDI/PrintScreen can't capture). Falls back to mss, then
PIL ImageGrab if dxcam cannot initialize.

    python capture.py out.png [monitor_index]
"""
import sys

MONITOR = int(sys.argv[2]) if len(sys.argv) > 2 else 0  # 0 = primary


def capture(path):
    # 1) dxcam (best - desktop duplication, sees game frames)
    try:
        import dxcam

        cam = dxcam.create(output_idx=MONITOR, output_color="RGB")
        if cam is None:
            raise RuntimeError("dxcam.create returned None")
        frame = cam.grab()
        if frame is not None and frame.size > 0:
            from PIL import Image

            Image.fromarray(frame).save(path)
            cam.release()
            return "dxcam"
    except Exception as e:
        print(f"dxcam failed: {e}", file=sys.stderr)

    # 2) mss
    try:
        import mss

        with mss.mss() as s:
            mon = s.monitors[MONITOR + 1]
            shot = s.grab(mon)
        from PIL import Image

        Image.frombytes("RGB", shot.size, shot.rgb).save(path)
        return "mss"
    except Exception as e:
        print(f"mss failed: {e}", file=sys.stderr)

    # 3) PIL ImageGrab
    try:
        from PIL import ImageGrab

        img = ImageGrab.grab()
        img.save(path)
        return "pil"
    except Exception as e:
        print(f"pil failed: {e}", file=sys.stderr)
        return None


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: capture.py out.png [monitor]")
        sys.exit(1)
    method = capture(sys.argv[1])
    if method:
        print(f"captured via {method}: {sys.argv[1]}")
    else:
        print("CAPTURE FAILED")
        sys.exit(1)