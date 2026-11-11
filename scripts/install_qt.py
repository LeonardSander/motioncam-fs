#!/usr/bin/env python3
"""Install public Qt archives where aqt lacks repository-layout support."""

import argparse
import hashlib
import os
import tempfile
from concurrent.futures import ThreadPoolExecutor
from html.parser import HTMLParser
from pathlib import Path
from subprocess import run
from urllib.parse import urljoin
from urllib.request import urlopen


def archive_components(platform, architecture, version):
    component = "qt6_" + version.replace(".", "")
    package = "qt.qt6." + version.replace(".", "")
    if platform == "android":
        if architecture not in ("arm64_v8a", "x86_64"):
            raise ValueError(f"Unsupported Android architecture: {architecture}")
        repository = f"all_os/android/{component}/{component}_{architecture}"
        kit = f"android_{architecture}"
    else:
        if architecture != "win64_msvc2022_64":
            raise ValueError(f"Unsupported Windows architecture: {architecture}")
        repository = f"windows_x86/desktop/{component}/{component}_msvc2022_64"
        kit = architecture
    base = f"https://download.qt.io/online/qtsdkrepository/{repository}/"
    modules = ["qtbase", "qtdeclarative", "qtsvg", "qttools"]
    if platform == "windows":
        modules += ["qttranslations", "opengl32sw", "d3dcompiler_47"]
    return base, {
        f"{package}.{kit}/": tuple(modules),
        f"{package}.addons.qtmultimedia.{kit}/": ("qtmultimedia",),
        f"{package}.addons.qtshadertools.{kit}/": ("qtshadertools",),
    }


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            self.links.extend(value for key, value in attrs if key == "href")


def download(url, destination):
    digest = urlopen(url + ".sha1", timeout=60).read().decode().strip().split()[0]
    with urlopen(url, timeout=60) as response, destination.open("wb") as output:
        sha1 = hashlib.sha1()
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
            sha1.update(chunk)
    if sha1.hexdigest() != digest:
        raise RuntimeError(f"Checksum mismatch: {destination.name}")
    print(f"Verified {destination.name}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("android", "windows"))
    parser.add_argument("destination", type=Path)
    parser.add_argument("architecture", nargs="?")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    architecture = args.architecture or (
        "arm64_v8a" if args.platform == "android" else "win64_msvc2022_64")
    version = os.environ.get("QT_VERSION", "6.12.0")
    base, components = archive_components(args.platform, architecture, version)
    target = args.destination.resolve()
    urls = []
    for component, modules in components.items():
        page = urljoin(base, component)
        parser = Links()
        parser.feed(urlopen(page, timeout=60).read().decode())
        matches = [name for name in parser.links if name.endswith(".7z")
                   and any(module in name for module in modules)]
        if len(matches) != len(modules):
            raise RuntimeError(f"Unexpected archive list at {page}: {matches}")
        urls.extend(urljoin(page, name) for name in matches)

    if args.dry_run:
        print("\n".join(urls))
        return
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rawpreamp-qt-") as temp:
        temporary = Path(temp)
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda url: download(url, temporary / url.rsplit("/", 1)[-1]), urls))
        for archive in temporary.glob("*.7z"):
            run(["7z", "x", "-y", "-bso0", f"-o{target}", str(archive)], check=True)
    if args.platform == "windows":
        (target / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n")
        # Match install-qt-action's public outputs and PATH setup.
        for variable, text in (
                ("GITHUB_OUTPUT", f"qt-root-dir={target}\n"),
                ("GITHUB_PATH", f"{target / 'bin'}\n")):
            if variable in os.environ:
                with open(os.environ[variable], "a", encoding="utf-8") as output:
                    output.write(text)
    print(f"Installed Qt {version} {args.platform} {architecture} kit in {target}")


if __name__ == "__main__":
    main()
