---
paths:
  - "src/ps2/system/iop_boot.*"
  - "src/ps2/fs/**"
  - "src/ps2/debug/log_file.*"
  - "src/ps2/tests/aio_test.*"
  - "src/tools/scripts/make_usb_image.py"
---

# Testing the storage boot paths: USB, HDD, launchers, the background loader

Companion to [testing-pcsx2.md](testing-pcsx2.md). PCSX2 setup basics (HostFs, logging, the
card) are there; the driver traps are in [ps2-platform.md](ps2-platform.md).

## The background loader test

- `ps2_testaio 1`: the background loader (fs/async_io.h) reads 13 files through the loader and
  through `FS_LoadFile` and compares them, twice (the second pass throttled to 900 KB/s), timing a
  main-thread read of `pics/conchars.pcx` every frame meanwhile; prints each file's source kind and
  request-to-landed time, the loader's read throughput and `AioTest: PASS`/`FAIL`. ~15 s in PCSX2
  from the pkz (2026-10-26: 24 comparisons, 0 failures, loose/pkz-stored/pkz-deflated seen, the
  probe read 4 ms at worst). It needs a run on a console from USB too: PCSX2 has no data cache,
  so a destination sharing a cache line with something else only shows there.
- **Emulating a USB stick's read rate in PCSX2:** `ps2_aio_throttle_kbps 900` in config.cfg paces
  the background loader (not the main thread's own reads) like the user's stick: 5 ms plus the
  transfer at 900 KB/s per 32 KB chunk. A perf capture with it shows whether the cinematic ring
  keeps up (2026-10-26: identical to the unthrottled capture); 200 KB/s, below ntro's bitrate,
  shows the starvation path (every cinematic row waits in FsIo). The `Read-ahead stream on ...`
  line is a `Com_DPrintf`, so it needs `developer 1`, which the perf run turns off.

## USB boot

- **The USB boot branch:** run a copy of the ELF from a directory with no `baseq2/` (e.g. the
  scratchpad), with HDD disabled. The `host:` probe misses, so the IOP inventory (PCSX2's BIOS
  leaves nothing in the way, so no reset), optional HDD probe, BDM module chain and 10 s
  `mass0:`-`mass9:` poll all run, ending on the "No game data found!" screen with
  `USB volumes mounted: none` when no emulated USB drive is attached. The run stops before
  `Qcommon_Init` and never touches `config.cfg`. Its
  `argv[0]` is an absolute host path, longer than the 50-char ELF folder cap, so only the
  root is searched.
- **PCSX2 emulated USB storage (3.20 debug runtime verified via uLaunchELF):**
  `[USB2] Type = Msd`, `Msd_subtype = 0` (Iomega Zip-100 / Generic), and
  `Msd_ImagePathMsd = /absolute/path/usb.img` attach a file-backed mass-storage device;
  USB1 can remain `hidkbd`. PCSX2 uses raw 512-byte sectors and opens the image `r+b`,
  so use a writable raw disk image; an MBR/FAT32 layout is a conservative test choice.
  Put `baseq2/` at its root for a directly launched host ELF. Disable `[EmuCore] HostFs`
  or launch from a folder without host game data, since the successful host probe skips
  storage bring-up. A directly launched ELF (a host path) searches the HDD first, so
  also disable HDD if it has a matching installation; one launched from `mass:` never
  searches it. The embedded BDM modules probe `mass0:` through `mass9:`.
  Attaching the image doesn't launch its ELF; loading the ELF itself from `mass:` needs
  a homebrew launcher such as wLaunchELF. See PCSX2's
  [usb-msd.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/USB/usb-msd/usb-msd.cpp)
  and [USB.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/USB/USB.cpp).
- **Creating a USB image on macOS:** `hdiutil create -size 256m -fs 'MS-DOS FAT32'
  -layout MBRSPUD -volname Q2PS2DEBUG -srcfolder <package-folder> -format UDTO
  -nospotlight <output>.cdr`, then rename `.cdr` to `.img`. Despite the UDTO format's
  "DVD/CD master" name, the explicit filesystem/layout produce raw MBR/FAT32 sectors,
  without a DMG trailer or an ISO9660 filesystem. The 3.20 debug package produced a
  256 MiB image with `quake2.elf` and `baseq2/` at the root; its partition/boot signatures,
  FAT copies, and SHA-256 hashes of all four packaged files were verified by reading the
  FAT32 image directly. Launch `mass:/quake2.elf` in uLaunchELF. The user verified this
  on 2026-10-07; the log reports `mass0:/baseq2` ready after 600 ms and reads from its pak.
  Initial debug-run loading-screen summaries report 120-131 seconds. These are instrumented
  observations from one session, not a storage-throughput benchmark. Its emulator log was
  preserved as `github_rel_3.20/q2ps2_debug_3.20_usb-emulog.txt` before another launch could
  overwrite it. Keep build, map, config, and logging settings matched in comparisons.
- **Reusable test USB image:** `python3 src/tools/scripts/make_usb_image.py` packages the
  existing stripped debug/release ELFs at the volume root as `quake2_debug.elf` and
  `quake2_release.elf`, beside `baseq2/`; unstripped ELFs are omitted. It copies the working
  data/config except `baseq2/pak0/` when `pak0.pak` is present, and includes only `.adp`
  files under `baseq2/music/` (case-insensitive, including subdirectories). macOS `hdiutil`
  produces an auto-sized writable MBR/FAT32 image with volume name `Q2PS2` at
  `build/quake2-usb.img`; `--output`, `--size-mib`, and `--force` override defaults.
  The initial full-data run produced an 832 MiB image: raw FAT32 verification checked all
  96 included files by SHA-256, with no missing/extra files or directories. Sixteen fixture
  cases cover selection, sizing, errors and safe replacement. Both ELFs now sit next to
  the shared data, requiring no folder fallback. Close PCSX2 before replacing an image.
- **Scripted runs from the emulated stick, no launcher** (2026-10-19): launch a host ELF
  from a folder with no `baseq2/` (the `host:` probe misses; HDD off), with `[USB2] Type = Msd`
  on the image. A USB boot reads the stick's config.cfg when it says `ps2_savedevice "usb"`, and
  leaves the card alone then; with `"mc"` (the shipped default) it reads the card's first, so with
  a card in, the stick's test cvars are ignored and autosaves go to the card, unless
  **`Slot1_Enable = false`**. Put the cvars in the image's `baseq2/config.cfg`:
  `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nobrowse -mountpoint <dir> <img>`,
  edit, `hdiutil detach <dir>`. Give each run an APFS clone of a pristine image (`cp -c`), so
  every run starts from the same bytes. Debug with the I/O log (`ps2_logfile "0"`), the first
  8 MapCycle loads from the pkz took 95.5 s in all (base1 22.4 s, 10.68 MB read); the boot
  found the data at 658 ms.

## HDD boot

- The current source embeds DEV9/ATAD/APA/PFS and probes HDD between `host:`
  and USB. ELFs packaged before HDD support need rebuilding.
  Enable `[DEV9/Hdd] HddEnable = true` and set `HddFile` to a PCSX2 HDD image. Create and
  format a new blank image with uLaunchELF's HDD Manager, create a main PFS partition
  (e.g. `+Q2PS2`, 512 MiB), and copy the new ELF and `baseq2/` side by side into it.
  With `HostFs = false`, launch it through uLaunchELF. The game mounts PFS itself, first
  unmounting whatever a launcher's PFS driver left at `pfs0:`. Successful bring-up logs
  `game data on pfs0:/.../baseq2 (HDD partition hdd0:+Q2PS2)` and keeps that mount alive.
  Canonical `hdd0:+Q2PS2:pfs:/dir/quake2.elf` and browser `hdd0:/+Q2PS2/dir/quake2.elf`
  paths retain the launch partition; bare `pfsN:` paths fall back to enumeration.
  The launch partition's folder/root comes first; other main PFS partitions are searched
  for the same folder before any roots. Missing HDD/drivers or no matching data falls
  through to USB. An ELF launched from `mass:` skips the HDD altogether (since 2026-10-12). The user verified emulated PFS boot/reads on 2026-10-07 and observed
  noticeably faster loading than USB. The missing HDD log still needs session-specific
  verification after adding HDD-only `fileXioSync` after log closes; see the PFS
  metadata-flush trap in [ps2-platform.md](ps2-platform.md). Builds from ed03e18 up to the
  2026-10-09 fix also stopped every USB/HDD log after its header line: they required
  `close() == 0`, and iomanX returns the slot number (see ps2-platform.md). The fix was
  verified on emulated USB (uLaunchELF, `mass:quake2_debug.elf`): a full 1349-line
  `quake2.log` from boot to quit, and no logging warning in emulog.

## Launcher states without a launcher

A small test ELF in the scratchpad (2026-10-12) brings the IOP up as a launcher would, then
starts `mass:/quake2_debug.elf` from the USB image with libelf-loader's
`LoadELFFromFileWithPartitionNoReset` (in `libelf-loader.a`, not declared in its header; declare
it by hand). It links `-lelf-loader -lpatches -lpad -lmc -lfileXio -lkernel` with the IRXs from
`$PS2SDK/iop/irx` embedded by `bin2c`, the way wLaunchELF's `Reset()` loads them. Launched with
`PCSX2 -batch -elf`, it runs from `host:`. Each state it made, and what the game did:

- wLaunchELF 2022-2026/05 (its BDM stack, ROM SIO2MAN and PADMAN, `mcman-old`/`mcserv-old`):
  no reset, every module reused, game data 44 ms after `main()` against 655 ms from a fresh
  IOP.
- The same, with pad port 0 left open: its DMA rewrote the game's `.text` until the boot
  closed the port (read through the uncached segment, 100 ms apart, before and after).
- The elf-loader with reset (wLaunchELF since 2026/05): the ROM's card drivers reused.
- wLaunchELF 2019 (`usbhdfsd`): the data read through it.
- ps2sdk's `sio2man` alone, and most of the IOP's memory taken: each reset, then booted.

Keep the launcher's `padArea` buffer and the ELF's address ranges in mind when reading a
probe: the launcher's addresses are the game's once it runs.

## Host harnesses for the boot and log code

- **Storage selection runtime harness:** include `iop_boot.cpp` with stub SDK headers
  and IOP/mount calls, then run from a directory holding folders literally named `pfs0:`,
  `mass0:`, `mass1:`... (macOS allows the colon). `fopen` then takes the probe paths as
  relative ones. The HDD implementation passed 84 cases under ASan/UBSan: launch-path
  parsing/bounds, host/HDD/USB precedence, partition filtering and mount lifetime,
  optional HDD failure fallback, mandatory initialization failures, and keyboard USB
  startup after HDD boot. `SyncGameDataDevice()` tests cover no-op before boot/host/USB/
  failed HDD, selected-mount sync and raw errors, and clearing stale state on another
  detection. This verifies selection logic, not the real drivers or DMA;
  `make` and `make release` remain the target compile/link checks.
- **HDD log sync runtime harness:** include `debug/log_file.cpp` with fake SDK headers,
  real host-backed open/seek/write/close wrappers and a mocked `SyncGameDataDevice()`.
  A host `close()` returns 0, unlike iomanX's slot number, so make the wrapper return a
  positive value on success or the harness can't see that trap;
  the boot harness checks its real fileXio implementation. Twenty-four ASan/UBSan cases
  verify header/append/fatal persistence ordering, no sync for host/USB or disabled
  logging, and disabling after open/seek/write/
  partial-write/close/sync failures. Every opened descriptor closes before HDD sync, even
  after a failed operation; an open failure never syncs. Real PFS persistence still needs
  an emulator run with the new ELF, checking `quake2.log` beside `baseq2/` after shutdown.
