#!/usr/bin/env python3
"""Reject macOS bundles that depend on the packaging machine's libraries."""

import argparse
import json
from functools import lru_cache
from pathlib import Path
import subprocess
import sys


MACHO_MAGIC = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


def is_system(path):
    return path.startswith(("/usr/lib/", "/System/Library/"))


def validate(app, require_vulkan=False):
    app = app.resolve()
    contents = app / "Contents"
    executable_dir = contents / "MacOS"
    errors = []
    images = []
    for path in contents.rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) in MACHO_MAGIC:
                    images.append(path)

    if not images:
        return ["No Mach-O images found in bundle"]

    def expand(value, image, executable):
        return Path(value.replace("@loader_path", str(image.parent)).replace(
            "@executable_path", str(executable.parent)))

    @lru_cache(maxsize=None)
    def load_paths(image):
        output = subprocess.check_output(["otool", "-l", str(image)], text=True)
        result = []
        identities = []
        lines = iter(output.splitlines())
        for line in lines:
            if line.strip() == "cmd LC_RPATH":
                next(lines)  # cmdsize
                value = next(lines).strip().removeprefix("path ").rsplit(" (offset ", 1)[0]
                result.append(value)
            elif line.strip() == "cmd LC_ID_DYLIB":
                next(lines)
                identities.append(next(lines).strip().removeprefix("name ").rsplit(" (offset ", 1)[0])
        return result, identities

    @lru_cache(maxsize=None)
    def dependencies(image):
        output = subprocess.check_output(["otool", "-L", str(image)], text=True)
        identities = load_paths(image)[1]
        result = []
        for line in output.splitlines():
            # Universal otool output has a header for each architecture.
            if " (compatibility version " not in line:
                continue
            dependency = line.strip().split(" (compatibility version ", 1)[0]
            if dependency in identities or is_system(dependency):
                continue
            result.append(dependency)
        return result

    reached = set()
    checked = set()

    def walk(image, executable, inherited=(), ancestors=frozenset()):
        image = image.resolve()
        if image in ancestors:
            return
        reached.add(image)
        search = tuple(dict.fromkeys(
            [expand(value, image, executable).resolve() for value in load_paths(image)[0]]
            + list(inherited)))
        context = (image, executable, search)
        if context in checked:
            return
        checked.add(context)
        for dependency in dependencies(image):
            if dependency.startswith("@rpath/"):
                candidates = [root / dependency[len("@rpath/"):] for root in search]
            else:
                candidates = [expand(dependency, image, executable)]
            target = next((candidate.resolve() for candidate in candidates
                           if candidate.is_file()), None)
            if target is None or not target.is_relative_to(app):
                errors.append(f"{image.relative_to(app)} (executable {executable.name}): "
                              f"dependency outside bundle or missing: {dependency}")
            else:
                walk(target, executable, search, ancestors | {image})

    executables = [image for image in images if image.parent == executable_dir]
    if not executables:
        return ["No executable found in Contents/MacOS"]
    for executable in executables:
        walk(executable, executable)

    # Qt plugins and Vulkan drivers are loaded dynamically by the main app.
    # Traverse each additional image with that executable's runpaths, while
    # keeping independently launched helpers in their own load graphs.
    main = next((image for image in executables if image.name == "RawPreamp"), executables[0])
    main_rpaths = tuple(expand(value, main, main) for value in load_paths(main)[0])
    for image in images:
        if image.resolve() not in reached:
            walk(image, main, main_rpaths)

    # MoltenVK is loaded through a JSON manifest, so otool cannot catch a
    # missing driver or an SDK/Homebrew path embedded in the manifest.
    manifest = contents / "Resources/vulkan/icd.d/MoltenVK_icd.json"
    loader = contents / "Frameworks/libvulkan.1.dylib"
    if require_vulkan or loader.exists() or manifest.exists():
        for required in (loader, contents / "Frameworks/libvulkan.dylib", manifest):
            if not required.is_file():
                errors.append(f"Missing Vulkan runtime file: {required.relative_to(app)}")
        if manifest.is_file():
            try:
                driver = manifest.parent / json.loads(manifest.read_text())["ICD"]["library_path"]
                if not driver.is_file() or not driver.resolve().is_relative_to(app):
                    errors.append("MoltenVK manifest does not resolve to a bundled driver")
            except (ValueError, KeyError, TypeError) as error:
                errors.append(f"Invalid MoltenVK manifest: {error}")
    return errors


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--require-vulkan", action="store_true")
    args = parser.parse_args()
    problems = validate(args.app, require_vulkan=args.require_vulkan)
    if problems:
        sys.exit("macOS bundle validation failed:\n" + "\n".join(problems))
    print("macOS bundle dependencies are self-contained")
