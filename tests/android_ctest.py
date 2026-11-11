#!/usr/bin/env python3
"""Deploy native tests and dependencies to an Android emulator; run via CTest."""

from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

REMOTE = "/data/local/tmp/rawpreamp-tests"


def adb(*args, **kwargs):
    return subprocess.run(["adb", *args], check=True, **kwargs)


def deploy(build, qt, ndk):
    build, qt, ndk = map(Path, (build, qt, ndk))
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    triplet = cache["VCPKG_TARGET_TRIPLET"]
    configuration = cache.get("CMAKE_BUILD_TYPE") or "Release"
    architectures = {
        "arm64-v8a": "aarch64-linux-android",
        "x86_64": "x86_64-linux-android",
    }
    architecture = architectures[cache["ANDROID_ABI"]]
    runtime = next(ndk.glob(
        f"toolchains/llvm/prebuilt/*/sysroot/usr/lib/{architecture}/libc++_shared.so"), None)
    if runtime is None:
        raise RuntimeError(f"NDK C++ runtime not found for {architecture}")
    with tempfile.TemporaryDirectory(prefix="rawpreamp-android-tests-") as directory:
        stage = Path(directory)
        (stage / "bin").mkdir()
        (stage / "lib").mkdir()
        for line in (build / f"android-tests-{configuration}.txt").read_text().splitlines():
            executable = Path(line)
            # A failed advisory device-test build must not prevent deployment
            # of the portable suite. CTest reports missing executables itself.
            if not executable.is_file():
                print(f"::warning::Test executable was not built: {executable}")
                continue
            shutil.copy2(executable, stage / "bin" / executable.name)
        # Include indirect dependencies too. Android's linker does not use
        # the build machine's RUNPATHs when these binaries run on the device.
        libraries = list((qt / "lib").glob("*.so"))
        libraries += list((Path(cache["VCPKG_INSTALLED_DIR"]) / triplet / "lib").glob("*.so*"))
        libraries.append(runtime)
        for library in libraries:
            shutil.copy2(library.resolve(), stage / "lib" / library.name)
        # Android QFactoryLoader searches one flat directory for
        # libplugins_<category>_*.so, as in an APK's native library directory.
        (stage / "plugins").mkdir()
        for plugin in (qt / "plugins").rglob("*.so"):
            shutil.copy2(plugin.resolve(), stage / "plugins" / plugin.name)
        adb("shell", "mkdir", "-p", REMOTE)
        adb("push", str(stage) + "/.", REMOTE + "/")
        adb("shell", "chmod", "-R", "755", REMOTE)
        adb("shell", "chmod", "700", REMOTE)


def run(executable, *args):
    environment = {
        "LD_LIBRARY_PATH": REMOTE + "/lib",
        "QT_PLUGIN_PATH": REMOTE + "/plugins",
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QPA_FONTDIR": "/system/fonts",
        "TMPDIR": REMOTE,
        "HOME": REMOTE,
        "XDG_RUNTIME_DIR": REMOTE,
    }
    command = ["env", *(f"{key}={value}" for key, value in environment.items()),
               REMOTE + "/bin/" + Path(executable).name, *args]
    # adb shell propagates the remote exit status, including CTest's skip 77.
    result = subprocess.run(["adb", "shell", f"cd {shlex.quote(REMOTE)} && {shlex.join(command)}"])
    if result.returncode not in (0, 77):
        # Native crashes otherwise reach CTest as only "Segmentation fault".
        # Keep the emulator's tombstone backtrace in the job log.
        print("Android crash log (most recent entries):", flush=True)
        try:
            subprocess.run(["adb", "logcat", "-b", "crash", "-d", "-t", "200"], timeout=15)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"Could not collect Android crash log: {error}", flush=True)
    return result.returncode


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "deploy":
        deploy(*sys.argv[2:])
    elif len(sys.argv) >= 3 and sys.argv[1] == "run":
        sys.exit(run(*sys.argv[2:]))
    else:
        sys.exit("Usage: android_ctest.py deploy BUILD QT NDK | run EXECUTABLE [ARGS...]")
