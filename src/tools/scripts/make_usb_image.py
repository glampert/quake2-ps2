#!/usr/bin/env python3
# ================================================================================================
# File: make_usb_image.py
# Brief: Packages existing debug/release builds and game data into a PCSX2 USB test image.
#
# Uses macOS hdiutil to make a writable, raw MBR/FAT32 image, rather than a CD ISO. Both ELF
# configurations sit beside one baseq2 copy at the volume root. Only the stripped ELFs are copied.
# The unpacked baseq2/pak0 tree is redundant when pak0.pak is available, so only that directory
# is omitted in that case. Music includes only ADP tracks. No build is run by this script.
#
# This source code is released under the GNU GPL v2 license.
# Check the accompanying LICENSE file for details.
# ================================================================================================

"""Create build/quake2-usb.img on macOS from the current builds and baseq2.

Image layout:
    quake2_debug.elf
    quake2_release.elf
    baseq2/                   (working copy, including config.cfg; ADP music only)

Run `make` and `make release` first. In PCSX2, select this .img as the USB Mass Storage
Device backing image, then launch either ELF from uLaunchELF. Exit PCSX2 before replacing
an image it has attached.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


MIB = 1024 * 1024
MIN_SIZE_MIB = 256
MAX_FAT32_FILE_BYTES = (1 << 32) - 1


def ParseArgs(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--output", type=Path,
                        help="output image (default: <repo>/build/quake2-usb.img)")
    parser.add_argument("--size-mib", type=int,
                        help="image size in MiB; default: payload plus free space, at least 256")
    parser.add_argument("-f", "--force", action="store_true",
                        help="replace an existing image after successful creation")
    return parser.parse_args(argv)


def ValidateSources(repoRoot):
    gameData = repoRoot / "baseq2"
    if not gameData.is_dir():
        raise ValueError(f"game data directory is missing: {gameData}")
    for config in ("debug", "release"):
        elf = repoRoot / "build" / config / "quake2.elf"
        if not elf.is_file():
            command = "make" if config == "debug" else "make release"
            raise ValueError(f"{elf} is missing; run '{command}' first")
    return gameData


def StageFiles(repoRoot, gameData, staging):
    omitPak0 = (gameData / "pak0.pak").is_file() and (gameData / "pak0").is_dir()
    musicDirectory = gameData / "music"

    def IgnoreSourceFiles(directory, names):
        directory = Path(directory)
        ignored = ["pak0"] if omitPak0 and directory == gameData and "pak0" in names else []
        if directory == musicDirectory or musicDirectory in directory.parents:
            ignored.extend(name for name in names
                           if not (directory / name).is_dir() and Path(name).suffix.lower() != ".adp")
        return ignored

    shutil.copytree(gameData, staging / "baseq2", ignore=IgnoreSourceFiles)
    for config in ("debug", "release"):
        source = repoRoot / "build" / config / "quake2.elf"
        shutil.copy2(source, staging / f"quake2_{config}.elf")
    return omitPak0


def MeasurePayload(staging):
    totalBytes = 0
    entries = 0
    for path in staging.rglob("*"):
        entries += 1
        if path.is_file():
            size = path.stat().st_size
            if size > MAX_FAT32_FILE_BYTES:
                raise ValueError(f"file exceeds FAT32's 4 GiB limit: {path.relative_to(staging)}")
            totalBytes += size
    return totalBytes, entries


def ImageSizeMib(payloadBytes, entries, requestedSize):
    # Include directory/cluster overhead and FAT metadata before reserving free space. The
    # per-entry allowance also covers small loose assets, whose allocated size exceeds their bytes.
    overhead = entries * 4096 + max(16 * MIB, payloadBytes // 20)
    minimumSize = max(MIN_SIZE_MIB, (payloadBytes + overhead + MIB - 1) // MIB)
    if requestedSize is not None:
        if requestedSize < minimumSize:
            raise ValueError(f"--size-mib is too small; this payload needs at least {minimumSize} MiB")
        return requestedSize

    reserve = max(32 * MIB, payloadBytes // 5)
    desiredSize = max(MIN_SIZE_MIB, (payloadBytes + overhead + reserve + MIB - 1) // MIB)
    return ((desiredSize + 31) // 32) * 32


def CreateImage(repoRoot, output, requestedSize=None, force=False):
    gameData = ValidateSources(repoRoot)
    # The image must not become part of the game-data copy on subsequent invocations.
    if output == gameData or gameData in output.parents:
        raise ValueError("output must be outside the baseq2 directory")
    if output.exists() or output.is_symlink():
        if not force:
            raise ValueError(f"{output} already exists; pass --force to replace it")
        if output.is_dir():
            raise ValueError(f"output is a directory: {output}")
    if requestedSize is not None and requestedSize < MIN_SIZE_MIB:
        raise ValueError(f"--size-mib must be at least {MIN_SIZE_MIB}")
    hdiutil = shutil.which("hdiutil")
    if hdiutil is None:
        raise ValueError("hdiutil is required; this script creates images on macOS")

    output.parent.mkdir(parents=True, exist_ok=True)
    # Staging and the finished image share the destination filesystem, so publishing is atomic.
    # Failed hdiutil runs remove their temporary files and leave an existing output untouched.
    with tempfile.TemporaryDirectory(prefix=".quake2-usb-", dir=output.parent) as temporary:
        working = Path(temporary)
        staging = working / "files"
        omitPak0 = StageFiles(repoRoot, gameData, staging)
        payloadBytes, entries = MeasurePayload(staging)
        sizeMib = ImageSizeMib(payloadBytes, entries, requestedSize)
        rawImage = working / "usb.cdr"
        print(f"Creating {sizeMib} MiB FAT32 image ({payloadBytes / MIB:.1f} MiB of files)...",
              flush=True)
        if omitPak0:
            print("Omitting baseq2/pak0/ because baseq2/pak0.pak is present.", flush=True)
        subprocess.run([hdiutil, "create", "-size", f"{sizeMib}m", "-fs", "MS-DOS FAT32",
                        "-layout", "MBRSPUD", "-volname", "Q2PS2", "-srcfolder", str(staging),
                        "-format", "UDTO", "-nospotlight", str(rawImage)],
                       check=True, capture_output=True, text=True)
        if rawImage.stat().st_size != sizeMib * MIB:
            raise ValueError("hdiutil output is not the requested raw image size")
        rawImage.chmod(0o644)
        if force:
            os.replace(rawImage, output)
        else:
            # An atomic no-overwrite operation also protects against an image created while
            # hdiutil was running. TemporaryDirectory removes our remaining link afterwards.
            os.link(rawImage, output)
    print(f"Created {output}")
    print("ELFs: quake2_debug.elf and quake2_release.elf; adjacent game data: baseq2/")


def Main(argv=None):
    args = ParseArgs(argv)
    repoRoot = Path(__file__).resolve().parents[3]
    output = (args.output or repoRoot / "build" / "quake2-usb.img").expanduser().absolute()
    # Resolve the parent only: --force replaces a destination symlink rather than its target.
    output = output.parent.resolve() / output.name
    try:
        CreateImage(repoRoot, output, args.size_mib, args.force)
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "no diagnostic output").strip()
        print(f"error: hdiutil failed (exit {error.returncode}): {detail}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())
