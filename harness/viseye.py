"""viseye.py - vision helper for the aoe3rate-mod test harness.

Sends a screenshot (PNG, base64 data-URI) to OpenRouter google/gemma-3-27b-it
(the ONLY model/path proven to read Dali's screens correctly - 4.2s and 18.3s
direct-API tests) and returns the model's answer. Uses the OpenRouter key from
opencode's auth.json (sk-or-v1-...). CLI:

    python viseye.py shot.png "prompt text" [--json]

Prints the model reply (or JSON if --json). Exit code 0 on success.
"""
import argparse
import base64
import json
import os
import sys
import urllib.request

MODEL = "google/gemma-3-27b-it"
API = "https://openrouter.ai/api/v1/chat/completions"


def find_key():
    """Locate the OpenRouter sk-or-v1 key in opencode auth.json (recursive)."""
    candidates = [
        os.environ.get("OPENROUTER_API_KEY", ""),
    ]
    auth_paths = [
        os.path.expandvars(r"%USERPROFILE%\.local\share\opencode\auth.json"),
        os.path.expandvars(r"%USERPROFILE%\.config\opencode\auth.json"),
    ]
    for p in auth_paths:
        if not os.path.exists(p):
            continue
        try:
            with open(p, "r", encoding="utf-8-sig") as f:
                data = json.load(f)
        except Exception:
            continue

        def walk(o):
            if isinstance(o, dict):
                for v in o.values():
                    yield from walk(v)
            elif isinstance(o, list):
                for v in o:
                    yield from walk(v)
            elif isinstance(o, str):
                yield o

        for s in walk(data):
            if s.startswith("sk-or-v1-"):
                candidates.append(s)
    for k in candidates:
        if k.startswith("sk-or-v1-"):
            return k
    return None


def ask(image_path, prompt, timeout=120):
    key = find_key()
    if not key:
        print("ERROR: no OpenRouter key found", file=sys.stderr)
        sys.exit(2)
    with open(image_path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode("ascii")
    data_uri = "data:image/png;base64," + b64
    body = {
        "model": MODEL,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {"url": data_uri}},
                ],
            }
        ],
        "max_tokens": 900,
    }
    req = urllib.request.Request(
        API,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.load(r)
    return resp["choices"][0]["message"]["content"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("prompt")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    try:
        out = ask(args.image, args.prompt)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    if args.json:
        print(json.dumps({"text": out}, ensure_ascii=False))
    else:
        print(out)


if __name__ == "__main__":
    main()