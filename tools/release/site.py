#!/usr/bin/env python3
"""Assembles the web flasher site.

  tools/release/site.py <site dir> <releases dir> [<dev dir>]

<releases dir> holds one directory per release tag with that release's files
(tools/release/package.py's output; downloaded by pages.yml), newest first in
<releases dir>/order.txt. <dev dir> is a package of the current main branch.
Writes <site dir>/index.html, firmware/<id>/... and channels.json, which lists what the
page offers: the releases (the newest stable one first) and the development build.
"""
import json
import os
import shutil
import sys

BOARD = "esp32s3-8"   # the flasher's board (the P4 has no WiFi to set up: download only)
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def copy_package(src, dst):
    manifest = os.path.join(src, f"manifest-{BOARD}.json")
    if not os.path.exists(manifest):
        return None
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        if name.startswith(f"mcserver-{BOARD}-") or name == f"manifest-{BOARD}.json":
            shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
    return json.load(open(manifest))


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    site, releases = sys.argv[1], sys.argv[2]
    dev = sys.argv[3] if len(sys.argv) == 4 else None
    os.makedirs(os.path.join(site, "firmware"), exist_ok=True)
    shutil.copyfile(os.path.join(ROOT, "web", "index.html"), os.path.join(site, "index.html"))
    channels = []
    order = os.path.join(releases, "order.txt")
    tags = [l.split() for l in open(order)] if os.path.exists(order) else []
    for fields in tags:
        tag, pre = fields[0], len(fields) > 1 and fields[1] == "true"
        m = copy_package(os.path.join(releases, tag), os.path.join(site, "firmware", tag))
        if not m:
            print(f"{tag}: no {BOARD} package, skipped")
            continue
        channels.append({"id": tag, "label": tag + (" (pre-release)" if pre else ""), "prerelease": pre,
                         "manifest": f"firmware/{tag}/manifest-{BOARD}.json",
                         "merged": f"firmware/{tag}/mcserver-{BOARD}-merged.bin",
                         "notes": f"https://github.com/JulianStremel/esp32-minecraft-server/releases/tag/{tag}"})
    # the newest stable release is the default
    channels.sort(key=lambda c: c["prerelease"])
    if dev:
        m = copy_package(dev, os.path.join(site, "firmware", "dev"))
        if m:
            channels.append({"id": "dev", "label": f"development build ({m['version']})", "prerelease": True,
                             "manifest": f"firmware/dev/manifest-{BOARD}.json",
                             "merged": f"firmware/dev/mcserver-{BOARD}-merged.bin",
                             "notes": "https://github.com/JulianStremel/esp32-minecraft-server/commits/main"})
    if not channels:
        sys.exit("nothing to offer: no release and no development build")
    with open(os.path.join(site, "channels.json"), "w") as f:
        json.dump(channels, f, indent=2)
    print("channels:", ", ".join(c["id"] for c in channels))


if __name__ == "__main__":
    main()
