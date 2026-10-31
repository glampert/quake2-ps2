---
paths:
  - "src/ps2/**"
---

# PS2 platform and ps2sdk facts

## Don't trust ps2sdk on sight

- **libc stubs.** libcglue stubs a number of libc functions to fail. `sysconf()` returns -1
  with `EINVAL` for *every* name, `_SC_PAGE_SIZE` included, and `tcgetattr`/`tcsetattr` etc.
  return -1/`ENOSYS`. The vendored dlmalloc took its page size from `sysconf`, got
  `(size_t)-1`, and the ELF black-screened. `dlmalloc.c` now pins
  `#define malloc_getpagesize ((size_t)4096)`. Before relying on a libc call, grep
  `~/ps2dev/src/ps2dev/build/ps2sdk/ee/libcglue/src/glue.c`. Prefer pinned constants to
  runtime libc queries.
- **Register-packing macros.** `VU_GS_PRIM`/`VU_GS_GIFTAG` (packet2_utils.h) don't
  parenthesize their parameters. `blended ? 1 : 0` as ABE parsed as `blended ? 1 : (0 << 6)`,
  which put the bit inside PRIM, where it was already set, so blending silently never
  happened. Use `GIF_SET_PRIM`/`GIF_SET_TAG` (gif_tags.h), which parenthesize and mask.
  `gs_gp.h` is not uniformly safe either. `GS_SET_DIMX` masks the 3-bit signed dither entries
  with `0x3`, which turns the matrix into a brightening bias, and libdraw's
  `draw_dither_matrix` inherits that. Pack DIMX by hand: `(u64)((u32)v & 0x7u) << (i * 4)`.
  Hoist computed arguments into named locals, read the macro body, and if a GS feature
  "isn't enabled", compile the macro standalone on the host and inspect the bits.
- `packet_init`/`packet2_create` each `calloc` a header and `memalign(64)` the buffer. Read the
  SDK source when internal behaviour matters.
- **stdio sizes its buffers from `st_blksize`** since the 2026-10-19 install: newlib's
  `ee-v4.6.0` is built with `HAVE_BLKSIZE` (2026-10-03), and ps2sdk reports `st_blksize`
  (`8745ab1`). A regular file reports 16 KB through ROM FILEIO (`host:`), and fileXio's read
  buffer through fileXio (`mass:`, `pfs:`): 64 KB on this port's USB and HDD boots
  (`SetFileXioBuffer`). Other files report 0. Before, a stream newlib sized itself got 1 KB
  (`BUFSIZ`), and every stream got `__SNPT`, so newlib optimized no `fseek`: each one dropped
  the buffer and went to the device. Now a stream newlib sizes itself, or one whose `setvbuf`
  size equals `st_blksize`, keeps `__SOPT`. A seek inside its buffer is free. Any other seek,
  the first after an open included, reads the aligned `st_blksize` block below the target: up
  to 64 KB more than asked for. A `setvbuf` of any other size keeps the old behaviour, so
  `FS_SetStreamBuffer`'s seeks stay in its buffer only on USB/HDD, for files of 64 KB or more.
  Streams opened without `setvbuf` (config.cfg, condump, demos, screenshots, host saves) now
  hold 16-64 KB instead of 1 KB.
  Same code on the old and new SDK in PCSX2 (2026-10-19): the MapCycle's memory, the perf
  capture and eight USB loads' times didn't move. The console cost of the first-seek over-read
  is unmeasured.
- **libcglue rewrites every path `open()` gets** (`__path_absolute`, cwd.c). Device paths come
  out as they went in (`host:x` gains its `/`), but a path without a device takes the cwd and a
  `\` separator since `cdc7d28` (it read an uninitialized variable before). Keep every path
  device-prefixed, as the game's bases (`host:`, `host:.`, `massN:`, `pfs0:`) are.
- **`close()` on a fileXio path (`mass:`, `pfs:`) returns a positive number on success.**
  `iomanX_close` returns `f - file_table`, the descriptor's slot (`HANDLE_RESULT_RETURN_FD`
  in iomanx/src/iomanX.c), and libcglue's `__transform_errno` passes any non-negative value
  through. Only `< 0` is a failure. `host:` goes through ROM FILEIO and returns 0, so PCSX2
  host runs hide it. A `close(fd) == 0` check in `log_file.cpp` turned the log off after its
  header line on every USB/HDD run from ed03e18 until 2026-10-09. newlib's `fclose` checks
  `< 0` and is fine. bdmfs_fatfs's `write` returns 0, not an error, when FatFs fails.

## EE CPU and FPU

- **No IEEE Inf/NaN on COP1.** Divide by zero saturates to ±FLT_MAX, denormals flush to zero,
  and rounding is toward zero. `Normalize(zero)` gives `(0,0,0)` on target but NaN on a host.
  This once vetoed valid ears in polygon triangulation: 714 faces with holes on target versus
  4 on host. Guard degenerate vectors explicitly. Never rely on NaN propagation.
- The compiler folds `constexpr` float math in the *target's* round-toward-zero semantics.
  `math::kPI` folds to `0x40490FDA`, not `...DB`. A host harness won't reproduce shipped
  constant bytes exactly, so check the ELF's `.rodata` instead.
- **No double-precision FPU.** `double` is libgcc soft-float. The engine C builds with
  `-fsingle-precision-constant` and `ps2/math/math_c.h`. Backend code uses `f` suffixes, and
  `-Wdouble-promotion` enforces them. Soft-float doubles were the cause of almost all
  dropped frames.
- `__builtin_ctz`/`__builtin_clz` are **libgcc calls** (the R5900 has no count-zeros
  instruction). Use `ps2::tex::Log2` (`plzcw`) or compares in hot paths.

## SIF DMA

- Any EE buffer that is a SIF DMA target (`SifRpcGetOtherData`, `sceSifSetDma`, memory card
  bounce buffers) must be `alignas(64)` and sized in whole 64-byte units. The DMAC ignores
  low address bits, and 64 bytes is the D-cache line, so a neighbour's dirty line can be
  written back over the DMA'd bytes. A plain `static char[128]` only gets 8-byte alignment.
  Check with `mips64r5900el-ps2-elf-nm build/debug/quake2_unstripped.elf | grep <sym>`.
  `SyncDCache(buf, buf + size)` before the transfer is the matching half.
- **`dma_channel_send_chain` writes back only the tag buffer** (`data` to `data + qwc`), never
  the data its REF tags point at. Anything the CPU wrote and a chain REFs needs its own
  `SyncDCache` first, or the DMAC reads stale RAM. PCSX2 has no data cache, so only a console
  shows it, and whether the lines happen to be written back in time varies from build to build.
  `gs::UploadCluts` missed it until 2026-10-10. A console run then drew only untextured fills
  (the red damage flash) with everything palettized gone, loading screen and HUD included,
  while a build a day earlier had drawn its loading screen. With the flush, a release build
  draws and plays on the console (2026-10-10). `UploadTexture` flushes on `dirtyPixels`, and
  the frame chain does a `FlushCache(0)` per kick.

## IOP modules, ROM FILEIO, memory card

- Boot (`system/iop_boot.cpp`): the `host:` fast path doesn't reset the IOP.
  `sbv_patch_enable_lmb()` alone lets `SifExecModuleBuffer` load embedded IRX without
  disturbing open `host:` handles. **Every console boot resets the IOP** and loads the game's
  own drivers (since 2026-10-31, the user's call, after the launcher's usbhdfsd froze every run
  that went through it; see below). From 2026-10-12 to then, console boots took the IOP as the
  launcher left it, and `-DPS2_QUAKE_ALWAYS_RESET_IOP=0` still does, for comparisons: the module
  list says what runs, each module the game needs is reused when one doing its job is there
  (`kModuleIdentities`), and the rest is loaded. The IOP is then reset only for a known conflict
  (`FindResidentConflict`), and as the fallback when a module won't start, when no game data
  turns up through the launcher's drivers, or when the largest free IOP block is under 384 KB.
  Boot probes host, then HDD/PFS, then USB before `Qcommon_Init`, except that an ELF launched
  from `mass:` probes USB only (the user's call, 2026-10-12).
- **Where a console's boot time goes** (the user's console, wLaunchELF 4.43a, HDD attached,
  debug builds, 2026-10-12): the IOP reset takes 175 ms; starting `ps2hdd` takes 3.4 s, most
  of it the drive answering `sceAtaInit` (ps2hdd only reads a few sectors at start); dev9, atad
  and pfs under 50 ms together. **A PFS mount pays twice for the disk's layout.** Opening the
  partition by name walks the APA chain from the start of the disk (`apaFindPartition`): one
  header read per partition ahead of it, each at that partition's start, so a seek apiece.
  Then the mount counts free zones (`pfsBitmapCalcFreeZones`), reading the allocation bitmap a
  1 KB block at a time, one per 8192 zones: for `+QUAKE` (7296 MB) about 114 reads with 8 KB
  zones, 228 with 4 KB, on a drive that loads at ~1.35 MB/s (performance.md). Mounting it took
  4.07 s; how that splits between the two isn't measured (time a by-name open of the
  partition apart from the mount). The drive also holds ~90 OPL games, a partition each, so
  the chain has 90+ entries for a walk to cross. Searching every partition on a USB launch took 24.6 s: the
  listing walks the chain once, and every mount walks it again from the start, past ps2hdd's
  20 cached headers (`-n 20`), then reads its own bitmap. Before and after, from `main()` to
  the game data:

  | Launch | Old order, with reset | Now |
  | --- | --- | --- |
  | USB, HDD attached | 29.0 s | 80 ms (no reset, launcher's usbd and usbhdfsd) |
  | HDD (`hdd0:+QUAKE:pfs:/`) | 7.7 s | 7.5 s (4.43a reset the IOP itself) |

  So the reset was never the cost; the HDD is. A smaller game partition, early in the chain,
  mounts faster.
- **The launcher's USB drivers froze a profiling release** (2026-10-26, wLaunchELF 4.43a, `mass:`
  launch, no keyboard attached): the game reused the launcher's usbd and usbhdfsd, loaded base2 from
  the stick in 31.7 s (reads fine, the I/O log written at the load's end), then froze within the
  first 64 frames of the demo, before the frame log's first append to `frame_log.txt`. The same ELF
  with `-DPS2_QUAKE_ALWAYS_RESET_IOP=1` (the game's BDM stack) ran the full 12-minute capture from
  the same stick. Not narrowed further: candidates are the first write-append through usbhdfsd 1.6
  (open `O_WRONLY`, `lseek` to the end, write) and the music reader's reads through it. A release
  has no exception handler, so a hang and a crash look the same; `quake2_dbg_diag_2026-10-26.elf`
  in `build/baselines/` would report a crash to quake2.log. Until it is understood, a run that
  writes to the stick (captures, logs) wants the reset build, and the 80 ms reuse boot is suspect
  for anything that writes. **Seen again 2026-10-31** (a profiling release built without the
  flag by mistake, three runs, `ps2_logfile 1`): `quake2.log` ends at `CL_PrepRefresh`'s
  `sky 256` line, 36.5 s in, and the I/O log, written at the load's end, never appears. The
  stick's LED kept blinking through the freeze, so I/O was still going on.
- **What launchers leave on the IOP** (their sources, read 2026-10-12). wLaunchELF before
  2026-05-26, the 2019 release included, resets the IOP only for an ELF on `hdd0:`. For
  `mass:`/`mc:` it leaves iomanX, fileXio (with a 128 KB buffer), the ROM's SIO2MAN and PADMAN
  (ports closed), ps2sdk's `mcman-old`/`mcserv-old` (`mcman_cex` 2.11, `mcserv` 2.8), cdfs,
  usbd and BDM (usbhdfsd before 2022), and the keyboard, HDD and network drivers if it used
  them. On the user's console, 4.43a left 40 modules for a `mass:` launch: the boot ROM's 24,
  then `AllowDVDV`, `sio2man` 1.1, `Poweroff_Handler`, `IOX/File_Manager` 1.1,
  `IOX/File_Manager_Rpc` 1.2, `dev9` 2.8, `mcserv` 2.8, `padman` 1.20, `atad` 2.4, `mcman_cex`
  2.11, `hdd_driver` 2.5, two unnamed (iomanX among them), `usbd` 1.1, `pfs_driver` 2.2 and
  `usbhdfsd` 1.6, with 576 KB of IOP memory free in one block. Its usbd goes by `usbd`, not
  ps2sdk's `USB_driver`: ours then found the "usbd" library taken, and the boot fell back to a
  reset until usbd was also recognised by its library. Its settings can
  point it at an external `USBD.IRX` (FMCB puts one in `SYS-CONF`). It reset the IOP for an ELF
  on `hdd0:`, as its source says. Since then it launches through ps2sdk's elf-loader2, which resets and loads the ROM's
  SIO2MAN, MCMAN and MCSERV. A `LoadExecPS2` launch gets EELOAD's set, with XLOADFILE
  (LOADFILE 2.1).
- **Telling modules apart:** `smod_get_next_mod`, plus the name read by SIF DMA, gives each
  module's IRX_ID name and version. The ROM's SIO2MAN, PADMAN, MCMAN and MCSERV are 1.x
  (`padman` 1.20); the X modules and ps2sdk's are 2.x or 3.x (`sio2man` 3.17, `padman` 3.6).
  ps2sdk's iomanX has no IRX_ID, so it lists unnamed: find it by its export library,
  `slib_get_exp_lib("iomanx")` after `slib_exp_lib_list()`. usbd is found by its "usbd"
  library too, whatever its name (`ModuleIdentity::library`). `smod_get_mod_by_name` and
  `slib_get_exp_lib` both match a prefix. The boot report lists every module running at boot,
  with versions, and the libraries found.
- **`sbv_patch_enable_lmb()` returns 1 without patching** for a ROM LOADFILE already patched,
  and for any LOADFILE it doesn't recognise (2.x has the RPC natively). An LMB call to a
  LOADFILE without the RPC hands back the request buffer, which reads as a module started. So
  after a 1, the boot checks the next module it loads from a buffer shows up in the list.
- **libpad and libmc find the server themselves:** `padInit` binds the old or the new PADMAN
  RPC, whichever exists, and `mcInit` ignores its type and tries XMCSERV's init first.
  ps2sdk's mcserv answers both command sets. So a resident pad or card driver of either
  generation can be reused, while loading the ROM's next to it can't work: its library or RPC
  server is taken. The ROM's PADMAN and MCMAN link only against SIO2MAN 1.x.
- **A pad port a launcher left open keeps being written:** PADMAN DMAs the port's state into
  the EE buffer it was opened with, every field. In PCSX2 a test launcher's buffer landed in
  the game's `.text`, and changed 6 times in 100 ms until the boot closed the port
  (`StartPadDriver`, 2026-10-12). What lands before that can't be undone, and an IOP reset has
  the same window. LaunchELF closes its ports before launching.
- **IOP memory for the later drivers:** sound, the ROM's pad and card drivers and the keyboard
  took 312 KB of the largest free IOP block in PCSX2 (1220 KB to 908 KB, the music playing
  too; 2026-10-12). No RPC reports free IOP memory; `LargestFreeIopBlockBytes()` halves its way
  there with `SifAllocIopHeap`, for the boot report and `ps2_dump_iop_mods`.
- **HDD drivers and limits:** standard `ps2hdd.irx` already uses iomanX; the separately named
  `ps2hdd-iomanx.irx` is a virtual-disk variant without DEV9/ATAD. Use ps2dev9 → ps2atad →
  ps2hdd → ps2fs. Missing hardware/failed HDD init must fall through to USB. **Skipping them
  once hung a console:** a temporary build that went straight to the USB chain hung on a black
  screen at boot on the user's console with an HDD attached (2026-10-11), and the same code
  with the probe back booted from USB. Not diagnosed; PCSX2 with an HDD image didn't reproduce
  it. A `mass:` launch now skips them (2026-10-12), and an always-reset build launched from
  USB on that console, HDD attached, booted: the IOP reset and the USB modules only, no HDD
  driver at all, data at 909 ms. So skipping them wasn't what hung the 2026-10-11 build. APA's default
  one open handle cannot enumerate while PFS is mounted; boot uses `-o 4 -n 20`. PFS's
  default two open files cannot cover pak/music/log/asset reads; boot uses `-m 1 -o 16 -n 40`,
  supplying NUL-separated args. Cache buffers must be at least `2 * maxOpen + 8`.
- **HDD selection:** retain the launch partition before reset; mount it read/write at `pfs0:`
  so the debug log can be written. Other partitions are enumerated from `hdd0:`/`hdd1:`:
  `HDIOC_STATUS == 0`, `stat.mode == APA_TYPE_PFS`, and no `APA_FLAG_SUB`. These constants
  are in `hdd-ioctl.h`. The `iox_dirent_t` DMA destination must own aligned 64-byte lines.
  Keep the winning mount alive and unmount misses before probing the next partition.
- **PFS close does not always persist metadata.** Standard read/write mount flag `0` leaves
  dirty inode/directory metadata cached: `pfsFioCloseFileSlot` only calls
  `pfsCacheFlushAllDirty` when mount flag `0x02` is set (`PFS_MT_ROBUST` in `libhdd.h`).
  `fileXioSync("pfs0:", 0)` explicitly flushes it. `system/iop_boot.cpp` keeps the selected
  HDD mount internal and exposes `SyncGameDataDevice()`, which returns 0 without an RPC for
  host/USB. `debug/log_file.cpp` calls it after each close, including header and fatal output,
  without knowing fileXio or the mount name. Check close/sync results and disable logging on
  failure. Sync uses the default blocking `FXIO_WAIT` mode and returns a negative SDK error
  directly rather than setting libc errno. The original missing-log diagnosis remains
  unconfirmed until a new HDD run; the available emulator log was from a later host run.
- **`fileXioSetRWBufferSize` frees the old buffer before allocating the new one**, and on
  failure keeps a null buffer and the new size (fileXio_iop.c), so every later read would
  DMA from IOP address 0. On a nonzero result, set the stock 16 KB again (`SetFileXioBuffer`
  in iop_boot.cpp). Read sizes per layer are in [performance.md](performance.md).
- **All IOP module loading and every sbv patch live in `system/iop_boot.cpp`.** Subsystems ask
  for a driver (`StartPadDriver`, `StartMemoryCardDriver`, `StartSoundDriver`,
  `StartKeyboardDriver`) and set up only their EE library; keep new drivers that way. Each
  driver is brought up once, and each module in it reuses a running one that does the job, the
  boot's or the launcher's, so the keyboard asks for usbd whatever the boot started.
  Module-loader readiness is separate: only the host: path reaches its first buffer load
  without the sbv patches.
- **IOP thread priorities decide who waits on a file read.** The IOP kernel has no time slicing:
  a thread runs until it blocks or a higher-priority one (a lower number) becomes ready, and an
  interrupt's `iSignalSema`/`iWakeupThread` switches at its exit. fileXio's RPC thread is 40, and
  a HDD read keeps it running (dev9's `SpdDmaTransfer` spins on the DMA with interrupts on, by
  design), so any other server at 40 waits for the whole read; ps2sdk's audsrv was one (its RPC
  thread 40, its play thread 39). The game's audsrv runs its RPC thread at 39 (audio.md). Check a
  module's `CreateThread` priority before routing per-frame RPCs through it.
- **`mass:` is `mass0:`.** iomanX's `parsefile` reads a missing unit number as 0, and libcglue
  passes both spellings through unchanged. bdmfs_fatfs serves the N-th FAT/exFAT volume it
  mounted as `massN:` (N < 10, FatFs `FF_VOLUMES`): one volume per partition, in mount order,
  with GPT's EFI and MS-reserved partitions skipped. usbmass_bd drives two USB drives at once.
  The boot probes all ten units.
- **Writing a file on USB: only `close()` puts it on the drive.** FatFs keeps a file's last
  partial sector in its own buffer and updates the size in the directory entry only on
  `f_sync`/`f_close`. bdmfs_fatfs's `sync` op returns EIO and libcglue's `fsync()` is
  ENOSYS, so the only flush is to close the file. A file held open across a power cycle can
  come back empty. BDM's block cache is write-through. `debug/log_file.cpp` opens, appends
  and closes on every write for this reason. On a real console over USB that costs **~23 ms
  per logged line** (2026-10-07). A load-trace run's base1 load logged ~1040 lines, about
  24 s of its 76 s, so hardware load times with the log on mostly measure the log.
- Console boot finds the ELF's folder from `argv[0]`. Loaders spell the device their own way
  (`mass:/dir/quake2.elf`, `mass:dir/quake2.elf`, `hdd0:__common:pfs:/...`), and the IOP reset
  renumbers USB drives. Preserve the HDD partition separately, and drop the partition's
  path component for browser-style `hdd0:/partition/dir/elf` paths. Bare `pfsN:` paths need
  partition enumeration after reset. Search folders before roots. PCSX2 passes `host:` plus
  the ELF's absolute host path. The folder is capped
  at 50 chars: the engine builds `<base>/baseq2/<name>` in `MAX_OSPATH` (128) with names up to
  `MAX_QPATH - 1`, so the base path (`massN:` + folder) must stay within 56.
- `rom0:FILEIO`'s `remove()` RPC handler lacks a `break`: every `remove(path)` on the fio
  backend also runs `mkdir(path)`. `sbv_patch_fileio()` fixes it (applied on the host: path;
  `ps2::sys::FileIoRemovePatched()` reports it). fio has no `rename()` (`ENOSYS`). The USB
  boot uses fileXio, which has neither problem.
- The pad uses rom0 SIO2MAN + PADMAN, so cards use **rom0 MCMAN + MCSERV** (same family).
  ps2sdk's own mcman.irx needs its own sio2man and won't link (its `mcman-old.irx`, which
  wLaunchELF leaves running, does link against the ROM's). iop_boot's
  `LoadRomModuleOnce` loads ROM modules, since SIO2MAN must not load twice; its driver
  bring-ups are one-shot.
- **A camera that turns on its own is the stick, not a stale poll.** padman reports a poll with
  no fresh data as `length` 0 (`padInit.c:353` in freepad, ps2sdk's padman, whose old-protocol
  build mirrors rom0:PADMAN; the ROM module itself is closed), so `padRead` returns 0 and
  `GamePad::Update` drops analog for that frame. A worn DualShock 2 can rest more than 0.15 of
  full deflection off centre: the user's old pad turned the camera with the stick released on
  a console (2026-10-11), which PCSX2 with a modern host pad never shows. The stick dead zones
  went from 0.15 to 0.25 for it (input.cpp), which fixed that pad on the console.
- **`mcInit` spins forever if MCSERV isn't running** (a SifBindRpc loop). Call it only after
  every module load succeeded.
- libmc with the old MCSERV: no `mcRename`. "Unformatted" comes from `mcGetInfo`'s result
  (-2), and the first `mcGetInfo` after insertion returns -1. `mcOpen` takes **IOP flags**
  (`FIO_O_RDONLY` 1, `WRONLY` 2, create `0x200`), not newlib `O_*`. There is no `O_TRUNC`, so
  delete then create. At most 3 open handles.
- The PS2 BIOS browser ignores the icon texture's alpha bit: transparent texels draw black.
