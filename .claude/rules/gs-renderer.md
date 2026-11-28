---
paths:
  - "src/ps2/renderer/**"
---

# GS renderer facts

The README's "Rendering" section is the architecture overview. The renderer files: `ref.cpp`
(refexport), `view.cpp` (world pass, BSP walk, frame pass order), `md2.cpp` (alias models),
`sky.cpp`, `render_system.*` (`ps2::rs`: VIF1 chains, batches, `DrawTriangles`, `Submit`),
`cmd_buffer.*`, `gs.*` (GS front-end, register values, 2D), `vram.*` (texture heap),
`splash_screen.*` (the boot splash, drawn before the renderer exists), `screenshot.*` (the
`screenshot` command),
`texture.*` (`ps2::tex`), `lightmap.*`, `scrap_atlas.*`, `clip.*` (EE sky clipper), `vu1.*`
(VU memory layout, microprogram declarations).

## Frame model: 2D and 3D interleave

- The engine draws **2D → 3D → 2D** per frame (`SCR_TileClear`, then `V_RenderView`, then
  HUD/console in `SCR_UpdateScreen`), with no hook at the boundaries.
- `BeginFrame` clears color+depth. 2D primitives accumulate in a lazily opened pending batch
  (ALLPASS z-test). `FlushPending2D` sends it and is a no-op when empty. **Every 3D emitter
  calls it first**, and `EndFrame` calls it last. Never defer all 2D to frame end: the 2D
  packet bakes texture VRAM addresses, and mid-frame 3D uploads can evict them. Don't
  reintroduce a frame-wide 2D bracket. Each 2D→3D switch is a GS sync point (~2 per frame).
- **`PresentFramebuffer` sleeps for vsync since 2026-10-26** (background loading stage 1): the
  EE INTC's vblank-start line (`INTC_VBLANK_S`, installed through `graph_add_vsync_handler`)
  signals a {0, 1} semaphore from its handler, and the wait drains it with `PollSema` before
  `WaitSema`, so it ends at the *next* edge as `graph_wait_vsync`'s spin did (that cleared VSINT
  before watching it). The stamp and the DISPFB write follow the wake. The PCSX2 A/B against
  `ps2_vsync_spin 1` (same build, `pcsx2-vsync-{sleep,spin}-2026-10-26.flog`) moved no column by
  more than 0.4% (SndMix, which wanders), 0 drops both ways, and the frame log's closing
  `FLOG#note,present:` line put the worst stamp-to-DISPFB gap at 1 µs, against a blanking interval
  of about 1.5 ms. The console USB capture of 2026-10-27 read 3 µs over 18261 presents, with no
  tearing reported. The semaphore or handler failing to create falls back to the spin with a
  console warning.
- **A texture may be `TexFlags::Loading` or `Failed`** (texture.h, since 2026-10-26, background
  loading stage 5): `tex::FindAsync` (what `PS2_RegisterSkin`/`PS2_RegisterPic` call) hands a skin,
  sprite frame or pic first wanted mid-level back at once with null pixels, in the cache under its
  name, while the loader reads its file into a staging buffer; `tex::PumpLoads` (from `aio::Pump`,
  top of `PS2_BeginFrame`) decodes it (`DecodePcx`/`DecodeTga`, image_load.h) and fills the slot.
  **Every draw site checks `Texture::IsLoaded()` and skips** rather than draw a placeholder:
  `DrawAliasMD2Entity` (after `SkinForEntity`), `DrawSpriteEntity`, `PS2_DrawPic`,
  `PS2_DrawStretchPic`; `PS2_DrawGetPicSize` answers -1,-1. The by-name `Draw_*` path keeps the
  synchronous `Find`, so menus never show a hole. A failed decode leaves the slot `Failed`, never
  drawn, reported once. `EnsureTextureResident`'s asserts stand: nothing binds a Loading texture.
  `tex::BeginRegistration` drains the pending table first, and loads during registration stay
  synchronous (`m_registering`). PCSX2, throttled to a USB rate: `pics/a_slugs.pcx` (a HUD icon
  the server names at a pickup) opened on frame 5012 and was ready on 5018; its row's ClParse
  went from 5.6 to 0.3 ms.
- **A model may be `LoadState::Loading` or `Failed`** (model.h `ModelInstance::state`, in the
  padding byte after `type`; since 2026-10-26, stage 6): `mod::Find` on a cache miss for a
  `.md2`/`.sp2` outside registration locates the file, reads (or inflates) it whole into a staging
  buffer through the loader, and answers at once with a Loading slot in the lookup; `mod::PumpLoads`
  converts it with `LoadAliasMD2ModelFromMemory`/`LoadSpriteModelFromMemory`, which share
  `ValidateMD2Header`, `LayOutAliasHunk`, `ExpandGLCmdsToTriangles` and `ClampAliasNormals` with the
  streamed loaders, and look its skins or frames up with `tex::FindAsync`. Whole-file staging rather
  than the plan's read-in-place phases because a pkz entry is deflated and has no ranges; files over
  768 KB load synchronously instead. `RenderEntities` skips a non-drawable model (`IsLoaded`),
  never the octahedron, which stays for a missing file (Find's nullptr, which `CL_LoadClientinfo`
  relies on). `mod::BeginRegistration` drains first. PCSX2 throttled to a USB rate:
  `models/items/ammo/slugs/medium/tris.md2` opened on frame 4758 and was ready on 4760, its skin on
  4761; the row's ClParse 4.8 → 0.4 ms, FsIo 2.8 ms → 4 µs.
- **With `ps2_gs_latency` on (the default), a frame reaches the screen only when the next
  `BeginFrame` runs.** A frame that nothing follows never shows. The loading plaque was one:
  `SCR_BeginLoadingPlaque` draws it once, then `cls.disable_screen` stops all drawing until
  the level is up, so a slow load sat on a black screen. Draw such frames with
  `rs::EndFrame(/*deferPresent=*/false)`, as `PS2_DrawLoadingScreen` does. `rs::FrameStarted()`
  tells you whether a frame is already open.
- **Frames drawn while the save notice is up are shown at once** too: `PS2_EndFrame` draws the
  notice over the engine's frame and passes `deferPresent=false` whatever `ps2_gs_latency` says,
  since the save device's access that follows draws nothing until it is done
  (`save/save_notice.h`). With the plaque up the notice is drawn on the loading screen instead.
  The card-only busy notice it replaced (`SCR_SetBusyNotice`) drew its frame with the latency
  on, which by the code put it on screen only after the save. Before the renderer exists (the boot
  reads config.cfg in `Qcommon_Init`), the splash draws the notice: `splash::ShowImage` uploads
  it, composed on the EE as RGBA, to a fixed VRAM spot past the logo's and draws it one texel to
  a pixel under the logo; the environment's alpha test (`NOTEQUAL 0`) leaves out its see-through
  texels. `gs::Init` calls `splash::Retire` before laying out VRAM, after which the splash draws
  nothing.
- **Framebuffer 0 is on screen from `gs::Init` until the first present** (the first frame is
  drawn into frame 1), showing whatever VRAM holds there. The boot splash draws the id logo
  into exactly the default layout of frame 0 (640 wide, the standard's full height, 16-bit, at
  VRAM 0), so it stays up through `gs::Init` until then. For any other layout `gs::Init`
  clears frame 0 (`splash::IsShownIn`), and it turns the output on only once that is drawn.
- **No frame may begin while a `.bsp` is parsed.** Both command buffer halves live in the world
  loader's lump scratch, lent from `cmdbuf::DrainBeforeWorldLoad` until `BspFileReader::Close`
  (`cmdbuf::LentToWorldLoad()`). `LoadTexInfo` loads every wall texture inside that window, so
  anything that draws from a file hook must check it. A frame built there writes its chain over
  the pinned vertex/edge lumps. That surfaced as "Surface lightmap too large for the
  accumulator!" and is now an assert in `cmdbuf::BeginFrame`. To reproduce: set the loading
  screen's `kRedrawIntervalMsec` to 0, so every file read draws.
- **TEST is not in the VU1 batch register block.** MIPTBP1 took its slot, and the 7-qword tag
  block can't grow: 8 qwords would need 2 more per double-buffer half, and there is 1. 3D
  relies on TEST holding the 3D pixel tests. The clear leaves it so, and `gs::EmitEnd2D`
  re-arms it whenever a 2D section closes. Any new path that writes TEST must restore it the
  same way.

## GS and libdraw

- **VRAM readback** (`gs::DownloadFramebufferRows`, ps2sdk's own sequence, as in the Quake port):
  BITBLTBUF, TRXPOS, TRXREG, FINISH and TRXDIR=1 go down PATH2 (VIF1 DIRECT) behind MSKPATH3
  and FLUSHA; wait for FINISH and an empty VIF1 FIFO, set `VIF1_STAT.FDR` and `BUSDIR`, receive
  by VIF1 DMA, then put both back and unmask PATH3. Nothing may be in flight
  (`rs::FinishFrameInFlight`, outside a frame), and the target is whole 64-byte lines (sync
  before, invalidate after). Works in PCSX2 with 16- and 32-bit framebuffers (2026-10-12); not
  yet tried on a console.
- **The TV standard is ours to pick, not libgraph's.** `graph_initialize` always takes the
  console's region (`graph_get_region()`: an `E` as the ROM name's 5th character is PAL) and
  has no override. So `gs::Init` makes its calls itself with the mode `ps2_video_mode` chose,
  through `system/video_mode.h`, which the fatal-error screen reads too. libgraph's interlaced
  FIELD mode shows 448 lines on NTSC and 512 on PAL (224/256 per field), and puts the picture at
  DX 652/DY 50 and 680/72. A 640x512 16-bit framebuffer pair plus Z costs 30 more VRAM pages
  than 640x448: the texture heap drops from 2408 to 2168 KB. The perf demos in PCSX2 showed no
  out-of-memory syncs, though fewer textures stayed resident (73 against 79 on average). The
  present clock's field rate follows the chosen standard, not the console's. Forcing NTSC on a
  PAL console works on hardware (2026-10-11): the perf test paced at 59.94 Hz.
- **480p (2026-11-28)**: `ps2_video_mode 480p` (video_mode.h `Output::Dtv480p`) is libgraph's
  `GRAPH_MODE_HDTV_480P` through `graph_set_mode` (non-interlaced, FRAME, filter off), but not
  `graph_set_screen`: that sets DW to the mode's whole 1440 VCK, 720 pixels at MAGH 1, out of a
  640-pixel buffer. `video::DtvDisplay` writes DISPLAY1/2 as gsKit does instead: the magnification
  that fits (MAGH 1, MAGV 0), the picture centred in what is left (+80 VCK, +16 lines), plus the
  BIOS's `_GetGsDxDyOffset` (PCSX2's: DX 308, DY 51). With the filter off only read circuit 2 is
  on, and `graph_set_framebuffer_filtered` would point it a row down, so a digital output writes
  both DISPFBs at row 0 (`SetDisplayFramebuffer`). PCSX2 logs `SDTV 720x480 @ 59.94 Progressive
  (FRAME)`; a scripted shot of the attract demo matched 480i's but for the demo's own timing.
- **Strip mode (720p, 1080i; 2026-11-28, experimental)** is hi_res.h's, after gsKit's
  `gsHires.c`; the README's "strip rendering" paragraph has the design. What any new code has to
  respect while `hires::Active()`:
  - **The chain is replayed once per strip per field**, from its first qword, behind a prologue
    that sets FRAME, ZBUF, XYOFFSET, SCISSOR and TEST for both contexts. So nothing in it may
    refer to one strip, nothing may be kicked mid-frame (`rs::KickBuilt` skips, `cmdbuf::Reserve`
    overflowing is fatal), and its clear is the prologue's. **A new ZBUF write in the chain needs
    `hires::AddZbufFixup`** on its value qword, as the chunk heads and `gs::EmitBegin2D` have: every
    pass patches ZBP to its z strip. A new FRAME, XYOFFSET or SCISSOR write would need the same.
  - **The picture is centred on X = 2048** (XYOFFSET.x = 2048 - W/2, `gs::ScreenCentreX`), since the
    0.8 guard band at 2048 + W/2 overflows 12.4 past 820 pixels wide. libdraw's 2D adds a fixed
    2048, so the 2D emitters subtract `originShiftX`; a new emitter must too.
  - **GS traffic outside the chain takes `GsLock`** (gs.cpp): it waits for the pass in flight and
    keeps the next one off the GIF. Texture and CLUT uploads, the seed, readbacks.
  - **The texture heap pins two frames** (`vram.cpp` `IsPinned`): the one being built and the one
    being replayed. Its out-of-memory rung holds the passes, blank, until the next frame takes over
    (`hires::HoldUntilNextFrame`) and unpins only the replayed frame; it can't defragment
    mid-frame (the frame's chain has the addresses baked in): that is fatal in strip mode. The
    level-change defragment holds the passes too.
  - **A world load copies the replayed chain to the heap** (`hires::LendHalves`, tag `HiRes`),
    relocating its NEXT and REF addresses, before the halves become the lump scratch; it is freed
    at the next `rs::BeginFrame` after a new frame takes over.
  - **GS privileged registers are write-only**: DISPLAY's DY is worked out again from
    `video::DtvDisplay`, not read back (reading DISPLAY2 gave pass lines past the frame's end).
  - **PCSX2 gives 720p NTSC's 525 lines a frame** (its log shows the rate as `??.???`) where a
    console has 750, so gsKit's pass 0 line (DY + 4 + 512) never came; VSINT now scales the
    schedule to the field it counted, which leaves a console's lines as gsKit has them. 1080i
    counted 562/563 a field, as a console does. PCSX2 also reads the display at vblank rather
    than behind the beam, so its live picture may show a strip's z-buffer; `screenshot` (each
    strip drawn alone and read back after its pass) is what to judge it by.
  - **Count the lines on EE timer T1, not with HSINT** (`ps2_hires_timer`, default 1). gsKit's
    HSINT-per-line counter cost the EE a quarter of its time in PCSX2: the 720p perf demos ran
    EntGeom at 0.331 µs a triangle against NTSC's 0.262, World 6.60 against 5.33 µs a surface,
    every stage alike (`build/baselines/hires-720p-perf1.flog`). T1 clocked by H-blank, its
    compare armed at each VSINT for the earliest pass and re-armed by each compare interrupt, is
    seven interrupts a frame with the FINISHes: 0.271 and 5.42 µs (+1.6 to +3.4%, the pass thread's
    own work; `hires-720p-timer-demos.emulog.txt`). ps2sdk takes T2 and T3; T0 and T1 are free.
  - Checked in PCSX2: scripted shots of the attract demo at 720p (3 strips) and 1080i match the
    480i scene at the wider 16:9 view, the HUD at 2x. The 720p perf demos: 3 passes and one
    adoption a frame, none late, no frame over a field, chains up to 649 KB of the 768, the world
    loads through the heap copy; the cinematics too. NTSC on the same build matched the GPU
    sampler capture within 1% (`hires-ntsc-after-perf1.flog`).
- **The picture's place on the TV is DISPLAY's DX/DY**, `graph_set_screen`'s x/y adjustments over
  libgraph's origin for the standard (NTSC 652/50, PAL 680/72): 4 VCK per pixel at 640 wide (DW
  2560, MAGH 3) and 2 raster lines per framebuffer row in FIELD mode. `ps2_screen_x/y` (ref.cpp,
  clamped to ±32/±16 and handed to `video::SetScreenOffset` each frame) keep them, `gs::Init` sets
  them and `PresentFramebuffer` rewrites them after its vsync wait when they changed, and the error
  screen's `DisplayX/Y` carry them. Nothing is redrawn: the TV shows BGCOLOR where the picture moved
  from, and clips it at the blanking (a few lines of room above on NTSC). The boot splash draws at
  the standard's own origin, before any config. PCSX2 ignores DX/DY unless `pcrtc_offsets = true`,
  and `screenshot` reads VRAM, so it never shows the shift either way.
- **Alpha 0x80 = 1.0.** 0xFF is about 2× overbright under `(Cs-Cd)*As/128+Cd`. Scale
  engine-facing 0..255 alpha with `a >> 1`. In MODULATE, vertex colour 0x80 is identity.
- The ALPHA register computes `(A - B) * C + D` where **C is a scalar alpha** (As/Ad/FIX), not
  a colour. Colour × colour (GL's `GL_ZERO, GL_SRC_COLOR`) is not expressible. That is why
  lightmaps are alpha-only intensity atlases, with chroma per vertex. Check this before
  promising any two-texture multiply.
- libdraw's `draw_enable_blending()`/`draw_disable_blending()` toggle a **static read at
  primitive emission** (PRIM.ABE), not a register write. Set it per emission. A global enable
  makes opaque fills blend about 2× bright.
- `draw_setup_environment()` defaults to CLAMP wrap (program REPEAT afterwards where tiling
  is needed) and an alpha test of NOTEQUAL 0 that discards A==0 texels. That is how conchars
  transparency works (palette index 255 has alpha 0). `ATEST_KEEP_*` names what is
  *preserved*: `ATEST_KEEP_FRAMEBUFFER` == GS `ZB_ONLY`. Any value where 0 is meaningful,
  such as a black lightmap luxel, must clamp to 1, or it draws nothing.
- **SCISSOR doesn't clip HOST→LOCAL uploads.** Writes past the destination width wrap in VRAM
  (for a 640-wide PSMCT32 buffer, to `(x-640, y+32)`).
- **One XGKICK with many GIF tags: only the last tag may set EOP.** The GIF stops at the
  first EOP, so EOP on every fan's tag drew only the first fan. Intermediate tags get EOP=0.
  On the VU, remember the last tag's address and OR in `0x8000` after the loop. Keep PRE=1 on
  each tag so the GS starts a new fan.
- `draw_rect_textured` = 4 qwords per sprite. A full console of text is about 9K qwords.

## Texture coordinates and sizes

- **Normalized ST spans the TEX0 TW/TH extent, the image rounded *up* to a power of two**, not
  the image. Every MD2 skin is non-power-of-two (276x194 samples as 512x256). On screen this
  looks like a UV-flip bug, but it isn't one: GS and GL both have T=0 at the first row. Scale
  coordinates in [0,1] with `tex::StScaleFor()`. A *tiling* non-POT texture needs resampling
  on load (33 of 2118 `.wal` files), not a coordinate scale. The 2D path uses `PRIM_MAP_UV`
  texel coordinates and is unaffected.
- **Keep S/T small: a console's GS loses precision on large values, and PCSX2 doesn't.** id's
  wall projection gives coordinates relative to the texture's world origin. In base2 a face's
  largest |S| or |T| has a median of 13.5 repeats and a max of 160 (16x16 tiles reach the most).
  On hardware some walls drew stretched and warped, and others swam as the camera moved. PCSX2's
  Metal renderer showed none of it. The world loader now shifts each face's coordinates by whole
  repeats so the smallest lands in [0, 1) (`ps2_st_rebase`, `mod::SetRebaseWallSt`). Under
  REPEAT this samples the same texels, mip levels included, and leaves base2's max at 15.
  Confirmed on hardware 2026-10-10, with the guard band at its usual 0.8. A first run had also
  narrowed the band to 0.25 through a test cvar, since removed. A second run without it showed
  the walls were fixed by the shift alone. Anything new that sends large S/T is a candidate for
  the same artifacts.
- **Water had the same problem, and the loader can't shift it.** It arrives in raw texels, and
  the ripple's phase is a function of them, with a 16pi-texel period that no whole number of
  64-texel repeats is a multiple of. On a console its texture wobbled as the camera turned,
  most visibly on the triangles the camera stood in (2026-10-11). So VU1 shifts the *output*
  by whole repeats, per triangle, after the warp (`warpCorners` in textured_triangles.vusl).
  Confirmed on hardware 2026-10-11: the wobble and the cut triangles' distortion were gone.

## CLUTs and palettized textures

- Most textures are PSMT8, sampling Quake's palette. Four CLUTs, at fixed VRAM outside the
  heap (4 KB of one page): the global palette, the lit palette (`ps2_intensity`; walls, skins,
  sprites), the alpha ramp (`Alpha8`, the particles' coverage) and the light ramp (`Light8`,
  the lightmap atlases). The palettes and the light ramp carry the brightness curve
  (`gamma.h`, `vid_gamma`/`vid_contrast`); the alpha ramp never changes, since coverage isn't
  light.
- **CSM1 reorder:** in every 32-entry group the two middle 8-entry blocks swap:
  `(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)`, which is its own inverse. If you
  forget it, colours come out scrambled but stable.
- A CLUT uploads as a 16x16 PSMCT32 image: exactly 4 blocks at CBP, dest width 64 (DBW=1).
  CBP is in blocks (word address >> 6).
- **CLD:** `gs::MakeTex0` binds indexed textures with `CLUT_COMPARE_CBP0` (CLD=4), and
  `gs::Init`'s `SeedClutBuffer` writes TEX2 with CLD=2 so CBP0 starts known. Invariants: every
  indexed TEX0 uses CLD=4 (one CLD=1 load leaves CBP0 stale), and CLUT contents change only
  through `gs::UpdateCluts`, between frames after `rs::FinishFrameInFlight`, which seeds the
  buffer again: the compare is on the address only, so a bind of the CLUT whose contents
  changed would keep the old copy. CLD is ignored for non-indexed formats.
- **A brightness change is one frame of wait:** `gamma::ApplyPendingChange` runs in `PS2_BeginFrame`,
  just before `rs::BeginFrame`, finishes the frame `ps2_gs_latency` left drawing, and uploads the four
  CLUTs (4 KB). A model's shade light takes the curve from a 257-entry table (`LightCurve`),
  not `powf`, since it runs per model per frame.
- **TBW must be even for PSMT8/PSMT4** (128-px stride). Narrow 8-bit textures round their
  stride up for TEX0 and the upload (`TextureStridePixels()`).
- Page sizes: PSMT8 128x64, PSMCT32 64x32, PSMCT16/16S 64x64. libdraw's
  `draw_texture_transfer` takes `dest_width` separately, and DBW = dest_width >> 6, so a
  16-px transfer needs `dest_width >= 64`.

## VRAM and mipmaps

- **The GS addresses textures in 256-byte blocks, not pages.** Any block-aligned TBP is
  valid. PSMT8's block order equals PSMCT32's (PCSX2 `_blockTable8 == _blockTable32`), so a
  texture occupies exactly `[TBP, TBP + far-corner block + 1)`. Size by
  `vram::TextureFootprintWords`, never by whole pages. Block granularity cut city3's walls
  from 2024 KB to 1391 KB.
- Mip levels share the base allocation, first-fit by block (`vram::MipLayout`, one per POT
  size). Each level has its own TBP/TBW via MIPTBP1, and PSMT8 TBW stays even. TEX1.MTBA
  auto-layout only handles square sizes.
- Bilinear needs every level ≥ 8 texels per side: `MXL = min(3, log2(min(w,h)) - 3)`.
- LOD: LCM=0 gives `LOD = log2(1/Q) + K`. With Q = 1/view depth, `K = -log2(f)`,
  `f = (screenH/2)·cot(fovY/2)` = 320 at 640x448, fov 90. LOD is depth-only, so grazing
  floors under-filter (`ps2_mip_bias` tunes it).
- Cvars: `ps2_mipmaps` (load-time, 0 = old behaviour exactly), `ps2_mip_filter`
  nearest/bilinear/trilinear (live), `ps2_mip_bias` (live).
- Lightmap atlases are `ImageType::Wall` (Alpha8). Exclude them by format when a rule means
  "walls".
- Before changing the heap or residency policy, read the README's VRAM heap notes: LRU, never
  evict textures bound this frame, recoverable OOM, defrag on level change.
