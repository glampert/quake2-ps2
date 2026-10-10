
# PS2 backend cvars

Every cvar registered by the PS2 backend under [src/ps2/](src/ps2/), with its default in the
debug (`make`) and release (`make release`) builds. See the [README](README.md) for the
bigger picture.

- **Arch.** marks `CVAR_ARCHIVE` cvars: they are written to `config.cfg` and read back on
  the next boot, so the value in your config, not the default, is what a run starts with.
- **Menu** marks the cvars the game menu's video options page sets
  ([vid.cpp](src/ps2/renderer/vid.cpp)); the page says which apply live and which need a
  map load or a restart.
- **—** in the release column means the cvar is not registered at all there: the code that
  reads it is compiled out with `PS2_QUAKE_DEBUG` / `PS2_QUAKE_PROFILE`. Archived cvars never
  get a **—**. Every build registers them, even where nothing reads them, because the quit
  drops an archived cvar the build never registered from `config.cfg`.

## Renderer: video and frame

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_fb_width` | `640` | `640` | Arch., Menu | Framebuffer width in pixels (512 or 640); read at startup. |
| `ps2_video_mode` | `auto` | `auto` | Arch., Menu | TV standard: `auto` (the console's own), `ntsc` (59.94 Hz) or `pal` (50 Hz); read at startup. A PAL console can drive NTSC if the TV takes 60 Hz. The fatal-error screen follows it too. |
| `ps2_fb_height` | `0` | `0` | Arch., Menu | Framebuffer height in pixels: 0 (auto) is the video mode's full height, 448 for NTSC and 512 for PAL; a taller value than the mode shows is clamped. 512 lines leave 240 KB less VRAM for textures. Read at startup. |
| `ps2_fb_16bit` | `1` | `1` | Arch., Menu | 16-bit colour buffers (more VRAM for textures) instead of 32-bit; read at startup. |
| `ps2_fb_dither` | `0` | `0` | Arch., Menu | GS dithering, to hide the 16-bit framebuffer's colour banding. |
| `ps2_gs_latency` | `1` | `1` | Arch., Menu | Let the GS draw a frame while the next is built: faster, one frame more input lag. |
| `ps2_intensity` | `2` | `2` | Arch., Menu | Brightness scale of walls and models (`ref_gl`'s `intensity`, floored at 1); read at startup. |
| `ps2_polyblend` | `1` | `1` | | Fullscreen damage/powerup/underwater tint (`ref_gl`'s `gl_polyblend`). |

## Renderer: textures and sky

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_mipmaps` | `1` | `1` | Arch., Menu | Load walls with their WAL mip levels; applies on the next map load. |
| `ps2_mip_filter` | `bilinear` | `bilinear` | Arch., Menu | Wall and model skin filtering: `nearest`, `bilinear` or `trilinear`. |
| `ps2_mip_bias` | `0` | `0` | Arch., Menu | Wall mip level bias, in levels; positive is blurrier. |
| `ps2_st_rebase` | `1` | `1` | Arch. | Shift each wall face's texture coordinates by whole repeats so they start near zero, which keeps precision the GS would lose on large values; the same texels either way. Applies on the next map load. |
| `ps2_skymip` | `0` | `0` | Arch., Menu | Load the sky faces at half resolution to save VRAM (`ref_gl`'s `gl_skymip`). |
| `ps2_sky_full_bounds` | `0` | `0` | | Debug: draw all six sky faces whole, ignoring what is visible. |

## Renderer: lighting

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_dynamic_lightmaps` | `2` | `2` | Arch., Menu | Dynamic lights: 0 = flares, 1 = per-luxel lightmap rebuild, 2 = per-vertex point lights on VU1. |
| `ps2_dlight_scale` | `0.1` | `0.1` | Arch., Menu | Brightness of the VU1 point lights in mode 2. |
| `ps2_lightmap_modulate` | `1` | `1` | Arch. | Scales every luxel and the entity lighting (`ref_gl`'s `gl_modulate`). |
| `ps2_lightstyle_epsilon` | `0.3` | `0.3` | Arch. | How far an animated light style must move before its surfaces are rebaked; 0 is exact. |
| `ps2_lightmap_color` | `1` | `1` | Arch. | Debug: 0 drops the per-vertex lightmap chroma, leaving lighting monochrome. |
| `ps2_lightmaps` | `1` | `1` | | Debug: 0 drops the lightmap pass, leaving the world fullbright. |
| `ps2_lightmap_only` | `0` | `0` | | Debug: 1 drops the diffuse textures, showing the lighting alone. |

## Renderer: MD2 models

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_md2_shadows` | `1` | `1` | Arch., Menu | Blob shadows under monsters and items. |
| `ps2_md2_lerp_on` | `1` | `1` | | Interpolate between animation keyframes; 0 snaps to the current frame. |
| `ps2_md2_vu_lerp` | `1` | `1` | | Do the keyframe lerp on VU1 rather than on the EE. |
| `ps2_md2_cullface` | `1` | `1` | | Model face culling: 0 = none, 1 = negative, 2 = positive winding. |

## Renderer: isolating parts of the frame

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_skip_world` | `0` | `0` | | Skip all world geometry. |
| `ps2_skip_alpha_surfaces` | `0` | `0` | | Skip the translucent glass/water pass. |
| `ps2_skip_brushmodels` | `0` | `0` | | Skip brush model entities (doors, platforms, props). |
| `ps2_skip_sprites` | `0` | `0` | | Skip sprites. |
| `ps2_skip_entities` | `0` | `0` | | Skip all entities. |
| `ps2_skip_particles` | `0` | `0` | | Skip particles. |
| `ps2_skip_sky` | `0` | `0` | | Skip the sky pass. |
| `ps2_skip_weapon_model` | `0` | `0` | | Skip the view weapon. |
| `ps2_force_null_models` | `0` | `0` | | Draw every entity as the octahedron placeholder. |

## Debug overlays and profiling

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_show_fps` | `1` | `0` | Arch., Menu | FPS counter at the top right; independent of `ps2_debug_overlays`. |
| `ps2_debug_overlays` | `1` | `0` | Arch., Menu | Master switch for the profile, memory, VRAM and draw stats panels below. |
| `ps2_show_memstats` | `1` | `0` | Arch. | Per-tag heap usage panel. |
| `ps2_show_vramstats` | `1` | `0` | Arch. | Texture heap occupancy and per-frame uploads panel. |
| `ps2_show_drawstats` | `1` | `0` | Arch. | Nodes, surfaces, triangles, batches, entities, particles and dlights per frame. |
| `ps2_show_profile` | `1` | `0` | Arch. | Per-stage frame timings panel; does nothing in release, where the profiler is compiled out. |
| `ps2_frame_log` | `0` | — | | Log per-frame timings and counters to stdout in batches, for the `frame_log` scripts. |
| `ps2_logfile` | `0` | `0` | | Copy the console, `Sys_Error` and the reports printed before it (pipeline hang dump, stack traces, out-of-memory stats) to `quake2.log` next to `baseq2/`, one open/append/close per write so a hang leaves the log on the drive. Defaults to `1` in a build with `PS2_QUAKE_LOAD_TRACE` set to 1. |
| `ps2_perftest` | `0` | `0` | Arch. | Unattended perf run: plays the attract demos with the frame log on, then quits; does nothing in release, where the profiler is compiled out. |

## Sound and music

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_disable_sound` | `0` | `0` | Arch. | Skip audio bring-up at boot and run silent. |
| `cd_nocd` | `0` | `0` | Arch. | Turn the streamed soundtrack off (the Options menu's "CD music" toggle). |
| `cd_volume` | `0.7` | `0.7` | Arch. | Music volume, 0-1 (the Options menu's "music volume" slider). |
| `cd_loopcount` | `4` | `4` | | Times a map's track plays before switching to `cd_looptrack`. |
| `cd_looptrack` | `11` | `11` | | The ambient track looped for good once `cd_loopcount` runs out. |

## Input

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `in_joystick` | `1` | `1` | Arch. | Use the gamepad's analog sticks (the Options menu's joystick toggle). |
| `joy_yawsensitivity` | `1` | `1` | | Right stick horizontal (turn) speed; negative inverts the axis. |
| `joy_pitchsensitivity` | `1` | `1` | | Right stick vertical (look) speed; negative inverts the axis. |
| `joy_forwardsensitivity` | `1` | `1` | | Left stick vertical (move) speed; negative inverts the axis. |
| `joy_sidesensitivity` | `1` | `1` | | Left stick horizontal (strafe) speed; negative inverts the axis. |
| `joy_yawthreshold` | `0.15` | `0.15` | | Right stick horizontal dead zone, as a fraction of full deflection. |
| `joy_pitchthreshold` | `0.15` | `0.15` | | Right stick vertical dead zone. |
| `joy_forwardthreshold` | `0.15` | `0.15` | | Left stick vertical dead zone. |
| `joy_sidethreshold` | `0.15` | `0.15` | | Left stick horizontal dead zone. |
| `in_rumble` | `1` | `1` | Arch. | Gamepad force feedback (the Options menu's "gamepad rumble"). |
| `in_rumbledebug` | `0` | `0` | | Print each rumble effect as it starts. |
| `in_keyboard` | `1` | `0` | Arch. | Bring up the USB keyboard driver (one-shot; the IOP modules load once). A keyboard is polled every frame while in use (a key within 10 s, or one held), four times a second otherwise. |
| `in_keyboarddebug` | `0` | `0` | | Print every raw USB scan code the keyboard driver delivers. |

## Save games

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_savedevice` | `host` | `host` | Arch. | Where saves go when running from `host:`: `host` (`baseq2/save/`) or `mc` (memory card). |

## Bring-up scenes and tests

All debug builds only.

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `ps2_testcube` | `0` | — | | Draw the spinning textured cube, a VU1 path smoke test. |
| `ps2_testcube_tess` | `8` | — | | Cube face tessellation (N x N quads), to exercise chunked batch submission. |
| `ps2_testcube_vram_tex_eviction` | `0` | — | | Slide the cube's textures through a window that forces VRAM evictions and re-uploads. |
| `ps2_testcube_vulerp` | `0` | — | | Draw the cube through the MD2 keyframe-lerp VU path. |
| `ps2_testcin` | `0` | — | | Play every stock cinematic in turn. |
| `ps2_testmaps` | `0` | — | | Load all 39 stock maps in order, logging memory use per map. |
| `ps2_testmaps_dwell` | `8` | — | | Seconds `ps2_testmaps` stays in each map once loaded. |
| `ps2_testsaves` | `0` | — | | Save game test: 1 = host files, 2 = memory card, 3 = load an earlier run's save only. |

## Engine cvars with PS2-specific defaults

Registered by id's code rather than the backend, but set up or read differently here:

| Cvar | Debug | Release | Flags | Description |
| --- | --- | --- | --- | --- |
| `developer` | `1` | `0` | | Engine debug prints (`Com_DPrintf`); the default follows the build here. |
| `s_khz` | `22` | `22` | Arch. | Sound output rate: 11, 22 or 44 (kHz); anything else falls back to 22. |
