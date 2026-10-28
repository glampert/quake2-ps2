# Background loading: one loader thread for mid-level assets and cinematic streaming

*Active, 2026-10-26. Stages 1-8 below; stages 1-6 landed 2026-10-26 and met their console gates in the USB capture of 2026-10-27 (stage 2's console self-test still owed, stage 6's MapCycle too); 7 deferred on those numbers; 8 half done (the USB capture). Replaces the plan parked on 2026-10-14,
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
  owed: one run on the console from USB**, the only place the cache-line rule is real. The first
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
- [ ] **8. Measure and record.** The USB capture is done (2026-10-27, `performance.md`), README
  and CVARS.md carry the cvars, `memory-budget.md` has the MapCycle line (2026-10-28). Still owed:
  a console HDD capture and the console self-test (stage 2).

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| Loader throughput lost to re-issue latency (lower priority: the main thread must sleep first) | Measured by the stage-2 self-test; the ring is sized for it; if it matters, a `Submit` from the pump at frame start keeps the queue deep |
| Cache-line sharing invisible in PCSX2 | Asserts in `Submit`, 64-rounded allocations, the stage-2 console run |
| A main-thread file call waits behind a chunk | 32 KB cluster-aligned chunks (≤41 ms USB); saves and config writes already run under a sound hold |
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
  and its reads would obey the chunk rule.
- The GS FINISH spin (`GsWait`, ~3.5 ms a frame) becoming an INTC_GS wait, giving the loader that
  time too.
