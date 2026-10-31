# Background loading: one loader thread for mid-level assets and cinematic streaming

*Active, 2026-10-26. Stages 1-8 below, §12 the researched follow-ups (2026-10-28); stages 1-6 landed 2026-10-26 and met their console gates in the USB capture of 2026-10-27 and the HDD capture and console self-test of 2026-10-28 (the HDD showed a new audio-submit stall, §12.4); 7 deferred on those numbers; 8 done but for the by-eye handoffs. Replaces the plan parked on 2026-10-14,
which covered mid-level models and skins only and reasoned from PCSX2 numbers. The console
captures of 2026-10-26 (`build/baselines/console-hdd-2026-10-26.flog`,
`console-usb-2026-10-26.flog`; the numbers are in `.claude/rules/performance.md`) changed what
the problem is and widened the scope to sounds, pics and cinematics. Names only below, no line
numbers: find functions by name.*

## 1. Goal

One mechanism that removes every frame stutter caused by a drive read on a console: mid-level
model, skin, sprite and HUD pic loads; mid-level sound-effect loads; cinematic (and demo)
playback. Level loads stay synchronous under the plaque, where a stall is expected. World and
sky textures stay level-load only. The CD music reader keeps its own thread (§11).

**The rule.** An asset that is still loading is skipped where it would be drawn or played. It
never blocks a frame, and it never draws a placeholder. An asset that is *missing* keeps
today's behaviour exactly (a NULL model and its octahedron, the placeholder texture, no sound),
because the existence check stays synchronous and on the main thread.

Decisions taken with the user on 2026-10-26: a sound asked for before its load lands plays
late, up to 0.5 s, then is dropped; this document covers the loader work only (no stopgaps as
stages); the music reader stays as it is; the player's sexed sounds are not preloaded (memory
held in every level isn't worth it, they load through the loader like any other mid-level
sound); the loader lives under `src/ps2/fs/`.

## 2. Context: the measured facts (console, 2026-10-26)

- **Mid-level loads are 95% I/O.** The slugs ammo model and skin: 43 ms of ClParse on the HDD,
  41 ms of it FsIo; 91 ms from USB. `a_slugs.pcx` plus `rg_hum.wav`: 61 ms (58 FsIo) and 147 ms,
  the second past the audsrv ring. Decode is 2-3 ms of each. PCSX2's host reads had hidden this:
  the parked plan's "decode is half the cost" was a PCSX2 artefact.
- **Cinematics.** `SCR_ReadNextFrame` reads through a 64 KB stdio buffer on the main thread. One
  refill is 93 ms on USB (the p50 of 971 undisturbed refills), and every refill frame runs
  120-130 ms, past audsrv's 106 ms IOP ring, which then replays its last tenth of a second: 1236
  of ntro's 1240 refills, about six times a second. From the HDD a refill is 10-54 ms (the
  drive's two speeds) and nothing underruns, but `sndFree` came within 2400 bytes of empty 23
  times. Huffman decode costs 10-13 ms per cinematic frame on the console, 35 ms at worst
  (ntro), 3-4x PCSX2: a cache-bound walk over a 512 KB table. Bitrates: ntro 384 KB/s mean and
  700 KB/s over its worst second; the others 270-360 KB/s; frames up to 63 KB compressed;
  eou1-8 carry 11 kHz 8-bit mono audio, the rest 22 kHz stereo.
- **The drive.** USB 1.1 costs about 5 ms per SCSI command plus 900 KB/s of transfer. FatFs ends
  every read at a cluster boundary (32 KB on the user's stick) and usbmass caps a command at
  64 KB, so a 64 KB read is two commands (~93 ms) and a cluster-aligned 32 KB read one (~41 ms,
  ~780 KB/s sustained). **A stick's reads run 5x slower for ~0.5 s after any write to it** (the
  capture's own dumps showed it). The HDD reads at 1.35 MB/s or 6-8 MB/s.
- **The main thread never sleeps at vsync.** `graph_wait_vsync` spins on `GS_CSR` in
  `PresentFramebuffer`; EE work is 6-10 ms of a 20 ms PAL field, so 8-11 ms per frame are spent
  spinning, plus ~3.5 ms spinning on the GS FINISH (`GsWait`). The only sleeps are RPC waits:
  two audsrv calls per frame, and any file I/O.
- **Memory.** 32 MB. The worst moment is a map transition, 30.33 MB (city3), with 0.86 MB free at
  the worst load peak; ~25 MB are free at boot. A between-levels cinematic plays with the old
  level still registered.
- **The demo stream** has the same refill class (Server 66-72 ms on the HDD, 125 ms from USB:
  five of the seven slow demo frames). Not the goal, but §6.3 covers it for free.

## 3. Findings

1. **Where mid-level loads enter.** `CL_ParseConfigString` (`CS_MODELS` → `re.RegisterModel`;
   `CS_SOUNDS` → `S_RegisterSound`, which loads synchronously when `!s_registering`; `CS_IMAGES`
   → `re.RegisterPic`; `CS_PLAYERSKINS` → `CL_LoadClientinfo` → model, weapon models, skin, icon).
   Inline `S_RegisterSound` calls across cl_fx.c and `S_StartLocalSound`. `S_RegisterSexedSound`
   on the first use of each `*pain*.wav` and friends per level, preceded by an existence probe
   through `FS_FOpenFile` and close. The menu's player model (registered every draw) and
   `RF_USE_DISGUISE` (every frame). All of them run outside `BeginRegistration`/
   `EndRegistration`.
2. **Only `RegisterModel` can return NULL.** `PS2_RegisterSkin` and `PS2_RegisterPic` go through
   `FindTextureOrPlaceholder`, which hands back `tex::DebugTexture()` for a miss, so
   `CL_LoadClientinfo`'s skin and icon fallbacks are dead code; only its male-model fallback
   works. "Missing" must therefore stay a synchronous answer: a pak lookup is a hash probe
   (`FS_FindInPack`), a loose-file lookup one `fopen` per directory searchpath.
3. **Blocking file I/O sleeps.** `sceSifCallRpc` waits on a semaphore; fio and fileXio serialise
   their callers. A thread in `read()` sleeps while others run, and any other thread's file call
   waits behind the one in flight: about 300 ms behind an 8 KB music read on USB (audio.md),
   which is why the keyboard poll checks `MusicReadPending()`.
4. **Nothing else is thread-safe.** dlmalloc (`USE_LOCKS 0`; newlib's malloc and stdio buffers
   route into it), the heap tag stats, the model and texture caches, the FS globals
   (`file_from_pak`, the shared `pack_t::handle` position, `ps2_pkz_s`'s `fd` and `filePos`),
   the profiler, `Sys_Milliseconds`'s statics, `Com_Printf`, the I/O log, the loading screen.
   `log_file.cpp` already locks, because the music reader prints. Opens and closes stay on the
   main thread, and a descriptor is never closed with a read in flight.
5. **fileXio splits a read around 64-byte alignment.** It DMAs the aligned middle and copies the
   head and tail on the EE inside the completion interrupt. A destination that shares a cache
   line with anything another thread touches is corrupted when that line is written back. So a
   destination is 64-byte aligned, a whole multiple of 64 bytes, and owns its lines. Chunk
   boundaries *inside* one destination are safe: both halves of a shared line belong to the same
   request and land in sequence. PCSX2 has no data cache and never shows a violation.
6. **A deflated pkz entry cannot be handed to a thread as {fd, offset, length}.** It has to be
   inflated: `InflateWhole` uses one global 32 KB input workspace and `tinfl_decompressor`
   (`s_readWorkspace`), and a deflated `fopencookie` stream owns a 32 KB input, a 32 KB
   dictionary and an inflator. A stored entry is a plain byte range of the pack. Entries are
   resolved at open for packs `make pkz` wrote (`Entry::resolved`), so locating one needs no read.
7. **Models read in place.** `LoadAliasMD2Model`: header, a 16-aligned hunk laid out vertexes
   then frames, a nested `tex::Find` per skin while the model's stream is open, glcmds expanded
   from a scratch buffer, frames `FS_Read` straight into the hunk, then the lightnormal clamp.
   `LoadSpriteModel` reads the whole file into its hunk. Keep the in-place reads: staging a
   278 KB player model would double it at the tightest moment.
8. **Textures.** `LoadPcx`, `LoadTga` and `LoadWal` do `FS_LoadFile` and decode into
   `AllocAligned(16, TexImage)`; `HasTransparentTexels` scans every texel; small Pics go to the
   scrap atlas; `Register` asserts `pixels != nullptr`. The VRAM upload is lazy, at first bind
   (`rs::EnsureTextureResident` → `gs::UploadTexture`), and asserts `pixels` too.
9. **The draw-side hooks are few, and no struct grows.** `RenderEntities` draws an octahedron for a
   NULL model before its type switch; `md2::SkinForEntity` falls back skinnum → skin 0 →
   `DebugTexture`; `DrawSpriteEntity` uses `DebugTexture` for a NULL frame; the 2D path goes
   through `FindTextureOrPlaceholder`. `ModelInstance` has a free byte at offset 69 (its
   `static_assert(sizeof == 224)` holds); `TexFlags : u8` has spare bits.
10. **Sounds.** `S_LoadSound`: `FS_LoadFile` → `GetWavinfo` (file-scope globals, not re-entrant)
    → `PS2_AudioMalloc` (fatal on failure) → `s->cache` assigned *before* `ResampleSfx` fills it
    → NULL on failure. `S_StartSound` loads synchronously at play time and drops the sound on
    NULL. **`S_IssuePlaysound` dereferences `S_LoadSound`'s result with no NULL check**: a latent
    crash on any sound that fails to load. `S_PaintChannels` breaks on NULL; `S_AddLoopSounds`
    skips a NULL cache and never loads. Playsounds are kept sorted by `begin` in
    `s_pendingplays`, and `S_PaintChannels` issues those with `begin <= paintedtime`.
11. **Cinematics.** cl_cin.c reads with `fread`/`FS_Read`, polls `feof`, never seeks, and
    `fclose`s in `SCR_StopCinematic`. `Huff1TableInit` reads 64 KB of counts; `SCR_RunCinematic`
    paces at 14 fps and reads one frame ahead. `cin::DrawFrame` (renderer/cinematic.cpp)
    re-converts the picture and re-uploads the 128 KB texture on every draw call (50/60 Hz),
    though `cin.pic` changes at 14 fps. The mixer-rate restart for 11 kHz cinematics is already
    fixed (up only, 2026-10-26).
12. **Threads.** The main thread runs at priority 1 (ps2sdk's `InitThread`). The EE kernel has
    no time slicing: a thread runs until it blocks or a higher-priority thread becomes ready, and
    `iSignalSema` from an interrupt handler dispatches a higher-priority waiter at interrupt exit
    (how every RPC completion wakes the main thread). An equal-priority thread runs only when the
    current one blocks or calls `RotateThreadReadyQueue`. The music reader (`music_stream.cpp`)
    is the port's template: equal priority, a static stack with `gp_reg = &_gp`, request and
    done semaphores, `volatile` state written last, `WaitForReads` re-checking on every wake, and
    an explicit yield after queueing.
13. **The vblank handler pattern exists.** ps2sdk's `AddIntcHandler(INTC_VBLANK_S, handler, -1)`
    plus `EnableIntc`, as libgraph's `graph_add_vsync_handler` and gsKit's semaphore-signalling
    handler do. `INTC_VBLANK_S` is the EE INTC's own line, independent of the GS IMR.

## 4. Decisions

- **One loader thread, `ps2::aio`, at the main thread's priority plus one (lower), and the main
  thread sleeps at vsync.** Lower priority means the loader never preempts the main thread, and
  the main thread preempts it at once whenever an interrupt makes it ready (the vblank handler, an
  RPC completion, a pad read). CPU work on the loader (inflate from stage 2, decode in stage 7)
  therefore costs the main thread only D-cache pollution, and no yields are needed anywhere.
  Equal priority with `RotateThreadReadyQueue`, the music reader's model, is unsafe once the
  loader does CPU work: without time slicing a 10 ms inflate step holds the CPU through the main
  thread's RPC wake, a dropped frame. The music reader gets away with it because it does one
  `read()` and nothing else. The cost of lower priority is re-issue latency: the loader issues
  its next chunk only when the main thread next sleeps, up to one frame's EE work (6-10 ms) on a
  41 ms chunk, about 10-20% of throughput. §6.3 sizes the ring for it. **This requires the
  sleeping vsync** (§6.2), so the 8-11 ms of idle per frame go to the loader; the GS FINISH spin
  stays a spin (§11).
- **One loader-owned inflate workspace**: a 32 KB input plus a `tinfl_decompressor`, 64-aligned,
  about 44 KB, allocated by the main thread on the first deflated request and kept. One job runs
  at a time, so per-request workspaces would only multiply 44 KB by the queue depth at the moment
  memory is tightest.
- **`FS_LocateFile` resolves every source on the main thread** to a byte range, or a compressed
  range plus the raw size. The loader sees only `Read` and `InflateWhole` jobs (stage 7 adds
  `Decode`).
- **Priorities at chunk granularity:** `Stream > Sound > Asset`, re-picked after every chunk of
  at most 32 KB, so a cinematic chunk waits at most one chunk (~41 ms on USB) behind a model.
- **Read-ahead streams are chosen in filesys.c by extension** (`.cin`, `.dm2`), so cl_cin.c and
  sv_user.c stay untouched and demos get the fix for free.
- **Pics go async only through `re.RegisterPic`.** The by-name `Draw_*` path keeps loading
  synchronously, so menus behave as today (a menu's open is a user-initiated pause).
- **Loading is skipped, Failed draws nothing, missing is unchanged.** A corrupt file becomes
  Failed and is reported once; today it gave NULL or the placeholder. That is the one semantic
  change. A NULL (missing) model keeps the octahedron: it is a useful debug signal and
  `CL_LoadClientinfo` relies on the NULL.
- **Sounds defer, then drop.** `S_StartSound` on a loading sfx queues the playsound with a
  deadline 0.5 s out; `S_IssuePlaysound` re-queues it 10 ms later until the deadline passes.
  Loops pick the sound up when its cache lands.
- **Drains, not cancels.** Every point that frees or closes behind an in-flight read first
  finishes everything pending, with no budget: `PS2_BeginRegistration`, `S_BeginRegistration`,
  `S_Shutdown`, `FS_SetGamedir`, and a read-ahead stream's close.

## 5. The loader: `ps2::aio` (new `src/ps2/fs/async_io.{h,cpp}`)

### 5.1 Thread model and rules

- **Priority:** `ReferThreadStatus(GetThreadId())` for the main thread's (1); the loader is
  created at +1. **Stack:** `alignas(16) static u8 s_stack[16 * 1024]` in `.bss`, `gp_reg = &_gp`
  (the music reader's pattern). 16 KB covers `read()` through libcglue and fileXio plus tinfl
  and the stage-7 decoders. **Lifetime:** created lazily by the first `Submit` (a run that never
  loads mid-level pays nothing), lives for the program.
- **What the loader may touch:** its request slot's fields; the descriptors handed to it (it
  never opens or closes one); `lseek` and `read`; its one workspace; `ReadCycles()` (a COP0 read)
  for timing; `DelayThread` for the debug throttle; tinfl and the stage-7 decode functions,
  which are pure over caller-owned buffers. **Never:** `heap::*` or malloc, `Com_Printf`,
  `Sys_Milliseconds`, the profiler, the I/O log, the caches, the FS globals, the loading screen.
  Errors are codes in the request; the main thread prints them with the name it kept.
- **What the main thread may not do while a request is pending on it:** free or write its
  destination, close its descriptor, or sweep the cache slot that owns it. The drain sites of §4
  enforce the last; `Submit` asserts the first's alignment.

### 5.2 Requests, ring and scheduling

```
enum class Kind     : u8 { Read, InflateWhole };   // stage 7 adds Decode
enum class Priority : u8 { Stream, Sound, Asset };
enum class State    : u8 { Free, Queued, Running, Done, Failed };

struct Request {
    volatile State state;   // main: Free->Queued, Done/Failed->Free; loader: Queued->Running->Done/Failed
    Priority prio; Kind kind; u32 sequence;
    int  fd;  u32 offset;  u32 bytes;  // the file range (the compressed range for InflateWhole)
    void * dest;  u32 destBytes;       // dest 64-aligned; destBytes >= RoundUp64(bytes or rawBytes)
    u32  rawBytes;                     // InflateWhole: the inflated size, what is delivered
    volatile u32 bytesDone;            // chunks landed so far: a stream may read behind this
    volatile int result;               // bytes delivered, or a negative code
    u32  ioCycles;                     // ReadCycles() spent inside read(), for the frame log
};
```

- **Ring:** 32 static slots. `Ticket Submit(const Request &)` asserts `dest % 64 == 0` and
  `destBytes >= RoundUp64(...)`, and returns an invalid ticket when no slot is free, on which the
  client loads synchronously. `State Poll(Ticket, u32 * bytesDone)`; `void Wait(Ticket)` sleeps
  on the shared done semaphore and re-checks, as `MusicStream::WaitForReads` does;
  `void Release(Ticket)`; `void Drain()` waits on every non-Free slot; `bool Pending()` is the
  keyboard poll's guard beside `MusicReadPending()`; `bool Enabled()`; `Stats TakeFrameStats()`
  (bytes landed, chunks, queue depth) read in `FrameLogCapture`.
- **Scheduling:** a counting semaphore carries submissions. The loader scans the ring for the
  Queued or Running request with the best (priority, sequence), runs **one chunk**, and rescans.
  A Running request can be set back behind a higher-priority arrival at a chunk boundary;
  `bytesDone` is its resume point, and the loader caches `{fd, pos}` to skip redundant `lseek`s.
  An InflateWhole may be interleaved with `Read`s between its input pieces, never with another
  InflateWhole (one workspace, one tinfl state).
- **Chunks:** `kChunkBytes = 32 KB`, with boundaries on 32 KB multiples of the *container file's*
  offset (the first chunk runs to the next boundary, then whole pieces, then the tail). On FAT
  with 32 KB clusters that is one SCSI command per chunk; on PFS one 64 KB-buffer piece. The
  head and tail copies fileXio makes at an unaligned chunk start land inside the request's own
  buffer and are safe (§3.5). A main-thread file call therefore waits at most one chunk: about
  41 ms on USB, under 10 ms on the HDD.
- **InflateWhole:** the compressed range is read in 32 KB pieces into the workspace input and
  inflated straight into `dest` with `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`, the loop
  `InflateWhole` in pkz_pack.cpp already has, as a resumable step (`RunInflateChunk`, one piece
  per scheduler step; a mirror of the synchronous loop rather than a lift of it, see stage 2, with
  the self-test comparing the two outputs). Per piece: ~41 ms of I/O and ~10 ms of CPU on the
  console (12 MB/s), preemptible.
- **Debug cvars** (all in CVARS.md): `ps2_aio` (1; 0 makes every client load synchronously, the
  A/B switch), `ps2_aio_throttle_kbps` (0; a `DelayThread` after each chunk so PCSX2 behaves like
  USB), `ps2_aio_pump_us` (2000, §6.7), `ps2_testaio` (stage 2), `ps2_readahead_kb` (512, §6.3),
  `ps2_vsync_spin` (0, §6.2).

### 5.3 Memory and alignment

- Every destination is 64-aligned, allocated to `RoundUp64`, and owned by its request until Done.
  Model hunks become 64-aligned (`HunkAllocator::Init` gains an alignment; the frames block comes
  first and is rounded to 64). Staging buffers and the cinematic ring come from
  `TryAllocAligned(64)` under a new `MemTag::Aio` (an `Aio` row in heap.cpp's names table).
- Static: the 16 KB stack, ~2 KB of ring, the pending tables (textures 16, models 16, sounds 8
  entries). Heap: the 44 KB workspace once a deflated file is requested; 256-512 KB of ring
  while a cinematic plays (it replaces the 64 KB stdio buffer); file-sized transient staging for
  sounds and textures; 128 B plus the glcmd scratch per model in flight.
- A failed allocation of a staging buffer or the ring means the synchronous path, never an error.
- `ps2_testmaps 1` must show the worst moment and the lowest free point unchanged: the drain runs
  at every level change, so nothing asynchronous survives into a load.

## 6. Clients

### 6.1 File system: locate without reading (`filesys.c`, `[PS2_QUAKE]`)

```c
typedef struct {
    enum { FS_LOC_LOOSE, FS_LOC_PAK, FS_LOC_PKZ_STORED, FS_LOC_PKZ_DEFLATED } kind;
    int fd;                  /* loose: fileno of the FILE* FS_FindFile opened; pak: pack->asyncFd;
                                pkz: the pack's reader descriptor */
    unsigned offset, bytes;  /* the range to read (the compressed range when deflated) */
    unsigned rawBytes;       /* deflated: the inflated size (== bytes otherwise) */
    FILE * looseFile;        /* closed by FS_ReleaseLocation, on the main thread */
} fs_location_t;
int  FS_LocateFile(const char * name, fs_location_t * out); /* length or -1; notes FLOG#open */
void FS_ReleaseLocation(fs_location_t * loc);
```

- Built on `FS_FindFile`. **Pak:** `pack_t` gains `asyncFd`, opened by the main thread on first
  use (`open(pak->filename, O_RDONLY)`), unbuffered, never touched by stdio, closed in
  `FS_SetGamedir` after a new `PS2_AsyncIoDrain()` hook ahead of its pak loop. **Pkz:**
  `PS2_PkzLocate(pkz, index, &loc)` resolves the entry (`ResolveDataOffset` reads only for packs
  pakzip didn't write, on the main thread) and `PS2_PkzReaderFd(pkz)` hands out a second
  descriptor on the pack, `ps2_pkz_s::readerFd`, owned by the loader side; the main thread's `fd`
  and `filePos` are untouched. **Loose:** the `FILE *` from `FS_FindFile`, `fileno()` for the
  read, kept open until the client finishes.
- `FS_FOpenFile` gains the read-ahead choice: a `.cin` or `.dm2` whose location is not
  pkz-deflated, with `ps2_readahead_kb > 0`, returns `PS2_OpenReadAheadStream(&loc, kb)` instead
  of the stdio stream.

### 6.2 Sleeping vsync (`gs.cpp`): landed 2026-10-26

- `gs::Init`: `CreateSema` {init 0, max 1}; `AddIntcHandler(INTC_VBLANK_S, VblankHandler, -1)`;
  `EnableIntc(INTC_VBLANK_S)`. The handler: `iSignalSema(s_vblankSema); ExitHandler(); return 0;`.
- `PresentFramebuffer`, inside the existing `VSync` probe: `while (PollSema(sema) >= 0) {}` then
  `WaitSema(sema)`, replacing `graph_wait_vsync()`. Draining first keeps "the next vblank edge".
  `StampPresent` and the DISPFB write follow as today; the wake latency is microseconds, well
  inside the blanking interval. "EE work = Frame − VSync" still holds: the probe wraps the sleep.
- Fallback: if the semaphore or handler can't be created, or `ps2_vsync_spin 1`, keep the spin.

### 6.3 Cinematics and demos: a read-ahead stream (new `src/ps2/fs/readahead_stream.{h,cpp}`)

- `FILE * PS2_OpenReadAheadStream(const fs_location_t *, int ringKB)` returns an `fopencookie`
  stream set `_IONBF`, the pkz deflated-stream pattern, so cl_cin.c's `fread`/`FS_Read`/`feof`/
  `fclose` and sv_send.c's `fread` work unchanged.
- **Ring:** `ringKB / 32` slots of 32 KB from `TryAllocAligned(64, MemTag::Aio)`; try 512 KB,
  then 256, else return the plain stdio stream with one console line. Each slot is one `Read`
  request at `Priority::Stream`, chunk boundaries cluster-aligned as in §5.2. Every slot is
  submitted at open, so the 64 KB Huffman table and the first frames are in flight while the
  first `fread` waits on slot 0: today's 1.6 s USB start becomes one chunk's wait.
- **Read callback:** copies from the head slot, which may still be Running, up to `bytesDone`,
  so a partially landed chunk already feeds the decoder; a consumed slot is released and
  resubmitted for the next file range; an unlanded head slot means `aio::Wait` (the main thread
  sleeps, the loader runs). EOF when the file range is exhausted. **Seek:** `SEEK_CUR 0`
  (`ftell`) and seeks inside the buffered window are served; anything else drains and restarts
  the ring at the target (nothing uses it today). **Close:** drain the ring's requests, free the
  ring and the cookie.
- **Sizing.** The fill rate on USB is about 780 KB/s against ntro's 384 KB/s mean and 700 KB/s
  worst second, so the ring only has to cover the loader's worst latency: a 0.5 s write-poisoned
  stretch during a peak is a deficit of about 270 KB. 512 KB is the default, 256 KB the
  fallback (`ps2_readahead_kb`; 0 = the plain stream).
- **Demos** (`sv.demofile`, opened through `FS_FOpenFile` in sv_user.c, read sequentially in
  sv_send.c) get the same stream through the extension rule, with one caveat: a `.dm2` in
  pak0.pkz is deflated and therefore ineligible. Either `pakzip` stores `.dm2` and `.cin` (a small
  tool change) or demos benefit from pak and loose sources only. Optional, in stage 3.
- **Decode stays on the main thread** at first: with the refills gone, a cinematic frame costs
  10-35 ms of decode, two fields at worst and far under the 106 ms ring. Stage 7 may move
  `Huff1Decompress` to the loader as budget hygiene; it is not a stutter fix after stage 3.

### 6.4 Sounds (`snd_mem.c`, `snd_dma.c`, `snd_loc.h`; new `src/ps2/audio/sound_load.{h,cpp}`)

- **Split `S_LoadSound`.** The part after `FS_LoadFile` (GetWavinfo, the size computation,
  `PS2_AudioMalloc`, `ResampleSfx`) becomes `static sfxcache_t * S_FinishLoadSound(sfx_t *,
  byte * data, int size)`, used by the synchronous path and by a new
  `void PS2_SoundLoadComplete(sfx_t *, void * data, int size)` that the backend calls from the
  pump on the main thread, where GetWavinfo's globals are fine. The data buffer is the backend's,
  freed afterwards.
- **`S_LoadSound`**, after its cache-hit return: `if (PS2_SoundLoadPending(s)) return NULL;`;
  then, when `!s_registering && PS2_SoundLoadAsync(s, namebuffer)`, return NULL; otherwise
  today's synchronous code. `PS2_SoundLoadAsync` does `FS_LocateFile` (missing → false → the
  synchronous path reports it as today), allocates a 64-aligned staging buffer, submits a
  `Sound`-priority request, and records `{sfx, ticket, buffer, size}` in an 8-entry table (full
  → false → synchronous).
- **Guards.** `S_StartSound`: `if (!sc && !PS2_SoundLoadPending(sfx)) return;`, and
  `ps->deadline = ps->begin + dma.speed / 2` (a new `unsigned deadline` in `playsound_t`).
  `S_IssuePlaysound`: the `S_LoadSound(ps->sfx)` moves before `S_PickChannel`; on NULL the
  playsound is re-linked, sorted, with `begin = paintedtime + dma.speed / 100` while
  `paintedtime < deadline` (the sort loop from `S_StartSound`, factored into `S_LinkPlaysound`),
  otherwise `S_FreePlaysound`. That also fixes the latent NULL dereference of §3.10. The re-queue
  lands strictly past `paintedtime`, so `S_PaintChannels`' issue loop always breaks.
  `S_PaintChannels` and `S_AddLoopSounds` are already safe.
- **Drains:** `PS2_SoundLoadsDrain()` at the top of `S_BeginRegistration` and `S_Shutdown` (both
  clear sfx slots or free caches). The pump finishes sounds first, inside `PS2_BeginFrame`, so a
  sound that landed in the last idle window is mixed by the same frame's `S_Update`.

### 6.5 Textures (`texture.{h,cpp}`, `image_load.{h,cpp}`, `ref.cpp`, `md2.cpp`, `view.cpp`)

- `TexFlags::Loading` and `TexFlags::Failed`; `Texture::IsLoaded()` is neither.
- **Decoder split:** `LoadPcx` and `LoadTga` become `FS_LoadFile` plus `DecodePcx` and
  `DecodeTga(const u8 *, int len, ...)`, shared by the synchronous path and the pump. The
  transparency test folds into the PCX run loop (one compare per run), which helps level loads
  too.
- **`TextureCache::LoadFromFile`**, when `aio::Enabled() && !IsRegistering()`, the type is
  Skin, Sprite or Pic and the extension `.pcx` or `.tga`: `FS_LocateFile` (missing → nullptr, as
  today) → `RegisterPending(fullname, type)` (a slot with name, type, regSequence,
  `flags = Loading`, `pixels = nullptr`, inserted in the lookup; `Register` keeps its asserts) →
  staging buffer plus an `Asset` request → a 16-entry pending table (full → synchronous). WAL,
  Sky and Wall never go async.
- **Finish (in the pump):** decode, transparency, scrap `TryPack` for Pics, fill the slot's
  fields (`Register`'s body factored into `FillTexture`), clear Loading, free the staging,
  `FLOG#ready`. A decode failure sets Failed with one console line; the slot stays, so repeated
  `Find`s don't retry.
- **Draw sites check `IsLoaded()`:** `md2::SkinForEntity` (skip the entity; shadows and shells
  follow), `DrawSpriteEntity` (skip), `PS2_DrawPic` and `PS2_DrawStretchPic` (skip the draw),
  `PS2_DrawGetPicSize` (-1, -1, id's "missing" answer). `EnsureTextureResident` keeps its
  asserts: it is never reached for a Loading texture. `Unload` of a slot with null pixels frees
  nothing.
- **Entry points:** async only through `PS2_RegisterSkin`, `PS2_RegisterPic` and the nested
  `tex::Find` from model finishes. `PS2_DrawPic` by name on a cache miss loads synchronously,
  so menus are unchanged. A missing file behaves as `FindTextureOrPlaceholder` does today.

### 6.6 Models (`model.{h,cpp}`, `model_load.{h,cpp}`, `view.cpp`)

- `ModelInstance::state : u8 {Ready, Loading, Failed}` in the padding byte at offset 69, and
  `IsLoaded()`; the 224-byte assert holds.
- **`ModelCache::LoadModel`**, when `aio::Enabled() && !IsRegistering()` and the extension is
  `.md2` or `.sp2` (the type comes from the extension; the header's id validates it when it
  lands; `.bsp` never goes async): `FS_LocateFile` (missing → nullptr, so `CL_LoadClientinfo`'s
  male fallback still works) → a slot with name, type, regSequence and Loading, inserted in the
  lookup → the first request → a 16-entry pending table (full → synchronous).
- **MD2, two phases.** (1) A 128-byte header read into a 64-aligned scratch. On landing:
  `ValidateAliasHeader`; `LayOutAliasHunk` (a 64-aligned hunk, **frames first**, rounded to 64,
  then vertexes; 64-aligned scratch for the glcmds and the skin names); three `Asset` requests
  (skins, glcmds, frames). (2) When all three are Done: `FinishAliasModel`: the skin names go
  through `tex::Find(Skin)` (async on their own), glcmd expansion, the normal clamp, free the
  scratch, Ready, `FLOG#ready`. `LoadAliasMD2Model` becomes these three functions plus the reads,
  so the synchronous and asynchronous paths cannot drift.
- **SP2:** one request of the whole file into the 64-aligned hunk; on landing validate the
  version and frame count, `tex::Find(Sprite)` per frame, Ready.
- **Draw:** `RenderEntities` keeps the octahedron for a NULL model and `continue`s on
  `!model->IsLoaded()` before the type switch. `ReferenceAllTextures` already skips null skins.
- **Failure:** a bad header or a short read → Failed, the hunk freed, the slot kept, one console
  line.

### 6.7 Frame integration (`ref.cpp`, `renderer/profile.{h,cpp}`, the scripts)

- **Pump:** `aio::Pump(budgetUs)` in `PS2_BeginFrame`, after `FrameLogCapture` and before
  `rs::BeginFrame`: finishes Done requests in FIFO order, sounds, then textures, then models,
  stopping after about 2 ms on the console (`ps2_aio_pump_us`) and always finishing at least
  one. Everything finished here is drawable and mixable in the same frame. `PS2_BeginFrame`
  doesn't run while the plaque is up, and nothing loads asynchronously then.
- **Drain sites** (§4): drain = `aio::Drain()` plus an unbudgeted pump, so every pending asset
  becomes an ordinary cache entry before any sweep.
- **Keyboard:** keyboard.cpp's guard becomes `audio::MusicReadPending() || aio::Pending()`.
- **Frame log:** events `MdlLoad`, `TexLoad`, `SndLoad` (charged by synchronous loads and by
  asynchronous finishes, FsIo nested in the synchronous ones) and `AioPump`; counters `aioKB`
  and `aioQ`; a `FLOG#ready,<frame>,<name>` line when an asset becomes drawable or mixable.
  `frame_budget.py` pairs `#open` with `#ready` for request-to-ready latency;
  `summarize_flog.py` is header-driven and needs nothing.

## 7. Failure handling

| Case | Behaviour |
| --- | --- |
| File missing | Synchronous answer from `FS_LocateFile`: NULL model, placeholder texture, no sound; unchanged |
| Ring, table or memory full | Synchronous load, today's path, counted in `aioQ` |
| Short or failed read, bad header, bad deflate data | Request Failed → asset Failed, never drawn or played, one console line, memory freed |
| Level change, snd_restart, gamedir change, stream close | Drain, then the sweep; at most one chunk plus the finish steps |
| Loader thread can't start | `aio::Enabled()` false, everything synchronous, one warning |
| Vblank semaphore or handler can't be created | The `graph_wait_vsync` spin, as today |
| A USB write poisons reads for ~0.5 s | Chunks take about 5x longer; the cinematic ring absorbs it; assets arrive later; nothing stalls |

## 8. Stages and gates

Each stage builds under `make` and `make release`, lands on its own, and is measured against
`build/baselines/console-hdd-2026-10-26.flog` and `console-usb-2026-10-26.flog` with
`frame_budget.py --pal` (PCSX2 for correctness, the console for timing; compare by `FLOG#map`
segment, since the USB capture's frame numbers are offset by a menu detour).

- [x] **1. Sleeping vsync.** Landed 2026-10-26 (gs.cpp `InitVsyncWait`/`WaitVsync`, through
  ps2sdk's `graph_add_vsync_handler`). Gate met in PCSX2: `pcsx2-vsync-sleep-2026-10-26.flog`
  against `-spin-` (same build, `ps2_vsync_spin 1`) within noise on every column, 0 drops both
  ways, 19111 against 19112 presents; the wake-to-write gap is measured instead of a FIELD
  counter: `PresentClock::worstWriteTicks`, printed as the frame log's closing `FLOG#note,present:`
  line, read 1 µs (the blanking interval is ~1500 µs). Console (USB, 2026-10-27): 3 µs over
  18261 presents, the run complete; no tearing was reported.
- [x] **2. `ps2::aio`, `FS_LocateFile` and a self-test, no clients.** Landed 2026-10-26
  (`src/ps2/fs/async_io.{h,cpp}`, `async_io_c.h` for the C hook, `FS_LocateFile`/
  `FS_ReleaseLocation`, `PS2_PkzLocate`, `MemTag::Aio`, the keyboard guard, `ps2_aio`,
  `ps2_aio_throttle_kbps`, `src/ps2/tests/aio_test.{h,cpp}` behind `ps2_testaio`). The test reads
  13 files through the loader and `FS_LoadFile`, compares them byte for byte with the 64-byte
  rounding checked untouched, twice (the second pass throttled to 900 KB/s, one ticket through
  `Wait`), times a main-thread read every frame, and checks the declined-ticket path with
  `ps2_aio 0`. PCSX2 gate met: 24 comparisons, 0 failures, loose, pkz-stored and pkz-deflated
  seen (a pak needs a run without the pkz), probe read 4 ms at worst, no keyboard stalls. **Still
  owed: one run on the console from USB**, the only place the cache-line rule is real; **passed
  2026-10-28** (`quake2_dbg_aio_2026-10-28.elf`, on screen, no log kept). The first
  try (2026-10-27) ran out of memory: the test started under the attract loop's idlog.cin, whose
  ring outranks the test's requests, so the pass (5.6 MB of destinations) was still in flight
  when the loop loaded demo1's level. Since 2026-10-28 the test kills the startup server first,
  as the perf run does, and reads each reference copy when its request lands (6 MB held, not 12). Deviation
  from §5.2: the loader has its own inflate stepper (`RunInflateChunk`, one input piece per
  scheduler step) rather than a loop lifted out of pkz_pack.cpp, since the synchronous loop and a
  resumable step don't share a shape; the self-test's byte comparison is the guard against drift.
- [x] **3. Cinematic read-ahead**, landed 2026-10-26 (`src/ps2/fs/readahead_stream.{h,cpp}`,
  the `FS_FOpenFileImpl` extension rule, `ps2_readahead_kb`, `aio::Cancel` so a close waits one
  chunk at most, the frame log's `aioBytes`/`aioQ`). Demos get it where their source is a range
  (pak, stored pkz entry, loose); pak0.pkz deflates `.dm2`, so they don't from it: the `pakzip`
  store rule stays optional. PCSX2 (can't show the gain, only that nothing broke): cinematic rows'
  FsIo 958 → 824 µs mean, p95 1418 → 1101, Cinematic and Frame unchanged; `ps2_aio_throttle_kbps
  900` gives the same capture to the row, 200 KB/s starves as designed (`pcsx2-readahead-
  2026-10-26.flog`, `-throttled-`). **Console gate met (USB, 2026-10-27,
  `console-usb-aio-2026-10-27.flog`):** rows over the 106 ms ring 86 → 1 on eou1_ and 1235 → 1 on
  ntro (the opens); FsIo on cinematic rows p95 0.6 ms and max 1.2 ms (from 93 ms and 1.8 s);
  `aioQ` 1-7 through ntro with 80 MB landed; `Cinematic` at the HDD's 10-13 ms. The decode rows
  still take 25-35 ms of wall time (decode plus the 3.7 ms re-upload), as before, which is stage
  7's territory and never reaches the ring. The cinematic starts shrank less than predicted (eou1_
  2.8 → 1.0 s, ntro 1.7 s both times); the open and the table read aren't instrumented.
- [x] **4. Sounds.** Landed 2026-10-26 (`src/ps2/audio/sound_load.{h,cpp}`, `src/ps2/fs/
  aio_pump.{h,cpp}` with `ps2_aio_pump_us`, `S_FinishLoadSound`/`S_LinkPlaysound`/
  `playsound_t::deadline`, the drains in `S_BeginRegistration` and `S_Shutdown`, `FLOG#ready`).
  PCSX2, throttled to 900 KB/s: `rg_hum.wav` opened on frame 5012, ready on 5017, its row's FsIo
  gone, no warnings, the run complete. No `SndLoad` column was added: `#open`/`#ready` pairs give
  the latency. Console (USB, 2026-10-27): `rg_hum.wav` ready 5 frames after its request, its row's
  ClParse 146.8 → 0.5 ms. Still owed: the by-ear check and the missing-file NULL path. Original gate: the `rg_hum.wav` row loses its FsIo; `SndLoad` moves to later rows;
  `#ready` latency 2-4 frames on USB; no dropped or doubled one-shots in a `ps2_perftest` run by
  ear; the `S_IssuePlaysound` NULL path exercised with a deliberately missing file in a debug run.
- [x] **5. Textures.** Landed 2026-10-26 (`TexFlags::Loading`/`Failed`, `Texture::IsLoaded`,
  `tex::FindAsync`/`PumpLoads`/`DrainLoads`, `AllocSlot`/`FillTexture`/`CommitIndexed`/
  `CommitRgba`, `DecodePcx`/`DecodeTga` with the transparency test folded into the PCX runs, the
  four draw-site skips, `SmallPool::IndexOf`). PCSX2 throttled: `pics/a_slugs.pcx` opened 5012,
  ready 5018, its row's ClParse 5.6 → 0.3 ms, FsIo gone, 0 drops, no asserts. Console (USB,
  2026-10-27): `a_slugs.pcx` ready 5 frames after its request, its row off the frame; the visual
  handoff by eye still owed. Original: (skins, sprite frames, `RegisterPic` pics; the decoder split; the fused
  transparency scan). Visual handoff: pickups and player skins appear a few frames late, nothing
  draws the checkerboard or garbage, menus unchanged. Gate: `a_slugs.pcx`'s FsIo gone;
  `ps2_testmaps 1` unchanged.
- [x] **6. Models.** Landed 2026-10-26 (`LoadState` in `ModelInstance::state`, `IsLoaded`,
  `mod::PumpLoads`, `LoadAliasMD2ModelFromMemory`/`LoadSpriteModelFromMemory` sharing
  `LayOutAliasHunk`/`ClampAliasNormals`/`ValidateSpriteHeader`/`ResolveSpriteFrames` with the
  streamed loaders, the `RenderEntities` skip). **Deviation from §6.6:** the file is read or
  inflated whole into a staging buffer and converted from memory, not read in place in two phases:
  a pkz entry is deflated, so it has no ranges to read, and pak0.pkz is what a USB stick carries.
  Files over 768 KB load synchronously, so the staging copy never doubles a level's largest model.
  PCSX2 throttled: slugs model opened 4758, ready 4760, its skin 4761; row ClParse 4.8 → 0.4 ms,
  FsIo 2.8 ms → 4 µs; 0 drops; no asserts. Console (USB, 2026-10-27): the slugs model ready 9
  frames (180 ms) after its request and its skin 11, the row's ClParse 90.7 → 0.5 ms and its
  Frame 99.9 → 19.9 ms. MapCycle (PCSX2, 2026-10-28): all 39 maps, worst moment 30.33 → 30.41 MB,
  the 40 KB inflate workspace plus the stages' code (memory-budget.md). Visual handoff by eye still
  owed. Original:
  (MD2 in two phases, SP2 in one read, the loader split). Handoff: weapon and
  ammo pickups, a weapon switch, the player-setup menu, a DM join (the 278 KB loose model). Gate:
  the slugs row's ClParse near 0; `#ready` 3-4 frames on USB for a pickup model; a throttled
  PCSX2 run with 0 drops; `ps2_testmaps 1` PEAK unchanged.
- [ ] **7. Decode on the loader, deferred (2026-10-27).** The USB capture shows what it would
  buy: the cinematic decode rows cost 15 ms (eou1_) to 10-28 ms (ntro) of EE work and span two
  fields in 54-74% of cases, unchanged by the loader, and the pump's finishing frame for a pic plus
  a sound was 27.6 ms. None of it reaches the audio ring, and a 14 fps picture doesn't show a
  two-field frame, so this stays undone unless a visible symptom appears. If taken up: a `Decode`
  job kind, pure CPU over main-allocated
  buffers, in order of measured need: the MD2 finish for large models (if the pump overruns), PCX
  decode, then `Huff1Decompress` (a preallocated output double buffer in place of the per-frame
  `Z_Malloc`, `cin.hnodes1` read-only, `SCR_ReadNextFrame` handing the block to a job: cl_cin.c
  changes, so last). Gate: EE work on the affected rows down by the decode's cost, the other
  columns within ~2% cache noise.
- [x] **8. Measure and record.** The USB capture (2026-10-27) and the HDD capture (2026-10-28) are
  in `performance.md`, README and CVARS.md carry the cvars, `memory-budget.md` has the MapCycle
  line (2026-10-28), the console self-test passed. From the HDD the main thread's cinematic reads
  are gone (ntro's 13.8 s of refills; rows over 30 ms 26.2 → 21.6%) and the pickups' ClParse
  42.9/61.0 → 0.5 ms, but a read in flight can hold the next audio submit 8-44 ms on the IOP
  (§12.4). Still owed: the by-eye and by-ear handoffs of stages 4-6.

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| Loader throughput lost to re-issue latency (lower priority: the main thread must sleep first) | Measured by the stage-2 self-test; the ring is sized for it; if it matters, a `Submit` from the pump at frame start keeps the queue deep |
| Cache-line sharing invisible in PCSX2 | Asserts in `Submit`, 64-rounded allocations, the stage-2 console run |
| A main-thread file call waits behind a chunk | 32 KB cluster-aligned chunks (≤41 ms USB); saves and config writes already run under a sound hold |
| The audio submit waits on the IOP behind an HDD read (seen 2026-10-28: 8-44 ms in 2% of chunk rows) | §12.4: the submit on a thread of its own (2026-10-30), the main thread never waiting; before that a hold of one frame while a read is in flight, on the HDD only |
| D-cache pollution from loader CPU work raises the main thread's EE time | Stage 7 is conditional and gated on a capture |
| Paks or the pkz closed, or caches swept, with reads in flight | The five drain sites; asserts that nothing is pending at `Unload` and `FreeUnregistered` |
| A deflated `.cin` or `.dm2` in the pkz gets no read-ahead | `pakzip` stores them, or pak and loose sources only |
| The PCSX2 `_request_end` TLB flake during host I/O | Known (testing-pcsx2.md); re-run before investigating |
| The deadline re-queue in `S_PaintChannels` | Re-queue strictly past `paintedtime`, so its issue loop always breaks |

## 10. Critical files

New: `src/ps2/fs/async_io.{h,cpp}`, `src/ps2/fs/readahead_stream.{h,cpp}`,
`src/ps2/audio/sound_load.{h,cpp}`, `src/ps2/tests/aio_test.{h,cpp}`. Engine (`[PS2_QUAKE]`):
`src/common/filesys.c` and `qcommon.h`, `src/client/snd_mem.c`, `snd_dma.c`, `snd_loc.h`.
Backend: `renderer/gs.cpp`, `fs/pkz_pack.{h,cpp}`, `renderer/texture.{h,cpp}`,
`renderer/image_load.{h,cpp}`, `renderer/model.{h,cpp}`, `renderer/model_load.{h,cpp}`,
`renderer/md2.cpp`, `renderer/view.cpp`, `renderer/ref.cpp`, `renderer/profile.{h,cpp}`,
`system/heap.{h,cpp}`, `input/keyboard.cpp`. Tooling: `src/tools/scripts/frame_log/frame_budget.py`,
the Makefile (`PS2_CXX_SRC`, `SIZE_OPT_CXX_SRC` for the test and the stream), CVARS.md.

## 11. Related work, not in this plan

- `cin::DrawFrame` converting and uploading only when `cin.pic` or the palette changed: about
  3.6x less per-frame conversion and upload during playback.
- `S_RegisterSexedSound`'s existence probe through `FS_LoadFile(name, NULL)` (a length query)
  instead of an open and close, saving one IOP round trip per first use.
- The CD music reader migrating onto `ps2::aio` as a `Stream`-priority client: one thread fewer,
  and its reads would obey the chunk rule. Worked out in §12.1.
- The GS FINISH spin (`GsWait`, ~3.5 ms a frame) becoming an INTC_GS wait, giving the loader that
  time too.
- The cinematic ring's size and chunk size against the console numbers, and the start's cost:
  §12.2. `cin::DrawFrame`'s per-frame conversion and upload: §12.3.

## 12. Follow-ups, researched 2026-10-28 (no code yet)

Three items from §11 read against the code as it stands after stages 1-6 and the console USB
capture of 2026-10-27, and a fourth (§12.4) from the HDD capture of 2026-10-28. Each is sized for one commit; none changes a format or a save.

### 12.1 The music reader as a loader client

**Implemented 2026-10-29**, with these deviations from the text below:
- `BufferState` stays a stored field (Empty, Queued, Deferred, Ready), set from the ticket by
  `Collect`; a Deferred buffer (declined by the loader) is read by `ReadNow` when decode needs
  it, after waiting out the other buffer's loader read on the same descriptor.
- **Chunking (b) can't keep every read one cluster command for stereo ADPCM**: the 2 KB header
  leaves every 4 KB chunk group 2 KB off a 4 KB boundary, so no group-aligned request starts on a
  32 KB one. Requests are 32 KB of whole groups anyway, and the loader splits each at the boundary
  it crosses: two reads per 1.3 s against five for 8 KB requests. A carry of the partial group
  into the next buffer would fix it, at a cost in the decoder not worth one USB command a second.
- **The seek cache keeps its slot key**; a new `Request::sequential` lets a client say its
  requests follow one another on a descriptor nothing else moves, and then any slot's cached
  position counts. Dropping the slot outright would trust the cache across a descriptor number
  reused after a close, or a file the main thread read before handing it over (the music's own
  header reads). The music sets it except after `Open` and a `ReadNow`; the read-ahead ring sets
  it after its first request. A failed call now clears the cached descriptor too.
- `MusicReadPending()` is gone: the keyboard guard is `aio::Pending()` alone, and snd.cpp's HDD
  hold `aio::FileCallInFlight()` alone.

PCSX2 gates met: the hash test (audio.md: all 12 pass hashes of four synthetic tracks equal to
HEAD's, with the loader and with `ps2_aio 0`); a throttled perf capture with Music, SndMix and
Input unchanged on the demo segments and the music landing as one 32 KB request every ~1.25 s;
the MapCycle (memory-budget.md: `Mus` 64 KB, worst moment 30.40 MB). **Console gate met**
(2026-10-29, USB, the profiling release with `in_keyboard 1`, `console-usb-music-aio-2026-10-29.flog`,
performance.md): no Input stalls (worst 0.64 ms, the keyboard polled on ~6% of rows),
the music landing as one 32 KB request every ~1.3 s, Music's worst row 1.38 → 0.59 ms.
**From the HDD, a regression** (2026-10-29, `console-hdd-music-aio-2026-10-29.flog`,
performance.md): about one 32 KB music request in six outlasts §12.4's one-frame hold, and the
submit waits 4-13 ms behind it; gameplay's blocked submits went from 2 to 13. Answered by
§12.4's submit thread (2026-10-30): on the console (2026-10-31, HDD and USB) no submit blocks
(§12.4).

**What exists.** `audio::MusicStream` (music_stream.{h,cpp}) owns a second thread: 8 KB static
stack, equal priority to main, two semaphores, `ReaderLoop` serving two `Buffer`s strictly in
turn (`Empty → Pending → Ready`), `RotateThreadReadyQueue` yields after every `RequestNext` and
in `Decode`, and `WaitForReads` for `Close`. A buffer is 8 KB for ADPCM (two 4 KB chunk groups,
~325 ms of 22 kHz stereo) and 8-64 KB for WAV (~3/8 s), both from `TryAllocAligned(64,
MemTag::Music)` at `Open`, freed at `Close`. The file is a loose `open()` descriptor; the header
reads (`ReadScratch`, 64 bytes) happen synchronously in `Open`, which also waits for the first
buffer so the track starts on the next frame. `ReadPending()` feeds `MusicReadPending()`, whose
only user is keyboard.cpp's poll guard (now `MusicReadPending() || aio::Pending()`).
`cd_audio.cpp` calls `Decode` from `PumpMusic` once a frame (`CDAudio_Update`, after `S_Update`)
and inside `HoldMusic`'s fade loop; nothing else touches the stream's I/O.

**What changes.** `Buffer` gains an `aio::Ticket` and loses `state`/`resultBytes`; the thread,
both semaphores, `s_readerStack`, `ReaderEntry/ReaderLoop`, `YieldToReader`, `m_readerIndex`,
`m_readerFilePos`, `m_priority` and `m_threadId` go. The buffer's state is read from the ticket:
`aio::Poll` Queued/Running is today's Pending, Done is Ready, Failed is a failed read, and Free
(after `Release`) is Empty, so `BufferState` can stay as a derived accessor rather than a stored
field.

- `RequestNext(i)`: fills a `Request{Kind::Read, Priority::Stream, m_fd, m_payloadStart +
  offset, requestBytes, buffer.data, RoundUp64(requestBytes)}` and `Submit`s it. A declined
  ticket (loader off, 32 slots busy) marks the buffer as a synchronous read: `Decode` then reads
  it on the main thread when it needs it, as the pre-thread code did, so `ps2_aio 0` still plays
  music. The buffer memory is already 64-aligned with a 64-multiple stride; `destBytes` is the
  stride.
- `AcquireBuffer`: `Poll(ticket)`; Done with `Result == requestBytes` is today's Ready path;
  Failed or a short result keeps today's warning and `m_finished/m_failed`.
- `ReleaseBuffer`: `aio::Release(ticket)` then `RequestNext`. No yield: the loader runs when the
  main thread next sleeps (the vsync wait, any RPC), which is within the frame.
- `Decode`'s trailing yield loop goes.
- `Open`: after `RequestNext` on both buffers, `aio::Wait(buffer[0].ticket)` replaces the
  `m_doneSema` loop (same behaviour: the first buffer lands before the track starts).
- `Close`/`WaitForReads`: `aio::Cancel` on every ticket (a Queued one drops at once, a Running one
  waits one chunk), then `close(m_fd)` and `FreeBuffers`, in that order: the descriptor and the
  buffers may not go while a request holds them (§5's rule).
- `ReadPending()` becomes "any ticket Queued or Running"; `MusicReadPending()` then duplicates
  `aio::Pending()` and the keyboard guard simplifies to the latter. Keep the function one
  release for the audio rule's cross-reference, or delete it with the thread.
- `StartReaderThread` goes; `Open`'s `Unusable` path for a failed thread becomes the loader's
  `Enabled()` false → synchronous reads, not silence.

**Chunking.** An 8 KB ADPCM request never crosses a 32 KB boundary of the file only if the payload
offsets are 8 KB-aligned from a 32 KB-aligned start; `music_file::kHeaderBytes` is 2 KB, so they
aren't, and about one request in four straddles a chunk boundary and costs two `read()`s. Two
options: (a) accept it (one extra USB command per 1.3 s of music); (b) size requests to whole
chunks: `m_readBytes` = the bytes up to the next 32 KB boundary of the file for the first request,
then 32 KB (eight ADPCM groups, ~1.3 s), the buffers growing to 2 × 32 KB (64 KB of `Music`, from
16 KB; a WAV CD rip already uses 2 × 64 KB). (b) halves the command count and keeps every read one
cluster command; the 48 KB is held only while a track plays, never across a load. Prefer (b),
with `kAdpcmReadBytes` becoming the chunk size and a first-request trim in `RequestNext`.

**The loader's seek cache** (`ReadRange`: `s_cacheSlot/s_cacheFd/s_cachePos`) is keyed by slot,
so two consecutive music requests from different slots re-`lseek` though the file position is
already right. Drop the slot from the key: the cache is "where this fd's position is", which is
true whatever slot moved it, and invalidated by any failed call as now. One IOP round trip saved
per music request and per read-ahead slot.

**Priority and latency.** `Priority::Stream` puts music level with the cinematic ring; they never
coexist (`SCR_PlayCinematic` stops the music, `PumpMusic` returns while `cl.cinematictime > 0`).
Against a pickup's `Asset` or `Sound` requests the music chunk goes first, which is right: a 1.3 s
buffer in flight 325 ms ahead of need is still the one with a deadline. The loader runs a priority
below main, so a music chunk is issued at main's next sleep (≤ one frame), against the reader's
immediate yield today; with ~650 ms of music buffered (a), or ~1.3 s (b), that is nothing. A
main-thread file call behind a music chunk waits ≤ 41 ms on USB (today ≤ 8 KB's ~15 ms): the
keyboard poll already guards, and the save paths run under a hold; nothing else reads per frame.

**Memory.** The 8 KB static stack returns to .bss's free; `Music` grows from 16 to 64 KB while a
track plays under (b). The 32 request slots: the ring takes up to 16, the music 2, sounds 8,
textures and models 16 each at most; `Submit` declines past 32 and the music then reads
synchronously for that buffer, so no new failure path.

**Gates.** The target hash test in audio.md (ADPCM wraps, the 3-pass 44.1 kHz WAV, the extensible
mono WAV, the forced allocation failure, `.adp` over `.wav`) bit-exact as before; a PCSX2 perf
capture with `Music` and `SndMix` unchanged against `menu-gamma.flog`; `ps2_aio 0` plays music;
a console USB run with `in_keyboard 1` showing no Input stalls (the 2026-10-10 hitch class), and
the music chunks visible in `aioBytes` at ~1 per 1.3 s.

### 12.2 The cinematic ring: size, chunk size, and the start

**Measured (USB, 2026-10-27).** The ring is 16 × 32 KB; `aioQ` (requests Queued or Running) read
0-7 through ntro, mode 1-2, so 9-16 slots sat landed and unread at any time: the ring ran 290-510
KB ahead of the reader at ntro's 384 KB/s mean. Its waits were nil (FsIo max 1.2 ms). On the HDD
the same ring would sit full nearly always.

- **Ring size.** The 512 KB default (`ps2_readahead_kb`) was sized for the stick's 0.5 s read
  stall after a write during a 700 KB/s peak (a ~270 KB deficit, §6.3); the capture confirms it
  was never breached, with 200+ KB to spare at the mode. 256 KB would have covered the mode but
  not the worst case the frame-log dumps provoke; a shipping build makes no such writes, but an
  autosave or `config.cfg` write before a cinematic does. Keep 512 KB for USB; the ring is
  heap-held only while a cinematic plays (between levels, the lowest-memory moment), so the
  256 KB it could save buys nothing a level needs. The `kMinSlots` fallback (8) stays for a
  cinematic that starts with less free. No change recommended; record the depth numbers in
  readahead_stream.cpp's banner.
- **Chunk size.** 32 KB is one cluster command on the user's stick, and FatFs ends every read at a
  cluster anyway, so a 64 KB chunk on USB is two commands with no saving; on the HDD a 64 KB chunk
  is one fileXio piece (PFS reads contiguous zones up to the piece size), halving RPCs for the same
  bytes. Against it: the main thread's worst wait behind a chunk doubles (82 ms USB), and the
  ring's granularity with it. The HDD's refills were never the stutter (its two speeds, 9.6-54 ms,
  stayed inside a field), so the gain there is RPC count, not a frame. Keep 32 KB. If a 64 KB
  cluster stick turns up, `kChunkBytes` could follow the volume's cluster size at boot (FatFs
  exposes it through `f_getfree`/`csize`; bdmfs_fatfs doesn't surface it to the EE today), which
  is the only version of this worth doing.
- **The copy out of the ring** is what FsIo now measures on a cinematic row: 0.4-0.6 ms a frame
  for ~27 KB (ntro's mean frame) through `fread` → `StreamRead` → `memcpy`, plus the frame's
  `Z_Malloc`/`Z_Free` of its 76 KB output. Handing cl_cin.c a pointer into the ring instead would
  save it, but needs the reader to see slot boundaries (a frame can straddle two) and changes
  cl_cin.c; at half a millisecond on a 20 ms field it is not worth either. Leave it.
- **The start.** eou1_'s open row fell 2.8 → 1.0 s, ntro's held at 1.7 s, and the frame log
  charges neither to FsIo or Cinematic (`SCR_PlayCinematic`'s reads are raw `fread`s, not
  `FS_Read`). What is in it: `pics/loading.pcx` (0.3 s measured earlier), `FS_FOpenFile`'s open
  of a loose 80 MB file (FatFs directory walk, then stdio's `fseek(SEEK_END)` for the length,
  which without `FF_USE_FASTSEEK` walks the FAT chain: 2560 clusters for ntro, mostly from BDM's
  cache), `FS_LocateFile`'s second open for the ring (the same walk again), the first chunk's
  wait (41 ms), the 64 KB Huffman table (256 `fread`s of 256 bytes, each a ring copy), and
  `CL_RegisterSounds` for the 11 kHz cinematics (fixed, up-only). Unmeasured: put the open under
  the profiler (`PS2_PROF_FS_IO` around `FS_FOpenFile` in `SCR_PlayCinematic`, one `[PS2_QUAKE]`
  pair) before touching anything. The likely saving is the second open: `FS_FOpenFileImpl` has the
  stream it just opened, and `FS_MakeLocation` could take its `fileno` instead of opening again
  (the loose branch holds `looseFile` already). Half the walk for a one-line change, once the
  measurement says the walk is where the second is.

### 12.3 `cin::DrawFrame`: convert and upload only when the picture changed

**Implemented 2026-10-29** as below: `PS2_CinematicFrameChanged` (cinematic.h, now shared with C)
called after `cin.pic = cin.pic_pending` in `SCR_RunCinematic` and in `CinematicTest_RunFrame`;
`NeedsConversion` compares the frame and palette versions, the pointer, `cols`/`rows` and whether
the buffer is new; `CurvePalette` builds `s_palette16`. **Console gate met (2026-10-29, HDD and
USB):** cinematic Ui 3.68 → 0.67 ms mean, EE work on cinematic rows down 2.4-2.9 ms, ntro's mean
frame 21.1 → 19.9 ms (performance.md).

**What exists.** `SCR_DrawCinematic` calls `re.DrawStretchRaw` every client frame (50 Hz) with
`cin.pic`, which `SCR_RunCinematic` replaces at 14 fps (`cin.pic = cin.pic_pending`, a fresh
`Z_Malloc` from `Huff1Decompress` each time). `cin::DrawFrame` then, every call: expands
`cols × trows` (320 × 240 for the stock films) through `s_palette` into the 256 × 256 RGBA5551
`s_frameBuffer` with the 16.16 column resample (unpacking each palette entry to r, g, b, a and
repacking per pixel), copies a guard row, `MarkPixelsDirty()`, and draws. The bind then takes
`EnsureTextureResident`'s dirty path: `vram::Touch`, no fence (the texture was bound last frame,
not this one), `gs::UploadTexture`: `SyncDCache` over 128 KB, `cmdbuf::WaitIdle`, a synchronous
128 KB PATH3 transfer. The console charges all of it to `Ui`: 3.68 ms on every cinematic row, 2.6
of every 3.6 calls redoing an unchanged picture.

**What changes.**

- cl_cin.c, one `[PS2_QUAKE]` line in `SCR_RunCinematic` after `cin.pic = cin.pic_pending;`:
  `PS2_CinematicFrameChanged();` (declared in q_common.h beside the other PS2 hooks). The pointer
  can't be the signal: `Z_Free` then `Z_Malloc` of the same size hands the same block back.
- cinematic.cpp: `static u32 s_frameGeneration` bumped by that hook, `s_convertedGeneration`,
  `s_convertedPaletteGeneration`. `DrawFrame` converts, copies the guard row and marks dirty only
  when `s_convertedGeneration != s_frameGeneration` or the palette generation moved
  (`gamma::Generation()` through `CurvePalette`, or a `SetPalette` with a new table: bump the
  palette generation there too, since a `command == 1` frame changes the palette without a
  gamma change); otherwise it goes straight to `rs::DrawTexturedRect`. An eviction between frames
  is already handled: `EnsureTextureResident` re-uploads a non-resident texture from
  `s_frameBuffer` whether or not it is dirty, and `SetPalette(nullptr)` (stop, or the menu
  blanking the screen) frees the buffer and must reset `s_convertedGeneration` so the next frame
  converts again. The `cols/rows` arguments can change between films only; fold them into the
  check (a static of the last `cols`, `rows`).
- The conversion itself, for the 14 frames a second that remain: a `u16 s_palette16[256]`
  prepacked by `CurvePalette` (RGBA5551 with the alpha bit), so the inner loop is a lookup and a
  store, about half the instructions of the unpack-repack. The 16.16 column step and the row
  skip stay.
- `SCR_DrawCinematic` is unchanged: the quad is still drawn every frame (`BeginFrame` clears),
  from the resident texture.

**Not this round.** An 8-bit PSMT8 frame texture with the cinematic palette as a CLUT would drop
the conversion and halve the upload (64 KB), but the four CLUTs live at fixed VRAM and change only
through `gs::UpdateCluts` between frames (gs-renderer.md); a fifth slot and a mid-film palette
change path is a renderer change out of proportion to 14 conversions a second. Moving the
remaining conversion to the loader is stage 7's `Decode` kind.

**Expected.** `Ui` on cinematic rows from 3.7 ms every frame to ~0.1 ms on 36 of 50 and ~2 ms (the
prepacked convert plus the upload) on the 14 that change: about 2.6 ms a frame on the console,
which takes ntro's decode rows (EE work 10-28 ms, 54-74% over a field) down by that much, the
largest single cut available short of stage 7. **Gate:** a PCSX2 capture of the perf run's two
cinematics with `Ui` down on cinematic rows and `Cinematic`, `FsIo` and `Frame` unchanged; the
`screenshot` recipe (testing-recipes.md) on a cinematic frame identical before and after (the
texels are the same bytes); a palette-change film (`command == 1` frames: ntro has them) showing
no stale colours; the menu over a cinematic and back; then the console capture's `Ui` column.

### 12.4 The HDD audio submit waiting behind a background read (added after the HDD capture)

**Implemented 2026-10-29** as below: `aio::FileCallInFlight` (set around `lseek`/`read` and the
throttle's sleep, so PCSX2 can exercise it), `sys::GameDataOnHdd`, `DeferSubmit` in snd.cpp, and
the `ps2_sound_defer` cvar (0 off, 1 the HDD, the default, 2 always). The limit became 30 ms, not
40: the test runs at a submit, so the gap reaches the limit plus a frame (a PCSX2 run at 40 ms
held submits back for 67 ms in a row), and 30 ms holds back one 50 Hz frame at most. **Console
(2026-10-29):** HDD stalls 66 → 38 (gameplay 9 → 2), 96% of holds enough, the pickups' dropped
field gone, USB unchanged; the stalls left are reads still out after the one-frame hold. The
NOWAIT fallback below stays unbuilt unless those show.

**The fallback, built 2026-10-30, as a thread rather than a NOWAIT call** (`audio::SubmitThread`,
submit_thread.{h,cpp}; `ps2_sound_async`, 1 by default, 0 the hold above): the music's 32 KB
requests (§12.1) outlasted the hold on the HDD. A NOWAIT binding turned out unsafe as sketched:
audsrv's IOP server has one receive buffer, and a request's data is DMAed into it when the
request is sent, so the free-space query and the play out at once could overwrite each other; the
SDK's client keeps them one at a time with a semaphore. So a thread one priority above the main
thread makes the SDK's two calls (`MixRing::Send`, Drain's body without the cursor) while the main
thread carries on. The main thread posts the frame's painted range at `SNDDMA_Submit` and collects
a finished send's result at the next `SNDDMA_GetDMAPos` or submit, where the submit cursor moves:
it never moves on the thread. A frame whose send before is still out posts nothing (`sndFree` -2),
and the next send takes both frames'. Every place the main thread calls the device itself (a hold,
its release, a silence, a restart, shutdown, `ps2_sound_async` going to 0) waits for the send out
first. The thread's priority makes the play RPC follow the free-space reply at once, as on the main
thread, so the ring is fed on the same schedule, now with nothing for the main thread to wait out.
PCSX2 (performance.md, `pcsx2-sound-async-throttled-2026-10-30.flog`): `Sound` 210-429 → 8 µs, EE
work per frame down 127 µs on the demos and ~400 on the cinematics, no send still out at the next
frame, the ring as before; the save test's music fades unchanged. **Gate (console, HDD):** gameplay
rows with `Sound` over 3 ms gone (13 in `console-hdd-music-aio-2026-10-29.flog`), the music's
requests no longer stretching frames, `-2` rows rare and never two in a row, and by ear the pickups'
stutter gone or not; from USB, a capture like `console-usb-music-aio-2026-10-29.flog`.
**HDD gate met** (2026-10-31, performance.md, `console-hdd-soundasync-2026-10-31.flog`):
rows with `Sound` over 3 ms 56 → 0 (gameplay 13 → 0), the slugs model pickup no longer stretching
a frame, steady dropped frames 17 → 10; `-2` rows are rare (55 runs, 12 of two frames or more,
the longest four, behind one music request), and the ring's readings are distributed as before,
though two empty-ring readings follow a send that was out (one at the `rg_hum.wav` pickup). The
ear check is owed. From USB that ELF froze at the end of the first load, with
`ps2_sound_async 0` too: it was not a reset build, so it ran on the launcher's usbhdfsd
(ps2-platform.md). **USB gate met** with the reset build (2026-10-31,
`console-usb-soundasync-2026-10-31.flog`, performance.md): no submit over 1 ms (6603 before), 4
`-2` rows in the whole run, both pickups clean, dropped steady frames 10 → 7. The ear check is
owed.

**What the console showed** (2026-10-28, `console-hdd-aio-2026-10-28.flog`, performance.md): from
the HDD, 57 cinematic rows had `SndMix` of 8-44 ms, each one a row where a loader chunk landed
(2.2% of them), and 9 gameplay rows had the same with no loader request, which fits the music
reader's reads (the 10-26 HDD capture had 7). The two pickups' following frames lost 27-29 ms
there, so on the HDD they still drop a field. From USB, never. It never empties the 106 ms ring.

**Why.** `AudsrvDevice::FreeBytes` (`audsrv_available`) and `Enqueue` (`audsrv_play_audio`) are
synchronous RPCs, every frame. On the IOP, audsrv's RPC thread and fileXio's both run at
priority 40 (SDK `rpc_server.c`, `fileXio_iop.c`), with no time slicing, so the audsrv request
waits until fileXio's thread blocks; on the HDD path it holds the CPU in places (dev9's DMA spins
by design), on the USB path it doesn't. Exactly where the IOP time goes wasn't measured. Raising
audsrv's IOP priority means patching SDK modules: out.

**Fix: hold the submit back while a background read is in flight, briefly, on the HDD.** Before
`FreeBytes`, `SNDDMA_Submit`'s path asks whether the loader is inside `read()` (a new volatile
`aio::ReadInFlight()`, set by the loader around its `lseek`/`read`, not the queue depth) or the
music reader has one out (`MusicReadPending()`), and if so, and the last submit was under
`kMaxSubmitDelayMs` = 40 ms ago, sends nothing this frame. That is a short hold without the
silence flush: the mixer's submit cursor stops, the ring drains at most 40 ms from the ~85 ms it
holds at a submit, and the next frame tops it up as usual. Only when the game data device is
the HDD (`iop_boot` knows it), so USB runs stay as captured. No race with the main thread: the
loader and the reader only run while main sleeps, so a read can't start between the check and
the RPC, and one that starts while main waits in the RPC queues behind it on the IOP. A skipped
frame logs `sndFree` -2 (held stays -1) so the capture counts them.

**If that leaves stalls** (reads past 40 ms, or IOP work the EE doesn't know of): send the play
RPC with `SIF_RPC_M_NOWAIT` through our own binding to audsrv's server, from a 64-aligned static
send buffer, and read its reply at the next submit, with the free space tracked locally from the
replies. More code, and it replaces the SDK client (whose wrapper advances by the size asked,
not sent, the trap `audsrv_device.cpp` documents), so only on evidence.

**Gates.** PCSX2 can't show it (host reads are instant): there the gate is correctness, with the
HDD check forced on and `ps2_aio_throttle_kbps 900` so reads overlap the submit: music, sound
effects and both cinematics' audio intact, `sndFree` -2 rows present, no `Frame` over the ring.
On the console from the HDD: `SndMix` p99 on chunk rows close to the other rows' (0.6 ms), the
pickups' following frames back to ~20 ms, the -2 rows never two in a row past 40 ms; from USB a
capture matching `console-usb-aio-2026-10-27.flog`.
