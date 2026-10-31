#!/usr/bin/env python3
"""Sample early and steady Android gallery playback with the thumbnail row open/closed."""
import argparse
import json
import os
import shlex
from pathlib import Path
import subprocess
import sys
import time


def run(*command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs).stdout


def adb(*command):
    return run("adb", *command, timeout=45)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True, help="Session JSON path on the phone")
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_ROOT") or os.environ.get("ANDROID_NDK_HOME"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, action="append", default=[], help="Unstripped app/Qt library directory; repeatable")
    parser.add_argument("--duration", type=int, default=6, help="Seconds per CPU sample")
    parser.add_argument("--late-after", type=int, default=16, help="Start steady sample this many seconds after first frame")
    parser.add_argument("--landscape", action="store_true")
    parser.add_argument("--flags", default="", help="Additional gallery perf flags, e.g. guipool")
    parser.add_argument("--frame-log", action="store_true", help="Also enable verbose per-frame timers (adds overhead)")
    args = parser.parse_args()
    if not args.ndk or args.duration <= 0 or args.late_after <= args.duration + 1:
        parser.error("Set --ndk/ANDROID_NDK_ROOT, positive --duration and --late-after > duration + 1")
    ndk = Path(args.ndk).resolve()
    profiler = ndk / "simpleperf/app_profiler.py"
    if not profiler.is_file():
        parser.error(f"NDK profiler not found: {profiler}")
    sys.path.insert(0, str(profiler.parent))
    from simpleperf_utils import get_host_binary_path
    report_tool = get_host_binary_path("simpleperf")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "configuration.json").write_text(json.dumps(vars(args), default=str, indent=2))
    package = "com.motioncam.fuse"
    log_path = "files/diagnostics/gallery-perf.log"
    stayon = adb("shell", "settings", "get", "global", "stay_on_while_plugged_in").strip()
    rotation, fixed = [], ""
    if args.landscape:
        rotation = adb("shell", "wm", "user-rotation").strip().split()
        fixed = adb("shell", "wm", "fixed-to-user-rotation").strip()
        valid_rotation = rotation == ["free"] or (
            len(rotation) == 2 and rotation[0] == "lock" and rotation[1] in ("0", "1", "2", "3"))
        if not valid_rotation or fixed not in ("default", "enabled", "disabled"):
            parser.error("Unrecognized device rotation state; omit --landscape and rotate manually")
    profiles = []
    try:
        adb("shell", "svc", "power", "stayon", "usb")
        if args.landscape:
            adb("shell", "wm", "fixed-to-user-rotation", "enabled")
            adb("shell", "wm", "user-rotation", "lock", "1")
        adb("shell", "run-as", package, "mkdir", "-p", "files/diagnostics")
        for name, thumbnails in (("open", "on"), ("closed", "off")):
            adb("shell", "am", "force-stop", package)
            adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
            adb("shell", "wm", "dismiss-keyguard")
            adb("shell", "run-as", package, "truncate", "-s", "0", log_path)
            flags = "nohist,skipbackfill,preserveproxy,zerocopy,capped,audio,keepalive"
            if args.flags:
                flags += "," + args.flags
            if args.frame_log:
                flags += ",profile"
            if thumbnails == "off":
                flags += ",hidethumbnails"
            playback_ms = (args.late_after + args.duration + 20) * 1000
            adb("shell", "am", "start", "-S", "-n", package + "/com.motioncam.fuse.MotionCamActivity",
                "--es", "gallery-perf-session", shlex.quote(args.session),
                "--es", "gallery-perf-playback-ms", str(playback_ms),
                "--es", "gallery-perf-thumbnails", thumbnails, "--es", "gallery-perf-flags", shlex.quote(flags))
            deadline = time.monotonic() + 45
            while True:
                log = adb("shell", "run-as", package, "cat", log_path)
                if "[error]" in log:
                    raise RuntimeError(log[-3000:])
                if "event=gallery_startup_first_frame" in log:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("Gallery did not produce its first frame")
                time.sleep(0.25)
            started = time.monotonic()
            for phase, delay in (("early", 1), ("late", args.late_after)):
                time.sleep(max(0, started + delay - time.monotonic()))
                prefix = args.output / f"{name}-{phase}"
                print(f"Sampling {name} thumbnails, {phase} playback", flush=True)
                subprocess.run([sys.executable, str(profiler), "-p", package,
                    "--disable_adb_root", "--unrepresentative_profile_debug_app", "-nb",
                    "-r", f"-e task-clock:u -f 400 -g --duration {args.duration}",
                    "-o", str(prefix.with_suffix(".data"))], cwd=args.output, check=True, timeout=args.duration + 45)
                profiles.append(prefix)
            log = adb("shell", "run-as", package, "cat", log_path)
            (args.output / f"{name}.log").write_text(log)
            (args.output / f"{name}.memory.txt").write_text(adb("shell", "dumpsys", "meminfo", package))
    finally:
        restore = [("am", "force-stop", package)]
        if args.landscape:
            restore += [("wm", "user-rotation", *rotation), ("wm", "fixed-to-user-rotation", fixed)]
        restore.append(("settings", "put", "global", "stay_on_while_plugged_in", stayon)
            if stayon.isdecimal() else ("settings", "delete", "global", "stay_on_while_plugged_in"))
        restore_failed = False
        for command in restore:
            try:
                adb("shell", *command)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                restore_failed = True
                print(f"Could not restore device state with {command}: {error}", file=sys.stderr)
        if restore_failed and sys.exc_info()[0] is None:
            raise RuntimeError("Profiling completed, but device cleanup failed; raw samples are retained")

    # Host-side symbol lookup must not delay the steady playback sample.
    symbol_options = [argument for directory in args.symbols
        for argument in ("--symdir", str(directory.resolve()))]
    for prefix in profiles:
        for kind, options in (("calls", ["--children"]), ("self", [])):
            run(report_tool, "report", "-i", str(prefix.with_suffix(".data")),
                *symbol_options, "--sort", "comm,dso,symbol", "--percent-limit", "1",
                *options, "-o", str(prefix.with_suffix(f".{kind}.txt")), timeout=45)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stderr or str(error), file=sys.stderr)
        raise SystemExit(1)
