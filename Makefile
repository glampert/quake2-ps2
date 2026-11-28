# ============================================================================
#  Quake II PS2 - build system
# ----------------------------------------------------------------------------
#  Produces build/<config>/quake2.elf for the PS2 EE, using the modern ps2dev
#  toolchain (mips64r5900el-ps2-elf-*). Built on the PS2SDK sample makefiles.
#
#    make             -> debug build -> build/debug/quake2.elf   (VSCode: Shift+Cmd+B)
#    make release     -> optimized   -> build/release/quake2.elf
#    make run         -> build + launch in PCSX2  (VSCode: F5)
#    make release run -> the same, with the release build
#    make iso         -> disc build (release) + build/iso_release/quake2.iso from baseq2/
#    make iso_dbg     -> disc build (debug)   + build/iso_debug/quake2.iso
#    make iso run     -> the same, then boot the image in PCSX2 (iso_dbg run likewise)
#    make tools       -> build host tools (imgdump, unpak, bspinfo, musenc, pakzip) into build/tools/
#    make music       -> encode baseq2/music/trackNN.wav into the trackNN.adp files the game streams
#    make pkz         -> convert baseq2/pakN.pak into the compressed pakN.pkz the game prefers
#    make clean       -> remove build artifacts (both configs)
#    make clean_vu    -> remove only the compiled VU microprograms and their headers
#
#  Header dependencies are tracked automatically (-MMD), so editing a header
#  rebuilds just the affected objects; no more `make clean` after header edits.
# ============================================================================

# Toolchain / tool locations (override from the environment if needed):
PS2DEV ?= /Users/guilherme/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
PCSX2  ?= /Applications/PCSX2.app/Contents/MacOS/PCSX2

# ----------------------------------------------------------------------------
#  Build configuration
# ----------------------------------------------------------------------------
#
#  Picked by the `release`, `iso` or `iso_dbg` goal, or by BUILD= on the command line:
#
#      make          -> debug       -O2, DWARF info, asserts and debug-only code IN
#      make release  -> release     -O3, no DWARF, asserts and debug-only code OUT
#      make iso      -> iso_release the release build, reading its data off a disc
#      make iso_dbg  -> iso_debug   the debug build, the same
#
#  The disc builds (PS2_QUAKE_ISO=1) load the game data from the CD/DVD drive
#  (ps2/fs/cdvd_device.h) - a disc image booted by Open PS2 Loader or PCSX2 -
#  instead of host:, the HDD or USB, and save to the memory card only. Their
#  goals also make the image, build/<config>/quake2.iso, from ISO_DATA (baseq2/
#  by default) with src/tools/scripts/make_iso.py; `make BUILD=iso_release`
#  builds the ELF alone. The repository has no game data to put on one.
#
#  Each config owns its object tree under build/<config>/, so alternating
#  between them neither mixes objects built with different flags nor forces
#  a full rebuild.

CONFIG_GOALS = $(filter release iso iso_dbg,$(MAKECMDGOALS))
ifneq ($(word 2,$(CONFIG_GOALS)),)
    $(error Pick one of release, iso and iso_dbg, not '$(CONFIG_GOALS)')
endif

ifeq ($(CONFIG_GOALS),release)
    BUILD := release
else ifeq ($(CONFIG_GOALS),iso)
    BUILD := iso_release
else ifeq ($(CONFIG_GOALS),iso_dbg)
    BUILD := iso_debug
else
    BUILD ?= debug
endif

ifeq ($(filter $(BUILD),debug release iso_debug iso_release),)
    $(error BUILD must be 'debug', 'release', 'iso_debug' or 'iso_release', got '$(BUILD)')
endif

# 1 for the disc builds. RELEASE_FLAGS picks the optimized flags below for either kind of release.
ISO_BUILD     = $(if $(filter iso_%,$(BUILD)),1,0)
RELEASE_FLAGS = $(if $(filter release iso_release,$(BUILD)),1,0)

# Strip the linked ELF - in BOTH configs, since the debug build is what gets
# iterated on and DWARF dominates its size (7.7 MB -> 1.7 MB). Costs nothing at
# runtime: the PS2 loader only reads program headers, which strip leaves alone.
# The symbols are not lost, they stay in quake2_unstripped.elf beside it (feed
# that one to addr2line to resolve a crash address). STRIP_ELF=0 turns this off
# and ships the unstripped ELF as quake2.elf instead.
STRIP_ELF ?= 1

SRC_DIR    = src
BUILD_DIR  = build
OUTPUT_DIR = $(BUILD_DIR)/$(BUILD)

# The SDK link rule (Makefile.eeglobal_cpp) produces $(EE_BIN) with full symbols;
# $(GAME_ELF) is the binary that actually runs, stripped out of it below.
EE_BIN   = $(OUTPUT_DIR)/quake2_unstripped.elf
GAME_ELF = $(OUTPUT_DIR)/quake2.elf

# ----------------------------------------------------------------------------
#  Source files
# ----------------------------------------------------------------------------

# New PS2 backend, modern C++:
PS2_CXX_SRC =                         \
	ps2/system/main.cpp               \
	ps2/system/sys.cpp                \
	ps2/system/iop_boot.cpp           \
	ps2/system/video_mode.cpp         \
	ps2/system/heap.cpp               \
	ps2/math/vec_mat.cpp              \
	ps2/net/net.cpp                   \
	ps2/input/input.cpp               \
	ps2/input/keyboard.cpp            \
	ps2/input/pad.cpp                 \
	ps2/input/rumble.cpp              \
	ps2/audio/snd.cpp                 \
	ps2/audio/audsrv_device.cpp       \
	ps2/audio/mix_ring.cpp            \
	ps2/audio/submit_thread.cpp       \
	ps2/audio/music_stream.cpp        \
	ps2/audio/sound_load.cpp          \
	ps2/audio/cd_audio.cpp            \
	ps2/save/save_api.cpp             \
	ps2/save/working_set.cpp          \
	ps2/save/slot_archive.cpp         \
	ps2/save/save_device.cpp          \
	ps2/save/memcard.cpp              \
	ps2/save/drive.cpp                \
	ps2/save/mc_icon.cpp              \
	ps2/save/save_notice.cpp          \
	ps2/fs/pkz_pack.cpp               \
	ps2/fs/async_io.cpp               \
	ps2/fs/readahead_stream.cpp       \
	ps2/fs/aio_pump.cpp               \
	ps2/renderer/gs.cpp               \
	ps2/renderer/gamma.cpp            \
	ps2/renderer/vram.cpp             \
	ps2/renderer/texture.cpp          \
	ps2/renderer/image_load.cpp       \
	ps2/renderer/model.cpp            \
	ps2/renderer/model_load.cpp       \
	ps2/renderer/lightmap.cpp         \
	ps2/renderer/scrap_atlas.cpp      \
	ps2/renderer/cinematic.cpp        \
	ps2/renderer/view.cpp             \
	ps2/renderer/md2.cpp              \
	ps2/renderer/sky.cpp              \
	ps2/renderer/profile.cpp          \
	ps2/renderer/vid.cpp              \
	ps2/renderer/ref.cpp              \
	ps2/renderer/vu1.cpp              \
	ps2/renderer/cmd_buffer.cpp       \
	ps2/renderer/render_system.cpp    \
	ps2/renderer/clip.cpp             \
	ps2/renderer/loading_screen.cpp   \
	ps2/renderer/splash_screen.cpp    \
	ps2/renderer/screenshot.cpp       \
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/tests/perf_run.cpp            \
	ps2/tests/save_test.cpp           \
	ps2/tests/test_quit.cpp           \
	ps2/tests/aio_test.cpp            \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp             \
	ps2/debug/log_file.cpp            \
	ps2/debug/load_trace.cpp          \
	ps2/debug/io_log.cpp              \
	ps2/debug/debug_menu.cpp          \
	ps2/builtin/palette.cpp           \
	ps2/builtin/conchars.cpp          \
	ps2/builtin/conback.cpp           \
	ps2/builtin/backtile.cpp          \
	ps2/builtin/m_main_logo.cpp

# The disc builds' game data device: a read-only ISO9660 file system over the CD/DVD drive.
ifeq ($(ISO_BUILD),1)
PS2_CXX_SRC += ps2/fs/cdvd_device.cpp
endif

# Doug Lea's allocator: vendored third-party C, left as C on purpose (see the
# note at the top of dlmalloc.c). Everything else of ours is C++.
PS2_C_SRC = ps2/system/dlmalloc/dlmalloc.c

# Stock Quake II engine / game / server - untouched C, statically linked.
# Sound output and the CD audio module are implemented in the backend, see ps2/audio/:
# with no CDVD path in this port (game data comes from host: or mass:), the CD tracks
# are streamed from baseq2/music/trackNN.adp files instead (cd_audio.cpp).
ENGINE_C_SRC = \
	client/cl_cin.c    client/cl_ents.c   client/cl_fx.c     client/cl_input.c \
	client/cl_inv.c    client/cl_main.c   client/cl_newfx.c  client/cl_parse.c \
	client/cl_pred.c   client/cl_scrn.c   client/cl_tent.c   client/cl_view.c  \
	client/console.c   client/keys.c      client/menu.c      client/qmenu.c    \
	client/snd_dma.c   client/snd_mem.c   client/snd_mix.c                     \
	common/cmd.c       common/cmodel.c    common/common.c    common/crc.c      \
	common/cvar.c      common/filesys.c   common/md4.c       common/net_chan.c \
	common/pmove.c                                                             \
	game/g_ai.c        game/g_chase.c     game/g_cmds.c      game/g_combat.c   \
	game/g_func.c      game/g_items.c     game/g_main.c      game/g_misc.c     \
	game/g_monster.c   game/g_phys.c      game/g_save.c      game/g_spawn.c    \
	game/g_svcmds.c    game/g_target.c    game/g_trigger.c   game/g_turret.c   \
	game/g_utils.c     game/g_weapon.c    game/q_shared.c    game/p_weapon.c   \
	game/m_actor.c     game/m_berserk.c   game/m_boss2.c     game/m_boss3.c    \
	game/m_boss31.c    game/m_boss32.c    game/m_brain.c     game/m_chick.c    \
	game/m_flash.c     game/m_flipper.c   game/m_float.c     game/m_flyer.c    \
	game/m_gladiator.c game/m_gunner.c    game/m_hover.c     game/m_infantry.c \
	game/m_insane.c    game/m_medic.c     game/m_move.c      game/m_mutant.c   \
	game/m_parasite.c  game/m_soldier.c   game/m_supertank.c game/m_tank.c     \
	game/p_client.c    game/p_hud.c       game/p_trail.c     game/p_view.c     \
	server/sv_ccmds.c  server/sv_ents.c   server/sv_game.c   server/sv_init.c  \
	server/sv_main.c   server/sv_send.c   server/sv_user.c   server/sv_world.c

C_SRC   = $(PS2_C_SRC) $(ENGINE_C_SRC)
CXX_SRC = $(PS2_CXX_SRC)

# Backend sources that run at load time or not at all in a normal frame: asset
# parsing, IOP module boot, device setup, the debug screen printer. None of them
# are on the per-frame path, so they are built for size instead of speed - worth
# ~9 KB of .text, which is RAM the levels get to use instead.
#
# The hot renderer (view/md2/sky/gs/vu1/vram/lightmap/ref),
# the math backend and the whole stock engine keep $(EE_OPTFLAGS).
SIZE_OPT_CXX_SRC =                    \
	ps2/renderer/model_load.cpp       \
	ps2/renderer/image_load.cpp       \
	ps2/renderer/texture.cpp          \
	ps2/renderer/model.cpp            \
	ps2/renderer/scrap_atlas.cpp      \
	ps2/system/iop_boot.cpp           \
	ps2/system/video_mode.cpp         \
	ps2/audio/audsrv_device.cpp       \
	ps2/input/keyboard.cpp            \
	ps2/input/pad.cpp                 \
	ps2/input/rumble.cpp              \
	ps2/renderer/vid.cpp              \
	ps2/renderer/loading_screen.cpp   \
	ps2/renderer/splash_screen.cpp    \
	ps2/renderer/screenshot.cpp       \
	ps2/save/save_api.cpp             \
	ps2/save/slot_archive.cpp         \
	ps2/save/save_device.cpp          \
	ps2/save/memcard.cpp              \
	ps2/save/drive.cpp                \
	ps2/save/mc_icon.cpp              \
	ps2/save/save_notice.cpp          \
	ps2/fs/pkz_pack.cpp               \
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/tests/perf_run.cpp            \
	ps2/tests/save_test.cpp           \
	ps2/tests/test_quit.cpp           \
	ps2/tests/aio_test.cpp            \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp             \
	ps2/debug/log_file.cpp            \
	ps2/debug/load_trace.cpp          \
	ps2/debug/io_log.cpp              \
	ps2/debug/debug_menu.cpp

SIZE_OPT_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(SIZE_OPT_CXX_SRC:.cpp=.o))

C_OBJS   = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(C_SRC:.c=.o))
CXX_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(CXX_SRC:.cpp=.o))

# VU microprograms, written in VUSL: oxide-vusl -> dvp-as. Each .vusl compiles
# into a .vsm, assembled into .vudata with <name>_CodeStart/_CodeEnd link symbols
# (see PS2_DECLARE_VU_MICROPROGRAM in ps2/renderer/vu1.h), and a C++ header,
# <name>.vusl.h, with the program's memory layout: vu1.h includes it and checks
# the host's structs and constants against it. None of the EE compiler flags
# reach this toolchain, so the output depends on one setting only, the GPU
# sampler's (below), which adds the programs' counters: build it once per value,
# into build/vu/ (off, what a release ships) or build/vu_sampler/ (on), and share
# it between the configs that agree on it.
VUSL_PATH    = $(SRC_DIR)/ps2/renderer/vu1progs
VUSL_FILES   = textured_triangles.vusl particles.vusl
VU_DIR       = $(BUILD_DIR)/vu$(if $(filter 1,$(GPU_SAMPLER)),_sampler,)
VU_OBJS      = $(addprefix $(VU_DIR)/, $(VUSL_FILES:.vusl=.o))
VU_HEADERS   = $(addprefix $(VU_DIR)/, $(VUSL_FILES:.vusl=.vusl.h))

# Shared include files (frame.vush). The programs include these by bare name,
# which the preprocessor finds next to the including file; they are prerequisites
# here because the pattern rule below would not otherwise see them change.
VUSL_INCS    = $(wildcard $(VUSL_PATH)/*.vush)

# oxide-vusl (https://github.com/glampert/oxide-vcl), the VUSL compiler the VU
# programs are built with: it compiles VUSL to VCL and hands that to oxide-vcl in
# the same process, in oxide mode with every fix and optimization. It is not part
# of the ps2dev distribution: it comes in as the oxide-vcl git submodule and is
# built by cargo into build/tools/oxide-vcl/, so every VU build runs the pinned
# version. --warnings-as-errors fails the build on any warning, the
# preprocessor's or the compiler's.
OXIDE_VCL_PATH   = $(SRC_DIR)/tools/oxide-vcl
OXIDE_VCL_BUILD  = $(BUILD_DIR)/tools/oxide-vcl
OXIDE_VUSL       = $(OXIDE_VCL_BUILD)/release/oxide-vusl
OXIDE_VUSL_FLAGS = --warnings-as-errors -DPS2_QUAKE_GPU_SAMPLER=$(GPU_SAMPLER)

# miniz (https://github.com/richgel999/miniz), the deflate codec the save games are
# compressed with (ps2/save/working_set.cpp). A git submodule like oxide-vcl; only the raw
# deflate/inflate and CRC-32 sources are built, straight from the submodule. Its headers
# include a miniz_export.h that miniz's CMake would generate: the one in MINIZ_CFG_PATH
# stands in for it and also carries the build configuration, so the library and every
# file including miniz.h agree on it (struct sizes depend on TDEFL_LESS_MEMORY).
MINIZ_PATH     = $(SRC_DIR)/tools/miniz
MINIZ_CFG_PATH = $(SRC_DIR)/ps2/save/miniz_cfg
MINIZ_SRC      = miniz.c miniz_tdef.c miniz_tinfl.c
MINIZ_OBJS     = $(addprefix $(OUTPUT_DIR)/miniz/, $(MINIZ_SRC:.c=.o))

# The name tables the game saves function and mmove_t pointers through (game/g_save.c),
# generated from the compiled game objects - see the script for the details.
SAVE_TABLES_GEN = $(SRC_DIR)/tools/scripts/gen_save_tables.py
SAVE_TABLES_C   = $(OUTPUT_DIR)/gen/g_save_tables.c
SAVE_TABLES_O   = $(OUTPUT_DIR)/gen/g_save_tables.o
GAME_C_OBJS     = $(filter $(OUTPUT_DIR)/$(SRC_DIR)/game/%.o, $(C_OBJS))

# Standalone command line tools: the C++ ones under src/tools/host, built with the
# HOST C++ compiler (not the EE toolchain) since they run on the development
# machine, and the Python ones under src/tools/scripts. Being host binaries they
# are config-independent, so they live outside build/<config>/.
#
# The C++ tools are a CMake project (src/tools/host/CMakeLists.txt), so they build the
# same way on every host, Windows included. It owns their compiler flags and the host
# configuration of miniz that pakzip links (full-size compressor tables, unlike the
# game's). Its build tree is build/tools/cmake/; the binaries land in build/tools/.
HOST_TOOLS_PATH  = $(SRC_DIR)/tools/host
HOST_TOOLS_CMAKE = $(BUILD_DIR)/tools/cmake
SCRIPTS_PATH     = $(SRC_DIR)/tools/scripts
TOOLS_CXX_BINS   = $(addprefix $(BUILD_DIR)/tools/, imgdump unpak bspinfo musenc pakzip)
TOOLS_PY_BINS    = $(addprefix $(BUILD_DIR)/tools/, symbolize)
HOST_CXX        ?= c++
HOST_CC         ?= cc

TOOLS_BINS = $(TOOLS_CXX_BINS) $(TOOLS_PY_BINS)

# IOP/IRX modules embedded into the ELF: HDD/PFS and BDM USB storage, booted
# by ps2/system/iop_boot.cpp when the game data isn't on host:,
# the USB keyboard driver started on demand by ps2/input/keyboard.cpp, and the
# sound driver pair (libsd under audsrv) started by ps2/audio/audsrv_device.cpp.
# A disc build reads its data through the IOP's own CD drivers (the boot ROM's, or OPL's) and has
# no keyboard, so it carries only the sound driver pair.
IRX_PATH  = $(PS2SDK)/iop/irx
ifeq ($(ISO_BUILD),1)
IRX_FILES = libsd.irx audsrv.irx
else
IRX_FILES = iomanX.irx fileXio.irx \
            ps2dev9.irx ps2atad.irx ps2hdd.irx ps2fs.irx \
            bdm.irx bdmfs_fatfs.irx usbd.irx usbmass_bd.irx \
            ps2kbd.irx libsd.irx audsrv.irx
endif

IRX_OBJS  = $(addprefix $(OUTPUT_DIR)/irx/, $(IRX_FILES:.irx=.o))

EE_OBJS = $(C_OBJS) $(CXX_OBJS) $(VU_OBJS) $(IRX_OBJS) $(MINIZ_OBJS) $(SAVE_TABLES_O)
DEPS    = $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d) $(MINIZ_OBJS:.o=.d)

# ----------------------------------------------------------------------------
#  Compiler / linker flags (appended to the SDK defaults from Makefile.eeglobal)
# ----------------------------------------------------------------------------

# Per-config flags. EE_OPTFLAGS and EE_DBGINFOFLAGS are the SDK's own knobs:
# Makefile.eeglobal_cpp defaults them with ?= (to -O2 and -gdwarf-2 -gz), so
# whatever is set here wins - including setting the debug info to empty.
#
# PS2_QUAKE_DEBUG, PS2_QUAKE_ASSERTS, PS2_QUAKE_PROFILE and PS2_QUAKE_ISO are always defined
# to 0 or 1, never undefined. PS2_QUAKE_ISO comes after CONFIG_DEFS, so a CONFIG_DEFS given on
# the command line (performance.md's profiling builds) keeps it.
ifeq ($(RELEASE_FLAGS),1)
    EE_OPTFLAGS     = -O3
    EE_DBGINFOFLAGS =
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=0 -DNDEBUG
else
    EE_OPTFLAGS     = -O2
    EE_DBGINFOFLAGS = -gdwarf-2 -gz
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=1 -DPS2_QUAKE_ASSERTS=1 -DPS2_QUAKE_PROFILE=1
endif

# PS2_QUAKE_GPU_SAMPLER (cmd_buffer.h: the GPU sampler and the VU programs' counters) follows
# PS2_QUAKE_PROFILE, so debug and profiling builds have it and a release never does. A CONFIG_DEFS
# that sets it says otherwise, for an A/B of what the sampler itself costs. It reaches the VU
# programs too, which is why it is worked out here rather than in a header.
GPU_SAMPLER = $(or $(patsubst -DPS2_QUAKE_GPU_SAMPLER=%,%,$(filter -DPS2_QUAKE_GPU_SAMPLER=%,$(CONFIG_DEFS))),$(if $(filter -DPS2_QUAKE_PROFILE=1,$(CONFIG_DEFS)),1,0))

COMMON_DEFS = -DGAME_HARD_LINKED -DPS2_QUAKE $(filter-out -DPS2_QUAKE_GPU_SAMPLER=%,$(CONFIG_DEFS)) \
              -DPS2_QUAKE_GPU_SAMPLER=$(GPU_SAMPLER) -DPS2_QUAKE_ISO=$(ISO_BUILD)

EE_INCS += -I$(SRC_DIR)

# The VU programs' generated headers (see VU_HEADERS), included by vu1.h by bare name.
EE_INCS += -I$(VU_DIR)

# Floating-point semantics, for the engine C and the backend C++ alike. GCC already models
# the EE FPU as having no NaN or Inf (sqrtf is a bare sqrt.s, fabsf an abs.s), so what is
# left to give up is the sign of zero: -fno-signed-zeros lets GCC turn each float clamp
# (x < lo ? lo : x) into a branchless min.s/max.s and fold x + 0.0f away. The perf test
# measured Particles -13.7% (the per-particle alpha clamp) and EntCull -4.2%. The other two
# are near no-ops kept for intent: -fno-math-errno is already implied, -fno-trapping-math
# moves a few dozen instructions. The EE has no FP exceptions either way. Nothing may depend
# on the sign of a zero result: x / -0.0f saturates to -FLT_MAX, x / 0.0f to +FLT_MAX.
# Override with EE_FPFLAGS= (and a separate OUTPUT_DIR) for an A/B.
EE_FPFLAGS ?= -fno-math-errno -fno-trapping-math -fno-signed-zeros

# The C side of the build is now just id's C89 engine sources, the vendored
# dlmalloc and the bin2c IRX blobs - everything of ours is C++. Under GCC 15
# that C needs C89 forced, -fcommon restored, and the constructs GCC 14+
# promoted to hard errors downgraded so the untouched engine still compiles.
#
# -fsingle-precision-constant: id's code writes its float constants unsuffixed (x * 0.5),
# which C makes double - and the EE has no double FPU, so every one of those turned a float
# expression into libgcc soft-float calls. The backend's C++ needs no such flag: it uses f
# suffixes, and -Wdouble-promotion enforces them.
#
# -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast: Allow integer<=>pointer conversions,
# PS2 has 32bit pointers so sizeof(int) == sizeof(void*).
#
EE_CFLAGS += -std=gnu89 -fcommon -fno-strict-aliasing -fsingle-precision-constant $(COMMON_DEFS) \
	-Wno-implicit-function-declaration -Wno-missing-braces -Wno-int-conversion \
	-Wno-pointer-sign -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
	-Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function \
	-Wno-switch $(EE_FPFLAGS) -MMD -MP

# Strict, portable, warnings-as-errors for the new C++ backend (applies ONLY to
# our .cpp - the untouched engine C above stays lenient). The set targets
# portability and undefined behaviour: value-changing/alignment/format hazards,
# accidental float->double promotion (the EE has no hardware doubles), shadowing,
# VLAs, and GCC's near-zero-false-positive logic/duplicate-branch checks.
# -Wconversion/-Wsign-conversion flag every implicit value-, sign- or precision-
# changing conversion (all backend code must cast intentionally); SDK/STL library
# conversions are silenced via -isystem below, so only our own code is enforced.
EE_CXX_WARNFLAGS = -Wall -Wextra -Werror \
	-Wshadow -Wdouble-promotion -Wconversion -Wsign-conversion \
	-Wformat=2 -Wno-format-nonliteral -Wundef -Wpointer-arith \
	-Wcast-align -Wwrite-strings -Wredundant-decls -Wnull-dereference \
	-Wnon-virtual-dtor -Woverloaded-virtual -Wvla \
	-Wlogical-op -Wduplicated-cond -Wduplicated-branches

# Reclassify the PS2SDK headers as system headers for C++ so their own warnings
# (e.g. redundant redeclarations in kernel.h) don't trip our -Werror. The same
# dirs are still added via -I by Makefile.eeglobal; GCC then ignores the -I copy
# and treats them as system. Our own headers stay under -Isrc (warnings enforced).
EE_CXX_SYSINCS = -isystem $(PS2SDK)/ee/include -isystem $(PS2SDK)/common/include \
	-isystem $(MINIZ_PATH) -isystem $(MINIZ_CFG_PATH)

# Lean, embedded C++ for the new backend.
EE_CXXFLAGS += -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-strict-aliasing $(EE_FPFLAGS) $(COMMON_DEFS) \
	$(EE_CXX_WARNFLAGS) $(EE_CXX_SYSINCS) \
	-MMD -MP

# -leedebug supplies the level 1 exception vector that src/ps2/debug/exception_handler.cpp
# hangs its post-mortem off. It contributes nothing to a release build - the whole
# handler is behind PS2_QUAKE_DEBUG - but the linker only pulls in what is referenced,
# so leaving it on the line for both configs costs nothing.
EE_LIBS += -lkernel -ldraw -lgraph -lpacket2 -ldma -lpad -lkbd -laudsrv -lpatches -lfileXio -lmc -leedebug

# The disc builds read the drive through libcdvd. fileXio stays on the line: they call none of it,
# so none of it is linked.
ifeq ($(ISO_BUILD),1)
EE_LIBS += -lcdvd
endif

# ----------------------------------------------------------------------------
#  Rules
# ----------------------------------------------------------------------------

.PHONY: all release iso iso_dbg iso_image run tools host_tools music pkz clean clean_vu compiledb

all: $(GAME_ELF) tools

# `release` only selects the config (see BUILD above); the build itself is `all`. `iso` and
# `iso_dbg` select a disc build, and make its image too.
release: all
iso iso_dbg: iso_image

# The disc image of a disc build, from your own game data: make_iso.py picks what goes on it (the
# pkz over its pak, the music's .adp tracks, no saves or logs) and copies it; ~0.5 s for 290 MB.
# It runs on every make, as make can't see a change in ISO_DATA. ISO_FLAGS passes it more, e.g.
# --uncompressed-paks.
ISO_DATA  ?= baseq2
ISO_FLAGS ?=
ISO_IMAGE  = $(OUTPUT_DIR)/quake2.iso
MAKE_ISO   = $(SCRIPTS_PATH)/make_iso.py

iso_image: all
	@test "$(ISO_BUILD)" = 1 || { echo "iso_image needs a disc build: make iso or make iso_dbg"; exit 1; }
	python3 $(MAKE_ISO) --force --elf $(GAME_ELF) --data $(ISO_DATA) -o $(ISO_IMAGE) $(ISO_FLAGS)

# Records which STRIP_ELF setting the current $(GAME_ELF) was made with. make
# compares timestamps, not recipes, so without this a `make STRIP_ELF=0` over an
# already-built tree would leave the stripped ELF in place and report nothing to
# do. Flipping the flag switches to a marker that does not exist yet, which
# re-makes the ELF below.
STRIP_MARKER = $(OUTPUT_DIR)/.strip_elf-$(STRIP_ELF)

$(STRIP_MARKER):
	@mkdir -p $(dir $@)
	@rm -f $(OUTPUT_DIR)/.strip_elf-*
	@touch $@

# The runnable ELF, made from the symbol-carrying one the SDK link rule builds.
ifeq ($(STRIP_ELF),0)
$(GAME_ELF): $(EE_BIN) $(STRIP_MARKER)
	cp -f $< $@
else
$(GAME_ELF): $(EE_BIN) $(STRIP_MARKER)
	$(EE_STRIP) --strip-all -o $@ $<
endif

# Out-of-tree object rules. These static-pattern rules take precedence over the
# generic %.o rules from Makefile.eeglobal so objects land under build/<config>/
# mirroring the src/ tree. ($(EE_BIN) link rule comes from Makefile.eeglobal_cpp.)
#
# One rule per language: the C one is only reached by the engine and dlmalloc,
# every backend source goes through the C++ one.
$(C_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(CXX_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(EE_CXX) $(EE_CXXFLAGS) $(EE_INCS) $(CXX_OPTFLAGS_FOR) -c $< -o $@

# Per-object optimization override for the cold sources listed above. EE_CXXFLAGS
# already carries $(EE_OPTFLAGS) from Makefile.eeglobal; appending -Os after it
# wins, since the last -O on the command line is the one GCC applies. Target-
# specific variables are inherited by the rule above, so only these objects see it.
$(SIZE_OPT_OBJS): CXX_OPTFLAGS_FOR = -Os

# miniz, straight out of the submodule. Always -O3, in the debug config too: deflating
# a level's state is a few hundred KB of work on a slow CPU, done while the player waits.
# It is C99 (the engine's C is built as C89), and its warnings are not ours to fix.
$(MINIZ_OBJS): $(OUTPUT_DIR)/miniz/%.o: $(MINIZ_PATH)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) -std=gnu11 -O3 -w -I$(MINIZ_PATH) -I$(MINIZ_CFG_PATH) -c $< -o $@

# A clone without the submodule has no miniz sources, so the rule above has nothing to
# build from; this one stops with the fix instead. It never runs while the files exist.
$(MINIZ_PATH)/%.c:
	@echo "$(MINIZ_PATH) is empty - run 'git submodule update --init'"; exit 1

# The save tables list every function and mmove_t the game defines, so they are made
# again whenever a game object changes. They include no game headers (see the script),
# so they build like any other engine C file.
$(SAVE_TABLES_C): $(GAME_C_OBJS) $(SAVE_TABLES_GEN)
	@mkdir -p $(dir $@)
	@echo "gen_save_tables -> $@"
	@python3 $(SAVE_TABLES_GEN) --nm $(EE_TOOL_PREFIX)nm --src $(SRC_DIR)/game -o $@ $(GAME_C_OBJS)

$(SAVE_TABLES_O): $(SAVE_TABLES_C)
	$(EE_CC) $(EE_CFLAGS) -c $< -o $@

# oxide-vusl, from the oxide-vcl submodule, through cargo. The up-to-date check is
# made here, on the crates' sources and manifests, so the rule runs again only when
# checking out a new commit changes them, and a make with nothing to do never starts
# cargo. --target-dir keeps cargo's output under build/tools/ rather than in the
# submodule's work tree, where git would report it as untracked content. Every VU
# program depends on the binary, so a new oxide-vusl rebuilds them all.
OXIDE_VCL_SRCS = $(OXIDE_VCL_PATH)/Cargo.toml $(OXIDE_VCL_PATH)/Cargo.lock \
                 $(shell find $(OXIDE_VCL_PATH)/crates -path '*/tests' -prune -o -type f -print 2>/dev/null)

$(OXIDE_VUSL): $(wildcard $(OXIDE_VCL_SRCS))
	@test -f $(OXIDE_VCL_PATH)/Cargo.toml || \
		{ echo "$(OXIDE_VCL_PATH) is empty - run 'git submodule update --init'"; exit 1; }
	@cargo build --quiet --release --locked --bin oxide-vusl \
		--manifest-path $(OXIDE_VCL_PATH)/Cargo.toml --target-dir $(OXIDE_VCL_BUILD)
	@touch $@

# VU1 microprograms. oxide-vusl writes the listing and the header together, or
# neither when the program has an error; the .vsm is kept beside the object for
# reading. In oxide mode oxide-vcl rewrites or rejects an immediate that does not
# fit its field (its 'imm' fix) rather than leaving dvp-as to truncate it. An
# 'operand out of range' warning from dvp-as on a branch longer than 1023
# instructions is harmless: VU1 micro memory is exactly 2048 instructions and the
# PC wraps, so the low 11 bits dvp-as keeps reach the same instruction.
#
# One recipe makes both targets: a pattern rule with two targets is run once for
# the pair.
$(VU_DIR)/%.o $(VU_DIR)/%.vusl.h: $(VUSL_PATH)/%.vusl $(VUSL_INCS) $(OXIDE_VUSL)
	@mkdir -p $(dir $@)
	$(OXIDE_VUSL) $(OXIDE_VUSL_FLAGS) --header=$(VU_DIR)/$*.vusl.h -o$(VU_DIR)/$*.vsm $<
	dvp-as $(VU_DIR)/$*.vsm -o $(VU_DIR)/$*.o

# vu1.h includes the generated headers, so no C++ object can be built before them.
# Order-only: once an object exists, its -MMD dependency file names the headers it
# really includes, and that is what rebuilds it when one changes.
$(CXX_OBJS): | $(VU_HEADERS)

# IOP modules embedded via bin2c.
$(OUTPUT_DIR)/irx/%.o: $(IRX_PATH)/%.irx
	@mkdir -p $(dir $@)
	$(PS2SDK)/bin/bin2c $< $(basename $@).c $(notdir $(basename $@))_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(basename $@).c -o $@

# Host tools.
tools: $(TOOLS_BINS)

# Configured once. After that, `cmake --build` configures again by itself whenever
# CMakeLists.txt changes. HOST_CC/HOST_CXX only take effect here, so remove build/tools/
# after changing them. No "Built target" lines, so a build with nothing to do is silent.
$(HOST_TOOLS_CMAKE)/CMakeCache.txt:
	cmake -S $(HOST_TOOLS_PATH) -B $(HOST_TOOLS_CMAKE) -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=$(HOST_CC) -DCMAKE_CXX_COMPILER=$(HOST_CXX) \
		-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(abspath $(BUILD_DIR)/tools) \
		-DCMAKE_TARGET_MESSAGES=OFF

# CMake tracks the tools' sources and headers itself (musenc shares spu_adpcm.h with the
# game, pakzip shares pkz_format.h, and both see the miniz submodule), and does nothing
# when none of them changed. So this runs on every make, instead of copying that list
# here. MAKEFLAGS is cleared so the Makefiles CMake generates don't try to join this
# make's jobserver, which isn't passed to a non-recursive recipe; --parallel gives them
# jobs of their own.
host_tools: $(HOST_TOOLS_CMAKE)/CMakeCache.txt
	@MAKEFLAGS= cmake --build $(HOST_TOOLS_CMAKE) --parallel

$(TOOLS_CXX_BINS): host_tools ;

# The soundtrack for the CD audio module (ps2/audio/cd_audio.cpp): every trackNN.wav in
# MUSIC_DIR, any case, is encoded to a lowercase trackNN.adp beside it, skipping the ones
# already newer than both their .wav and the encoder. The .wav files are only the source;
# the game never reads them, so they needn't go onto the USB stick.
MUSIC_DIR ?= baseq2/music

music: $(BUILD_DIR)/tools/musenc
	@found=0; \
	for wav in $(MUSIC_DIR)/[Tt]rack*.wav; do \
		[ -e "$$wav" ] || continue; \
		found=1; \
		adp="$(MUSIC_DIR)/$$(basename "$$wav" .wav | tr 'A-Z' 'a-z').adp"; \
		if [ "$$adp" -nt "$$wav" ] && [ "$$adp" -nt $(BUILD_DIR)/tools/musenc ]; then continue; fi; \
		$(BUILD_DIR)/tools/musenc "$$wav" "$$adp" || exit 1; \
	done; \
	[ $$found = 1 ] || { echo "No trackNN.wav files in $(MUSIC_DIR)/"; exit 1; }

# The compressed game packs (ps2/fs/pkz_format.h): every pakN.pak in PAK_DIR is converted to
# the pakN.pkz beside it, skipping the ones already newer than both their .pak and the
# converter. The game loads a pkz instead of the pak with the same number, so the .pak needn't
# go onto the USB stick once its pkz exists.
PAK_DIR ?= baseq2

# Demos stay stored, so the game streams them through its read-ahead ring
# (ps2/fs/readahead_stream.h): a deflated one is read on the main thread, a 64 KB
# refill at a time, which froze a frame for 80 ms from the HDD.
PKZ_FLAGS ?= --store-ext .dm2

pkz: $(BUILD_DIR)/tools/pakzip
	@found=0; \
	for pak in $(PAK_DIR)/pak[0-9].pak; do \
		[ -e "$$pak" ] || continue; \
		found=1; \
		pkz="$${pak%.pak}.pkz"; \
		if [ "$$pkz" -nt "$$pak" ] && [ "$$pkz" -nt $(BUILD_DIR)/tools/pakzip ]; then continue; fi; \
		$(BUILD_DIR)/tools/pakzip $(PKZ_FLAGS) "$$pak" "$$pkz" || exit 1; \
	done; \
	[ $$found = 1 ] || { echo "No pakN.pak files in $(PAK_DIR)/"; exit 1; }

# Script tools are published into build/tools/ under the same extensionless names
# as the compiled ones, so everything in there is invoked the same way.
$(TOOLS_PY_BINS): $(BUILD_DIR)/tools/%: $(SCRIPTS_PATH)/%.py
	@mkdir -p $(dir $@)
	cp -f $< $@
	@chmod +x $@

# PCSX2 exposes the ELF's directory as host:, so the game data must be reachable
# as build/<config>/baseq2. A symlink back to the repo's baseq2/ does it.
$(OUTPUT_DIR)/baseq2:
	@mkdir -p $(dir $@)
	ln -sfn $(abspath baseq2) $@

# A disc build boots its image as a disc, the way PCSX2 boots any game. PCSX2 then serves host:
# from the image's directory, where a disc build writes quake2.log, and the baseq2 link takes its
# frame and I/O logs and screenshots to the repo's baseq2/, as on the other builds.
ifeq ($(ISO_BUILD),1)
run: iso_image $(OUTPUT_DIR)/baseq2
	$(PCSX2) -batch -- $(abspath $(ISO_IMAGE))
else
run: all $(OUTPUT_DIR)/baseq2
	$(PCSX2) -batch -elf $(abspath $(GAME_ELF))
endif

# Regenerate compile_commands.json so the editor's IntelliSense uses the exact
# per-file compile flags. Run after adding/removing source files. The debug build's
# lines, then the disc debug build's for the sources only it has (the generator keeps
# the first line it sees for each source).
compiledb:
	@{ $(MAKE) -Bnk BUILD=debug; $(MAKE) -Bnk BUILD=iso_debug; } | python3 $(SCRIPTS_PATH)/gen_compile_commands.py

# Every config, not just the selected one.
clean:
	rm -rf $(BUILD_DIR)/debug $(BUILD_DIR)/release $(BUILD_DIR)/iso_debug $(BUILD_DIR)/iso_release \
		$(BUILD_DIR)/vu $(BUILD_DIR)/vu_sampler $(BUILD_DIR)/tools

clean_vu:
	rm -rf $(BUILD_DIR)/vu $(BUILD_DIR)/vu_sampler

-include $(DEPS)

# Pull in the PS2SDK toolchain definitions and the C++ link rule. These provide
# EE_CC/EE_CXX/EE_STRIP, the -D_EE/-G0 defaults (the optimization and debug-info
# ones are set per-config above), EE_LDFLAGS (linkfile, max-page-size) and the
# `$(EE_BIN): $(EE_OBJS)` link recipe (links with g++).
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
