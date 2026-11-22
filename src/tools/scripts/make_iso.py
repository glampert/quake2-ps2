#!/usr/bin/env python3
# ================================================================================================
# File: make_iso.py
# Brief: Packages a disc build of the game (make iso / make iso_dbg) and your own game data into a
#        PS2 disc image, for Open PS2 Loader (OPL) or for booting in PCSX2.
#
# The repository ships no game data, so the image is made from the player's own baseq2/. This is a
# plain ISO9660 writer in the standard library alone, rather than a front end to mkisofs/xorriso,
# so the script runs wherever Python 3 does and needs nothing installed. The image is:
#
#   SYSTEM.CNF            BOOT2 = cdrom0:\Q2PS_319.00;1 - what the BIOS, PCSX2 and OPL boot. OPL
#                         also takes the boot file's name as the game's ID, the key of its per-game
#                         settings, virtual memory card and cheats (at most 11 characters).
#   Q2PS_319.00           the ELF, which must be a disc build (it looks for the data on the disc).
#   BASEQ2/               the game data, picked as make_usb_image.py picks it: of a pakN.pak and
#                         pakN.pkz pair only the pkz (or the pak, with --uncompressed-paks), no
#                         unpacked pak0/ tree, the music's .adp tracks (a track's .wav only when it
#                         has no .adp, which the game then plays instead), and none of what a run
#                         leaves behind (save/, scrnshot/, logs).
#
# Names are written in upper case to ISO9660 level 2 (up to 30 characters, which the players' skins
# need); the game's disc reader (ps2/fs/cdvd_device.h) matches them without regard to case or the
# ";1" version. Directories come first, then the files, in an order that keeps the boot files and
# the packs at the start of the disc. The same inputs make the same image, byte for byte.
#
# This source code is released under the GNU GPL v2 license.
# Check the accompanying LICENSE file for details.
# ================================================================================================

"""Create a PS2 disc image from a disc build of the game and a baseq2 directory.

Build the ELF with `make iso` (release) or `make iso_dbg` (debug), which also run this script on
the repository's baseq2/. To make an image from other files:

    make_iso.py --elf quake2.elf --data /path/to/baseq2 -o quake2.iso

For OPL, copy the image into the CD folder of the USB drive, hard disk or network share
(DVD when it is over 700 MB). In PCSX2, boot the image as a disc.
"""

import argparse
import datetime
import os
from pathlib import Path
import re
import struct
import sys
import tempfile


SECTOR_BYTES = 2048
SYSTEM_AREA_SECTORS = 16
CD_MAX_BYTES = 700 * 1024 * 1024  # an 80-minute CD-R
MAX_FILE_BYTES = (1 << 32) - 1    # one ISO9660 extent

DEFAULT_BOOT_NAME = "Q2PS_319.00"
VOLUME_ID = "QUAKE2"
SYSTEM_ID = "PLAYSTATION"

# Set into the disc build's ELF (ps2/fs/cdvd_device.cpp). Any other ELF can't find its data on a
# disc, so the script refuses it unless told otherwise.
DISC_BUILD_MARKER = b"Quake II PS2 disc build"

# ISO9660 d-characters, and the level 2 limits: a file identifier of up to 30 characters, name and
# extension together, and directory identifiers of up to 31. Eight directory levels at most.
D_CHARACTERS = re.compile(r"^[A-Z0-9_]*$")
MAX_FILE_ID_CHARS = 30
MAX_DIR_ID_CHARS = 31
MAX_DIR_DEPTH = 8

# What a run of the game leaves in baseq2/, and what no PS2 build reads.
SKIPPED_DIRS = {"save", "scrnshot"}
SKIPPED_FILES = {"quake2.log", "frame_log.txt", "io_log.txt", "condump.txt", "stats.log"}
SKIPPED_SUFFIXES = {".dll", ".so", ".dylib", ".exe"}


# ------------------------------------------------------------------------------------------------
# Command line
# ------------------------------------------------------------------------------------------------

def ParseArgs(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elf", type=Path, required=True,
                        help="the disc build's stripped ELF (build/iso_release/quake2.elf)")
    parser.add_argument("--data", type=Path, required=True,
                        help="the baseq2 directory to put on the disc")
    parser.add_argument("-o", "--output", type=Path, required=True, help="the image to write")
    parser.add_argument("-f", "--force", action="store_true", help="replace an existing image")
    parser.add_argument("--uncompressed-paks", action="store_true",
                        help="of a pakN.pak/pakN.pkz pair, put the pak on the disc, not the pkz")
    parser.add_argument("--boot-name", default=DEFAULT_BOOT_NAME,
                        help=f"the ELF's name on the disc, OPL's game ID (default {DEFAULT_BOOT_NAME})")
    parser.add_argument("--vmode", choices=("NTSC", "PAL"), default="NTSC",
                        help="SYSTEM.CNF's video mode; the game picks its own either way")
    parser.add_argument("--any-elf", action="store_true",
                        help="accept an ELF that isn't a disc build (it won't find its data)")
    return parser.parse_args(argv)


# ------------------------------------------------------------------------------------------------
# Choosing the files
# ------------------------------------------------------------------------------------------------

class Entry:
    """A file or directory of the image. Directories hold their children by ISO identifier."""

    def __init__(self, name, source=None, size=0, data=None, isDir=False):
        self.name = name        # as on disk, for messages
        self.source = source    # Path to copy from, or None
        self.data = data        # bytes, for the files the script makes itself
        self.size = size
        self.isDir = isDir
        self.children = {}      # identifier -> Entry
        self.parent = None
        self.identifier = b""
        self.sector = 0
        self.dirBytes = 0       # a directory's extent size
        self.number = 0         # a directory's path table number, from 1


def OmittedPacks(gameData, uncompressedPaks):
    # The game loads pakN.pkz instead of pakN.pak when both are there, so one of a pair is dead
    # weight: the pak by default, the pkz when the image is to load uncompressed.
    omitted = set()
    for index in range(10):
        pak = gameData / f"pak{index}.pak"
        pkz = gameData / f"pak{index}.pkz"
        if pak.is_file() and pkz.is_file():
            omitted.add(pkz.name if uncompressedPaks else pak.name)
    return omitted


def MusicFileWanted(name, namesHere):
    """Whether a file of the music folder goes on the disc: a track's .adp, or its .wav when there is
    no .adp of it, which the game plays instead (ps2/audio/cd_audio.cpp). namesHere holds the
    folder's file names in lower case."""
    path = Path(name.lower())
    if path.suffix == ".adp":
        return True
    return path.suffix == ".wav" and (path.stem + ".adp") not in namesHere


def DataFiles(gameData, uncompressedPaks):
    """The files of gameData that go on the disc, as paths relative to it, sorted."""
    hasPak0 = (gameData / "pak0.pak").is_file() or (gameData / "pak0.pkz").is_file()
    if not hasPak0:
        raise ValueError(f"{gameData} has neither pak0.pak nor pak0.pkz")
    omittedPacks = OmittedPacks(gameData, uncompressedPaks)

    chosen = []
    for directory, dirNames, fileNames in os.walk(gameData):
        here = Path(directory)
        relative = here.relative_to(gameData)
        topLevel = relative == Path(".")
        inMusic = not topLevel and relative.parts[0].lower() == "music"

        kept = []
        for name in dirNames:
            if name.startswith("."):
                continue
            if topLevel and (name.lower() in SKIPPED_DIRS or name == "pak0"):
                continue
            kept.append(name)
        dirNames[:] = sorted(kept)

        lowerNames = {name.lower() for name in fileNames}
        for name in sorted(fileNames):
            lower = name.lower()
            if name.startswith(".") or lower in SKIPPED_FILES or Path(lower).suffix in SKIPPED_SUFFIXES:
                continue
            if topLevel and name in omittedPacks:
                continue
            if inMusic and not MusicFileWanted(name, lowerNames):
                continue
            path = here / name
            if path.is_file():
                chosen.append(path.relative_to(gameData))
    return chosen


def Identifier(name, isDir):
    """The ISO9660 identifier of a file or directory name, or a ValueError saying why it has none."""
    upper = name.upper()
    if isDir:
        if not D_CHARACTERS.match(upper) or not 0 < len(upper) <= MAX_DIR_ID_CHARS:
            raise ValueError(f"directory name '{name}' can't go on an ISO9660 disc "
                             f"(A-Z, 0-9 and _ only, up to {MAX_DIR_ID_CHARS} characters)")
        return upper.encode("ascii")

    stem, dot, extension = upper.partition(".")
    if (not D_CHARACTERS.match(stem) or not D_CHARACTERS.match(extension) or
            "." in extension or not (stem or extension) or len(stem) + len(extension) > MAX_FILE_ID_CHARS):
        raise ValueError(f"file name '{name}' can't go on an ISO9660 disc "
                         f"(A-Z, 0-9 and _ with one '.', up to {MAX_FILE_ID_CHARS} characters)")
    return f"{stem}.{extension};1".encode("ascii")


def SortKey(entry):
    # ECMA-119 9.3: by name, then extension, each padded with spaces, which byte order gives, as
    # every d-character sorts above the space.
    stem, _, rest = entry.identifier.partition(b".")
    return (stem, rest.partition(b";")[0])


def AddEntry(directory, entry):
    entry.identifier = Identifier(entry.name, entry.isDir)
    if entry.identifier in directory.children:
        raise ValueError(f"'{entry.name}' and '{directory.children[entry.identifier].name}' have the "
                         f"same name on the disc, which ignores case")
    entry.parent = directory
    directory.children[entry.identifier] = entry
    return entry


def BuildTree(elf, gameData, files, bootName, systemCnf):
    root = Entry("", isDir=True)
    AddEntry(root, Entry("SYSTEM.CNF", data=systemCnf, size=len(systemCnf)))
    AddEntry(root, Entry(bootName, source=elf, size=elf.stat().st_size))

    baseq2 = AddEntry(root, Entry("baseq2", isDir=True))
    for relative in files:
        directory = baseq2
        for part in relative.parts[:-1]:
            identifier = Identifier(part, True)
            directory = directory.children.get(identifier) or AddEntry(directory, Entry(part, isDir=True))
        if len(relative.parts) + 1 > MAX_DIR_DEPTH:
            raise ValueError(f"'{relative}' is nested deeper than ISO9660's {MAX_DIR_DEPTH} levels")
        source = gameData / relative
        size = source.stat().st_size
        if size > MAX_FILE_BYTES:
            raise ValueError(f"'{relative}' is over 4 GiB, which one ISO9660 extent can't hold")
        AddEntry(directory, Entry(relative.parts[-1], source=source, size=size))
    return root


def Directories(root):
    """Every directory in path table order: by depth, then parent number, then name. Numbers them."""
    ordered = [root]
    level = [root]
    while level:
        nextLevel = []
        for directory in level:
            nextLevel.extend(sorted((e for e in directory.children.values() if e.isDir), key=SortKey))
        ordered.extend(nextLevel)
        level = nextLevel
    for number, directory in enumerate(ordered, start=1):
        directory.number = number
    return ordered


def FileOrder(root):
    """The files, in the order they are laid out on the disc."""
    files = []

    def Walk(directory, path):
        for entry in sorted(directory.children.values(), key=SortKey):
            if entry.isDir:
                Walk(entry, path + (entry.name.lower(),))
            else:
                files.append((path + (entry.name.lower(),), entry))

    Walk(root, ())

    # The boot files, then baseq2's loose files and the players' skins (all small), the packs, the
    # music, the cinematics, and anything else. The packs and the music are read in turn while a
    # level plays, so on a real drive they sit side by side.
    def Group(path):
        if len(path) == 1:
            return 0
        if len(path) == 2:
            return 3 if re.match(r"^pak\d\.pk[az]$", path[1]) else 1
        return {"players": 2, "music": 4, "video": 5}.get(path[1], 6)

    files.sort(key=lambda item: (Group(item[0]), item[0]))
    return [entry for _, entry in files]


# ------------------------------------------------------------------------------------------------
# ISO9660 structures (ECMA-119)
# ------------------------------------------------------------------------------------------------

def Both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def Both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def Padded(text, length):
    data = text.encode("ascii")
    if len(data) > length:
        raise ValueError(f"'{text}' is longer than its {length}-byte field")
    return data.ljust(length, b" ")


def RecordDate(when):
    return bytes((when.year - 1900, when.month, when.day, when.hour, when.minute, when.second, 0))


def VolumeDate(when):
    return when.strftime("%Y%m%d%H%M%S").encode("ascii") + b"00" + b"\x00"


UNUSED_VOLUME_DATE = b"0" * 16 + b"\x00"


def RecordBytes(identifierBytes):
    length = 33 + identifierBytes
    return length + (length & 1)


def DirectoryRecord(sector, size, isDir, identifier, when):
    length = RecordBytes(len(identifier))
    record = (bytes((length, 0)) + Both32(sector) + Both32(size) + RecordDate(when) +
              bytes((0x02 if isDir else 0x00, 0, 0)) + Both16(1) + bytes((len(identifier),)) + identifier)
    return record.ljust(length, b"\x00")


def DirectoryListing(directory):
    """(identifier, entry) pairs of a directory's extent, "." and ".." first."""
    listing = [(b"\x00", directory), (b"\x01", directory.parent or directory)]
    listing.extend((e.identifier, e) for e in sorted(directory.children.values(), key=SortKey))
    return listing


def DirectoryBytes(directory):
    # A record never crosses a sector: one that doesn't fit starts the next.
    used = 0
    for identifier, _ in DirectoryListing(directory):
        length = RecordBytes(len(identifier))
        if used % SECTOR_BYTES + length > SECTOR_BYTES:
            used += SECTOR_BYTES - used % SECTOR_BYTES
        used += length
    return Sectors(used) * SECTOR_BYTES


def DirectoryExtent(directory, when):
    data = bytearray()
    for identifier, entry in DirectoryListing(directory):
        record = DirectoryRecord(entry.sector, entry.dirBytes if entry.isDir else entry.size,
                                 entry.isDir, identifier, when)
        if len(data) % SECTOR_BYTES + len(record) > SECTOR_BYTES:
            data.extend(bytes(SECTOR_BYTES - len(data) % SECTOR_BYTES))
        data.extend(record)
    return bytes(data).ljust(directory.dirBytes, b"\x00")


def PathTable(directories, bigEndian):
    order = ">" if bigEndian else "<"
    table = bytearray()
    for directory in directories:
        identifier = directory.identifier if directory.parent else b"\x00"
        parent = directory.parent.number if directory.parent else 1
        table.extend(bytes((len(identifier), 0)) + struct.pack(order + "IH", directory.sector, parent))
        table.extend(identifier)
        if len(identifier) & 1:
            table.append(0)
    return bytes(table)


def PrimaryVolumeDescriptor(totalSectors, pathTableBytes, lTableSector, mTableSector, root, when):
    descriptor = (bytes((1,)) + b"CD001" + bytes((1, 0)) + Padded(SYSTEM_ID, 32) + Padded(VOLUME_ID, 32) +
                  bytes(8) + Both32(totalSectors) + bytes(32) + Both16(1) + Both16(1) +
                  Both16(SECTOR_BYTES) + Both32(pathTableBytes) +
                  struct.pack("<I", lTableSector) + struct.pack("<I", 0) +
                  struct.pack(">I", mTableSector) + struct.pack(">I", 0) +
                  DirectoryRecord(root.sector, root.dirBytes, True, b"\x00", when) +
                  Padded("", 128) + Padded("", 128) + Padded("", 128) + Padded("QUAKE II PS2 MAKE_ISO.PY", 128) +
                  Padded("", 37) + Padded("", 37) + Padded("", 37) +
                  VolumeDate(when) + VolumeDate(when) + UNUSED_VOLUME_DATE + VolumeDate(when) +
                  bytes((1, 0)))
    assert len(descriptor) == 883, len(descriptor)
    return descriptor.ljust(SECTOR_BYTES, b"\x00")


def TerminatorDescriptor():
    return (bytes((255,)) + b"CD001" + bytes((1,))).ljust(SECTOR_BYTES, b"\x00")


def Sectors(byteCount):
    return (byteCount + SECTOR_BYTES - 1) // SECTOR_BYTES


# ------------------------------------------------------------------------------------------------
# Writing the image
# ------------------------------------------------------------------------------------------------

def Layout(root):
    """Places every structure and file; returns what writing the image needs."""
    directories = Directories(root)
    for directory in directories:
        directory.dirBytes = DirectoryBytes(directory)

    # Each path table's size doesn't depend on where the directories go, only on their names.
    pathTableBytes = len(PathTable(directories, False))
    lTableSector = SYSTEM_AREA_SECTORS + 2  # after the primary descriptor and the terminator
    mTableSector = lTableSector + Sectors(pathTableBytes)
    sector = mTableSector + Sectors(pathTableBytes)

    for directory in directories:
        directory.sector = sector
        sector += directory.dirBytes // SECTOR_BYTES

    files = FileOrder(root)
    for entry in files:
        # An empty file takes no sector; it points at the first one, which is on the disc.
        entry.sector = sector if entry.size > 0 else 0
        sector += Sectors(entry.size)

    return directories, files, pathTableBytes, lTableSector, mTableSector, sector


def CopyFile(entry, out):
    if entry.data is not None:
        out.write(entry.data)
        copied = len(entry.data)
    else:
        copied = 0
        with open(entry.source, "rb") as source:
            while chunk := source.read(1024 * 1024):
                out.write(chunk)
                copied += len(chunk)
    if copied != entry.size:
        raise ValueError(f"{entry.source} changed size while the image was written")
    out.write(bytes(Sectors(copied) * SECTOR_BYTES - copied))


def WriteImage(root, out, when):
    directories, files, pathTableBytes, lTableSector, mTableSector, totalSectors = Layout(root)

    out.write(bytes(SYSTEM_AREA_SECTORS * SECTOR_BYTES))
    out.write(PrimaryVolumeDescriptor(totalSectors, pathTableBytes, lTableSector, mTableSector, root, when))
    out.write(TerminatorDescriptor())
    for bigEndian in (False, True):
        table = PathTable(directories, bigEndian)
        out.write(table.ljust(Sectors(pathTableBytes) * SECTOR_BYTES, b"\x00"))
    for directory in directories:
        out.write(DirectoryExtent(directory, when))
    for entry in files:
        CopyFile(entry, out)

    if out.tell() != totalSectors * SECTOR_BYTES:
        raise AssertionError("the image's size doesn't match its layout")
    return totalSectors, len(files)


def SystemCnf(bootName, vmode):
    return (f"BOOT2 = cdrom0:\\{bootName};1\r\n"
            f"VER = 1.00\r\n"
            f"VMODE = {vmode}\r\n").encode("ascii")


def CheckElf(elf, anyElf):
    if not elf.is_file():
        raise ValueError(f"{elf} is missing; build it with 'make iso' or 'make iso_dbg'")
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{elf} is not an ELF")
    if not anyElf and DISC_BUILD_MARKER not in data:
        raise ValueError(f"{elf} is not a disc build, so it wouldn't find its data on the disc; "
                         f"build it with 'make iso' or 'make iso_dbg' (or pass --any-elf)")


def CreateImage(elf, gameData, output, force=False, uncompressedPaks=False,
                bootName=DEFAULT_BOOT_NAME, vmode="NTSC", anyElf=False):
    CheckElf(elf, anyElf)
    if not gameData.is_dir():
        raise ValueError(f"game data directory is missing: {gameData}")
    if output == gameData or gameData in output.parents:
        raise ValueError("output must be outside the game data directory")
    if output.exists() or output.is_symlink():
        if not force:
            raise ValueError(f"{output} already exists; pass --force to replace it")
        if output.is_dir():
            raise ValueError(f"output is a directory: {output}")
    if len(bootName) > 11 or Identifier(bootName, False).count(b".") != 1:
        raise ValueError(f"boot name '{bootName}' must be an 8.3 name of at most 11 characters")

    # The ELF's time stamps every record, so the same inputs make the same image.
    stamp = int(os.environ.get("SOURCE_DATE_EPOCH", int(elf.stat().st_mtime)))
    when = datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc)

    files = DataFiles(gameData, uncompressedPaks)
    root = BuildTree(elf, gameData, files, bootName, SystemCnf(bootName, vmode))

    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".quake2-iso-", dir=output.parent)
    try:
        with os.fdopen(descriptor, "wb") as out:
            totalSectors, fileCount = WriteImage(root, out, when)
        os.chmod(temporary, 0o644)
        if force:
            os.replace(temporary, output)
        else:
            os.link(temporary, output)  # fails, rather than replacing, if one appeared meanwhile
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

    imageBytes = totalSectors * SECTOR_BYTES
    print(f"Created {output}: {fileCount} files, {imageBytes / (1024 * 1024):.1f} MiB.")
    print(f"Boots cdrom0:\\{bootName};1 (OPL game ID {bootName}).")
    if imageBytes > CD_MAX_BYTES:
        print("Over 700 MiB, so too big for a CD: OPL's DVD folder, not CD.")
    return imageBytes


def Main(argv=None):
    args = ParseArgs(argv)
    output = args.output.expanduser().absolute()
    output = output.parent.resolve() / output.name
    try:
        CreateImage(args.elf.expanduser().resolve(), args.data.expanduser().resolve(), output,
                    args.force, args.uncompressed_paks, args.boot_name, args.vmode, args.any_elf)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(Main())
