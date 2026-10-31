#!/usr/bin/env python3
"""Install the public Qt 6.12.0 Android arm64 archives into a Qt kit directory."""

import hashlib
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from html.parser import HTMLParser
from pathlib import Path
from subprocess import run
from urllib.parse import urljoin
from urllib.request import urlopen

BASE = ("https://download.qt.io/online/qtsdkrepository/all_os/android/"
        "qt6_6120/qt6_6120_arm64_v8a/")
COMPONENTS = {
    "qt.qt6.6120.android_arm64_v8a/":
        ("qtbase", "qtdeclarative", "qtsvg", "qttools"),
    "qt.qt6.6120.addons.qtmultimedia.android_arm64_v8a/":
        ("qtmultimedia",),
    "qt.qt6.6120.addons.qtshadertools.android_arm64_v8a/":
        ("qtshadertools",),
}


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            self.links.extend(value for key, value in attrs if key == "href")


def download(url, destination):
    digest = urlopen(url + ".sha1").read().decode().strip().split()[0]
    with urlopen(url) as response, destination.open("wb") as output:
        sha1 = hashlib.sha1()
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
            sha1.update(chunk)
    if sha1.hexdigest() != digest:
        raise RuntimeError(f"Checksum mismatch: {destination.name}")
    print(f"Verified {destination.name}", flush=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: install_qt_android.py DESTINATION_KIT_DIRECTORY")
    target = Path(sys.argv[1]).resolve()
    target.mkdir(parents=True, exist_ok=True)
    urls = []
    for component, modules in COMPONENTS.items():
        page = urljoin(BASE, component)
        parser = Links()
        parser.feed(urlopen(page).read().decode())
        matches = [name for name in parser.links if name.endswith(".7z")
                   and any(module in name for module in modules)]
        if len(matches) != len(modules):
            raise RuntimeError(f"Unexpected archive list at {page}: {matches}")
        urls.extend(urljoin(page, name) for name in matches)

    with tempfile.TemporaryDirectory(prefix="motioncam-qt-android-") as temp:
        temporary = Path(temp)
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda url: download(url, temporary / url.rsplit("/", 1)[-1]), urls))
        for archive in temporary.glob("*.7z"):
            run(["7z", "x", "-y", "-bso0", f"-o{target}", str(archive)], check=True)
    print(f"Installed Qt Android arm64 kit in {target}")


if __name__ == "__main__":
    main()
