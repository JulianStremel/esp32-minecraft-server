#!/usr/bin/env python3
"""Packages a firmware build for a GitHub release and the web flasher.

  tools/release/package.py <build dir> <board> <version> <out dir>

Writes into <out dir>:
  mcserver-<board>-bootloader.bin, -partition-table.bin, -app.bin  (flashed separately)
  mcserver-<board>-merged.bin    (one image, flashed at 0x0)
  manifest-<board>.json          (ESP Web Tools; parts relative to the manifest)
The offsets come from the build's flash_args, so they cannot drift from the build.
"""
import json
import os
import shutil
import sys

CHIP_FAMILY = {"esp32s3": "ESP32-S3", "esp32p4": "ESP32-P4"}
PART_NAMES = {"bootloader.bin": "bootloader", "partition-table.bin": "partition-table", "mcserver.bin": "app"}


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    build, board, version, out = sys.argv[1:]
    chip = board.rsplit("-", 1)[0]
    if chip not in CHIP_FAMILY:
        sys.exit(f"unknown board {board}")
    os.makedirs(out, exist_ok=True)
    parts = []
    for line in open(os.path.join(build, "flash_args")):
        fields = line.split()
        if len(fields) != 2 or not fields[0].startswith("0x"):
            continue
        offset, path = int(fields[0], 16), fields[1]
        base = os.path.basename(path)
        if base not in PART_NAMES:
            sys.exit(f"unexpected flash part {path}")
        name = f"mcserver-{board}-{PART_NAMES[base]}.bin"
        shutil.copyfile(os.path.join(build, path), os.path.join(out, name))
        parts.append({"path": name, "offset": offset})
    if sorted(p["path"] for p in parts) != sorted(f"mcserver-{board}-{n}.bin" for n in PART_NAMES.values()):
        sys.exit(f"flash_args lists {parts}, expected bootloader, partition table and app")
    shutil.copyfile(os.path.join(build, "merged-binary.bin"), os.path.join(out, f"mcserver-{board}-merged.bin"))
    manifest = {
        "name": "ESP32 Minecraft server",
        "version": version,
        "new_install_prompt_erase": True,
        # seconds ESP Web Tools waits for Improv after flashing (the board boots in ~2 s)
        "new_install_improv_wait_time": 15 if chip == "esp32s3" else 0,
        "builds": [{"chipFamily": CHIP_FAMILY[chip], "parts": sorted(parts, key=lambda p: p["offset"])}],
    }
    with open(os.path.join(out, f"manifest-{board}.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"packaged {board} {version}: " + ", ".join(f"{p['path']}@{p['offset']:#x}" for p in manifest["builds"][0]["parts"]))


if __name__ == "__main__":
    main()
