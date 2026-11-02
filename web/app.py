#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
alg-rgb local dashboard

A small Flask application that turns the alg-rgb CLI into a local,
SaaS-style control panel.  It binds to loopback by default and has no
authentication because it is intended for a single-user desktop session.
Run it with --allow-lan only on a trusted network.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import platform
import re
import shutil
import subprocess
import threading
import webbrowser
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, request, send_from_directory

APP_VERSION = "1.0.0"
DEVICE_PATH = "/dev/alg_rgb"
ACPI_DEVICE_PATH = Path("/sys/bus/acpi/devices/CLV0001:00")
MODULE_PATH = Path("/sys/module/alg_rgb")
CAPABILITIES_PATH = Path("/sys/class/misc/alg_rgb/capabilities")
BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

EFFECTS = {
    "none",
    "random",
    "flash",
    "tempo",
    "dance",
    "wave",
    "cycle",
    "breath",
    "static",
}
ZONES = {"main", "left", "mid", "right", "bar", "logo", "all"}
COLOR_NAMES = {
    "off",
    "red",
    "orange",
    "yellow",
    "lime",
    "light-green",
    "green",
    "green-cyan",
    "cyan",
    "light-blue",
    "blue",
    "violet",
    "magenta",
    "pink",
    "flesh",
    "bluish-white",
    "white",
}

app = Flask(__name__, static_folder=str(STATIC_DIR), static_url_path="/static")
app.config["JSON_SORT_KEYS"] = False


class ApiError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.message = message
        self.status = status


ACTIVITY: deque[dict[str, Any]] = deque(maxlen=50)
ACTIVITY_LOCK = threading.Lock()
ACTIVITY_IDS = itertools.count(1)


def add_activity(action: str, detail: str, ok: bool = True) -> None:
    entry = {
        "id": next(ACTIVITY_IDS),
        "time": datetime.now().strftime("%H:%M:%S"),
        "action": action,
        "detail": detail,
        "ok": ok,
    }
    with ACTIVITY_LOCK:
        ACTIVITY.appendleft(entry)


def activity_snapshot() -> list[dict[str, Any]]:
    with ACTIVITY_LOCK:
        return list(ACTIVITY)


def find_cli() -> str | None:
    configured = os.environ.get("ALG_RGB_BIN")
    candidates = [
        configured,
        shutil.which("alg-rgb"),
        "/usr/local/bin/alg-rgb",
        str(BASE_DIR.parent / "cli" / "alg-rgb"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def run_cli(args: list[str], timeout: float = 12.0) -> tuple[int, str, str]:
    cli = find_cli()
    if not cli:
        return 127, "", "alg-rgb CLI not found (install it or set ALG_RGB_BIN)"
    try:
        proc = subprocess.run(
            [cli, *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            env={**os.environ, "LC_ALL": "C"},
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, "", f"alg-rgb {' '.join(args)} timed out"
    except OSError as exc:
        return 126, "", f"could not run {cli}: {exc}"


def parse_json(text: str) -> dict[str, Any] | None:
    if not text:
        return None
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def get_status() -> dict[str, Any]:
    rc, out, err = run_cli(["status", "--json"])
    data = parse_json(out)
    if data is None:
        return {"ok": False, "error": err or out or "empty response from alg-rgb"}
    if rc != 0 and data.get("ok") is not False:
        data["ok"] = False
    return data


def read_text(path: Path, default: str = "") -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return default


def read_capabilities() -> list[str]:
    text = read_text(CAPABILITIES_PATH)
    return [line for line in text.splitlines() if line.strip()]


def find_perkey_hid() -> list[str]:
    matches: list[str] = []

    hid_dir = Path("/sys/bus/hid/devices")
    if hid_dir.is_dir():
        try:
            for entry in hid_dir.iterdir():
                name = entry.name.lower()
                if "048d" in name and "8910" in name:
                    matches.append(entry.name)
        except OSError:
            pass

    usb_dir = Path("/sys/bus/usb/devices")
    if usb_dir.is_dir():
        try:
            for entry in usb_dir.iterdir():
                try:
                    vendor = (entry / "idVendor").read_text(encoding="utf-8").strip()
                    product = (entry / "idProduct").read_text(encoding="utf-8").strip()
                except OSError:
                    continue
                if vendor.lower() == "048d" and product.lower() == "8910":
                    matches.append(entry.name)
        except OSError:
            pass

    return sorted(set(matches))


def list_hid_devices() -> list[str]:
    hid_dir = Path("/sys/bus/hid/devices")
    if not hid_dir.is_dir():
        return []
    try:
        return sorted(entry.name for entry in hid_dir.iterdir())
    except OSError:
        return []


def get_health() -> dict[str, Any]:
    cli = find_cli()
    perkey = find_perkey_hid()
    acpi_status = read_text(ACPI_DEVICE_PATH / "status")
    return {
        "app_version": APP_VERSION,
        "kernel": platform.release(),
        "cli": {"found": cli is not None, "path": cli or ""},
        "device": {
            "path": DEVICE_PATH,
            "present": os.path.exists(DEVICE_PATH),
            "accessible": os.access(DEVICE_PATH, os.R_OK | os.W_OK),
        },
        "module": {
            "present": MODULE_PATH.exists(),
            "version": read_text(MODULE_PATH / "version"),
        },
        "acpi": {
            "device": "CLV0001:00",
            "present": ACPI_DEVICE_PATH.exists(),
            "status": acpi_status,
        },
        "perkey_hid": bool(perkey),
        "perkey_hid_matches": perkey,
        "hid_devices": list_hid_devices(),
        "capabilities": read_capabilities(),
    }


def validate_color(value: Any) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ApiError("A colour value is required")
    value = value.strip()
    if re.fullmatch(r"#[0-9a-fA-F]{6}", value):
        return value.lower()
    if re.fullmatch(r"0x[0-9a-fA-F]{6}", value, re.IGNORECASE):
        return "#" + value[2:].lower()
    if re.fullmatch(r"[A-Za-z][A-Za-z-]*", value):
        if value.lower() in COLOR_NAMES:
            return value.lower()
        raise ApiError("Unknown colour name")
    raise ApiError("Colour must be #RRGGBB or a known colour name")


def validate_brightness(value: Any) -> int:
    try:
        level = int(value)
    except (TypeError, ValueError) as exc:
        raise ApiError("Brightness must be an integer from 0 to 4") from exc
    if level < 0 or level > 4:
        raise ApiError("Brightness must be from 0 to 4")
    return level


def validate_zone(value: Any) -> str:
    if value is None:
        return "main"
    if not isinstance(value, str) or value.lower() not in ZONES:
        raise ApiError("Unknown zone")
    return value.lower()


def validate_effect(value: Any) -> str:
    if not isinstance(value, str) or value.lower() not in EFFECTS:
        raise ApiError("Unknown effect")
    return value.lower()


def run_checked(args: list[str]) -> None:
    rc, out, err = run_cli(args)
    if rc == 0:
        return
    message = err or parse_json(out) or out or f"alg-rgb exited with status {rc}"
    if isinstance(message, dict):
        message = message.get("error", "alg-rgb command failed")
    raise ApiError(str(message), 502)


def action_response(action: str, detail: str):
    status = get_status()
    if status.get("ok") is False:
        add_activity(action, status.get("error", "failed"), False)
        return (
            jsonify(
                {
                    "ok": False,
                    "error": status.get("error", "The keyboard did not report its state"),
                    "status": status,
                }
            ),
            502,
        )
    add_activity(action, detail, True)
    return jsonify({"ok": True, "status": status, "activity": activity_snapshot()})


@app.get("/")
def index():
    return send_from_directory(str(STATIC_DIR), "index.html")


@app.get("/favicon.svg")
def favicon():
    return send_from_directory(str(STATIC_DIR), "favicon.svg")


@app.get("/api/status")
def api_status():
    data = get_status()
    data["health"] = get_health()
    return jsonify(data)


@app.get("/api/health")
def api_health():
    return jsonify({"ok": True, **get_health()})


@app.get("/api/activity")
def api_activity():
    return jsonify({"ok": True, "activity": activity_snapshot()})


@app.get("/api/meta")
def api_meta():
    return jsonify(
        {
            "ok": True,
            "app": "Lumen RGB Studio",
            "version": APP_VERSION,
            "protocol": {
                "acpi_device": "CLV0001",
                "uuid": "93f224e4-fbdc-4bbf-add6-db71bdc0afad",
                "function": "0x67",
                "color_order": "GRB",
                "brightness_levels": [0, 1, 2, 3, 4],
                "safety": "verified _DSM 0x67 only; 0x68-0x6A disabled",
            },
            "endpoints": [
                "GET  /api/status",
                "POST /api/color",
                "POST /api/brightness",
                "POST /api/effect",
                "POST /api/power",
                "POST /api/preset",
                "GET  /api/probe",
                "GET  /api/activity",
                "GET  /api/health",
                "GET  /api/meta",
            ],
        }
    )


@app.post("/api/color")
def api_color():
    payload = request.get_json(silent=True) or {}
    color = validate_color(payload.get("color"))
    brightness = payload.get("brightness")
    zone = validate_zone(payload.get("zone"))

    args = ["color", color]
    if brightness is not None:
        args.append(str(validate_brightness(brightness)))
    if zone != "main":
        args += ["--zone", zone]

    run_checked(args)
    suffix = f" at {brightness}/4" if brightness is not None else ""
    return action_response("Color", f"{color}{suffix} · {zone}")


@app.post("/api/brightness")
def api_brightness():
    payload = request.get_json(silent=True) or {}
    level = validate_brightness(payload.get("brightness"))
    run_checked(["brightness", str(level)])
    return action_response("Brightness", f"{level}/4")


@app.post("/api/effect")
def api_effect():
    payload = request.get_json(silent=True) or {}
    effect = validate_effect(payload.get("effect"))

    if effect in {"none", "static"}:
        status = get_status()
        color = status.get("color", "#ffffff") if status.get("ok") else "#ffffff"
        run_checked(["color", color])
        return action_response("Effect", "Static colour")

    run_checked(["effect", effect])
    return action_response("Effect", effect.title())


@app.post("/api/power")
def api_power():
    payload = request.get_json(silent=True) or {}
    on = payload.get("on")
    if not isinstance(on, bool):
        raise ApiError("'on' must be true or false")
    run_checked(["on" if on else "off"])
    return action_response("Power", "on" if on else "off")


@app.post("/api/preset")
def api_preset():
    payload = request.get_json(silent=True) or {}
    color = validate_color(payload.get("color", "#ffffff"))
    brightness = validate_brightness(payload.get("brightness", 3))
    effect = validate_effect(payload.get("effect", "none"))
    zone = validate_zone(payload.get("zone"))

    # A preset always ends powered on.  Apply the static base first, then
    # brightness, then start the motion so the animation is not cancelled by
    # the brightness write.
    run_checked(["on"])
    run_checked(["color", color, "--zone", zone])
    run_checked(["brightness", str(brightness)])
    if effect not in {"none", "static"}:
        run_checked(["effect", effect])
    effect_label = "static" if effect in {"none", "static"} else effect
    return action_response(
        "Preset",
        f"{color} · {effect_label} · {brightness}/4",
    )


@app.get("/api/probe")
def api_probe():
    health = get_health()
    status = get_status()
    perkey = health["perkey_hid"]

    checks = [
        {
            "id": "acpi",
            "label": "ACPI CLV0001 device",
            "ok": health["acpi"]["present"],
            "detail": (
                f"present, status {health['acpi']['status']}"
                if health["acpi"]["present"]
                else "not present in the ACPI namespace"
            ),
        },
        {
            "id": "module",
            "label": "alg_rgb kernel module",
            "ok": health["module"]["present"],
            "detail": (
                f"loaded, version {health['module']['version'] or 'unknown'}"
                if health["module"]["present"]
                else "not loaded; run the installer and modprobe alg_rgb"
            ),
        },
        {
            "id": "device",
            "label": "/dev/alg_rgb access",
            "ok": health["device"]["accessible"],
            "detail": (
                "read/write access"
                if health["device"]["accessible"]
                else "device missing or current user lacks the alg-rgb group"
            ),
        },
        {
            "id": "hid",
            "label": "Per-key HID 048D:8910",
            "ok": perkey,
            "detail": (
                "detected on the HID bus"
                if perkey
                else "not enumerated; only the CLV0001 zone-RGB path is exposed"
            ),
        },
    ]

    reason = (
        "HID 048D:8910 is present, but no live per-key ACPI protocol has been "
        "verified. Per-key output is not implemented."
        if perkey
        else "HID 048D:8910 (ITE 829x per-key controller) is not enumerated. This "
        "machine exposes zone RGB through CLV0001 only; per-key control would "
        "need a HID interface that the firmware is not publishing."
    )

    return jsonify(
        {
            "ok": True,
            "checks": checks,
            "perkey": {
                "available": perkey,
                "hid_id": "048D:8910",
                "reason": reason,
            },
            "hid_devices": health["hid_devices"],
            "capabilities": health["capabilities"],
            "status": status,
        }
    )


@app.errorhandler(ApiError)
def handle_api_error(error: ApiError):
    return jsonify({"ok": False, "error": error.message}), error.status


@app.errorhandler(404)
def handle_not_found(_error):
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "unknown API endpoint"}), 404
    return send_from_directory(str(STATIC_DIR), "index.html")


@app.errorhandler(500)
def handle_server_error(error):
    return jsonify({"ok": False, "error": f"internal error: {error}"}), 500


@app.after_request
def add_headers(response):
    if request.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["Referrer-Policy"] = "no-referrer"
    return response


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ALG RGB local dashboard")
    parser.add_argument("--host", default="127.0.0.1", help="bind address")
    parser.add_argument("--port", type=int, default=8722, help="TCP port")
    parser.add_argument(
        "--allow-lan",
        action="store_true",
        help="allow binding to a non-loopback address (no authentication)",
    )
    parser.add_argument("--open", action="store_true", help="open a browser")
    parser.add_argument("--debug", action="store_true", help="Flask debug mode")
    args = parser.parse_args()

    loopback = {"127.0.0.1", "localhost", "::1"}
    if args.host not in loopback and not args.allow_lan:
        parser.error("refusing non-loopback bind without --allow-lan")

    return args


def main() -> None:
    args = parse_args()
    display_host = "127.0.0.1" if args.host in {"0.0.0.0", "::"} else args.host
    url = f"http://{display_host}:{args.port}/"

    print(f"Lumen RGB Studio {APP_VERSION}")
    print(f"Dashboard: {url}")
    print(f"CLI:       {find_cli() or 'not found'}")
    print(f"Device:    {DEVICE_PATH} ({'present' if os.path.exists(DEVICE_PATH) else 'missing'})")

    if args.open:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()

    app.run(host=args.host, port=args.port, debug=args.debug, use_reloader=False)


if __name__ == "__main__":
    main()
