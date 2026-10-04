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
#    make tools       -> build host tools (imgdump, unpak, bspinfo, musenc) into build/tools/
#    make music       -> encode baseq2/music/trackNN.wav into the trackNN.adp files the game streams
#    make clean       -> remove build artifacts (both configs)
#    make clean_vu    -> remove only assembled VU microprograms
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
#  Picked by the `release` goal, or by BUILD= on the command line:
#
#      make          -> debug   -O2, DWARF info, asserts and debug-only code IN
#      make release  -> release -O3, no DWARF, asserts and debug-only code OUT
#
#  Each config owns its object tree under build/<config>/, so alternating
#  between the two neither mixes objects built with different flags nor forces
#  a full rebuild.

ifneq ($(filter release,$(MAKECMDGOALS)),)
    BUILD := release
else
    BUILD ?= debug
endif

ifeq ($(filter $(BUILD),debug release),)
    $(error BUILD must be 'debug' or 'release', got '$(BUILD)')
endif

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
	ps2/audio/music_stream.cpp        \
	ps2/audio/cd_audio.cpp            \
	ps2/save/save_api.cpp             \
	ps2/save/working_set.cpp          \
	ps2/save/slot_archive.cpp         \
	ps2/save/save_device.cpp          \
	ps2/save/memcard.cpp              \
	ps2/save/mc_icon.cpp              \
	ps2/renderer/gs.cpp               \
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
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/tests/perf_run.cpp            \
	ps2/tests/save_test.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp             \
	ps2/builtin/palette.cpp           \
	ps2/builtin/conchars.cpp          \
	ps2/builtin/conback.cpp           \
	ps2/builtin/backtile.cpp

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
	ps2/audio/audsrv_device.cpp       \
	ps2/input/keyboard.cpp            \
	ps2/input/pad.cpp                 \
	ps2/input/rumble.cpp              \
	ps2/renderer/vid.cpp              \
	ps2/save/save_api.cpp             \
	ps2/save/slot_archive.cpp         \
	ps2/save/save_device.cpp          \
	ps2/save/memcard.cpp              \
	ps2/save/mc_icon.cpp              \
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/tests/perf_run.cpp            \
	ps2/tests/save_test.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/pipeline_dump.cpp       \
	ps2/debug/exception_handler.cpp   \
	ps2/debug/profile.cpp

SIZE_OPT_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(SIZE_OPT_CXX_SRC:.cpp=.o))

C_OBJS   = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(C_SRC:.c=.o))
CXX_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(CXX_SRC:.cpp=.o))

# VU microprograms: vclpp -> openvcl -> dvp-as
# Each .vcl assembles into .vudata with <name>_CodeStart/_CodeEnd link symbols
# (see PS2_DECLARE_VU_MICROPROGRAM in ps2/renderer/vu1.h).
# None of the EE compiler flags reach this toolchain, so the output is identical
# in both configs: build it once into build/vu/ and share it.
VCL_PATH  = $(SRC_DIR)/ps2/renderer/vu1progs
VCL_FILES = textured_triangles.vcl particles.vcl
VU_OBJS   = $(addprefix $(BUILD_DIR)/vu/, $(VCL_FILES:.vcl=.o))

# Shared macro/constant includes. The programs include these by bare name, which
# vclpp finds next to the including file (and through -I $(VCL_PATH) as well);
# they are prerequisites here because the pattern rule below would not otherwise
# see them change.
VCL_INCS  = $(wildcard $(VCL_PATH)/*.i)

# The openvcl/dvp-as output checks every VU build runs (see the rule below). They
# come in as a git submodule (https://github.com/glampert/vu-checker), so other
# PS2 projects can share them.
VU_CHECK = $(SRC_DIR)/tools/vu-checker/check_vu_code.py

# vclpp is not part of the ps2dev distribution: it comes in as a git submodule
# (https://github.com/glampert/vclpp) and is built by its own Makefile, so every
# VU build runs the pinned version rather than whatever is on PATH - an older
# vclpp silently mangles macro bodies that have comments in them. vclpp has a
# submodule of its own, parse-utils (https://github.com/glampert/parse-utils),
# whose lexer it is built with.
VCLPP_PATH        = $(SRC_DIR)/tools/vclpp
VCLPP_PARSE_UTILS = $(VCLPP_PATH)/external/parse-utils
VCLPP             = $(BUILD_DIR)/tools/vclpp

# miniz (https://github.com/richgel999/miniz), the deflate codec the save games are
# compressed with (ps2/save/working_set.cpp). A git submodule like vclpp; only the raw
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
HOST_TOOLS_PATH = $(SRC_DIR)/tools/host
SCRIPTS_PATH    = $(SRC_DIR)/tools/scripts
TOOLS_CXX_BINS  = $(addprefix $(BUILD_DIR)/tools/, imgdump unpak bspinfo musenc)
TOOLS_PY_BINS   = $(addprefix $(BUILD_DIR)/tools/, symbolize)
TOOLS_BINS      = $(TOOLS_CXX_BINS) $(TOOLS_PY_BINS)
HOST_CXX       ?= c++
HOST_CXXFLAGS  ?= -std=gnu++20 -O2 -Wall

# IOP/IRX modules embedded into the ELF: the BDM USB mass-storage stack, booted
# by ps2/system/iop_boot.cpp when the game data isn't on host: (real hardware),
# the USB keyboard driver started on demand by ps2/input/keyboard.cpp, and the
# sound driver pair (libsd under audsrv) started by ps2/audio/audsrv_device.cpp.
IRX_PATH  = $(PS2SDK)/iop/irx
IRX_FILES = iomanX.irx fileXio.irx \
            bdm.irx bdmfs_fatfs.irx usbd.irx usbmass_bd.irx \
            ps2kbd.irx libsd.irx audsrv.irx

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
# PS2_QUAKE_DEBUG, PS2_QUAKE_ASSERTS and PS2_QUAKE_PROFILE are always defined to 0 or 1,
# never undefined.
ifeq ($(BUILD),release)
    EE_OPTFLAGS     = -O3
    EE_DBGINFOFLAGS =
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DPS2_QUAKE_PROFILE=0 -DNDEBUG
else
    EE_OPTFLAGS     = -O2
    EE_DBGINFOFLAGS = -gdwarf-2 -gz
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=1 -DPS2_QUAKE_ASSERTS=1 -DPS2_QUAKE_PROFILE=1
endif

COMMON_DEFS = -DGAME_HARD_LINKED -DPS2_QUAKE $(CONFIG_DEFS)

EE_INCS += -I$(SRC_DIR)

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
	-Wno-switch -MMD -MP

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
	-fno-strict-aliasing $(COMMON_DEFS) \
	$(EE_CXX_WARNFLAGS) $(EE_CXX_SYSINCS) \
	-MMD -MP

# -leedebug supplies the level 1 exception vector that src/ps2/debug/exception_handler.cpp
# hangs its post-mortem off. It contributes nothing to a release build - the whole
# handler is behind PS2_QUAKE_DEBUG - but the linker only pulls in what is referenced,
# so leaving it on the line for both configs costs nothing.
EE_LIBS += -lkernel -ldraw -lgraph -lpacket2 -ldma -lpad -lkbd -laudsrv -lpatches -lfileXio -lmc -leedebug

# ----------------------------------------------------------------------------
#  Rules
# ----------------------------------------------------------------------------

.PHONY: all release run tools music clean clean_vu compiledb

all: $(GAME_ELF) tools

# `release` only selects the config (see BUILD above); the build itself is `all`.
release: all

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

# The vclpp submodule, through its own Makefile. That one recompiles on every
# run, so the up-to-date check is made here instead: it runs again only when the
# submodule's sources or Makefile change, as checking out a new commit does.
# BIN_TARGET puts the binary under build/tools/ rather than in the submodule's
# work tree, where git would report it as untracked content. Every VU program
# depends on the binary, so a new vclpp rebuilds them all.
$(VCLPP): $(wildcard $(VCLPP_PATH)/Makefile $(VCLPP_PATH)/src/*.cpp $(VCLPP_PATH)/src/*.hpp $(VCLPP_PARSE_UTILS)/*.hpp)
	@test -f $(VCLPP_PATH)/Makefile && test -f $(VCLPP_PARSE_UTILS)/lexer.hpp || \
		{ echo "$(VCLPP_PATH) is incomplete - run 'git submodule update --init --recursive'"; exit 1; }
	@mkdir -p $(dir $@)
	@$(MAKE) --no-print-directory -C $(VCLPP_PATH) CXX=$(HOST_CXX) BIN_TARGET=$(abspath $@)

# The vu-checker submodule. The script needs no build step, so this rule only
# runs when the file is missing - an uninitialized submodule. Every VU program
# depends on it, so a new vu-checker checks them all again.
$(VU_CHECK):
	@echo "$(patsubst %/,%,$(dir $@)) is incomplete - run 'git submodule update --init --recursive'"; exit 1

# VU1 microprograms.
# The checks in check_vu_code.py are not optional, and every one of them exists
# because the toolchain fails silently. openvcl allocates VI registers by
# liveness and gets it wrong on control flow past a single counted loop - it
# hands a live register to a temporary, with no diagnostic, and the microprogram
# then runs away or reads garbage. It pads a clip flag or Q read for latency only
# within a basic block. dvp-as truncates an immediate that does not fit its field
# and says nothing, so a constant one larger than the instruction can hold
# becomes a different constant. All of them reach the screen rather than the
# build log.
# The register allocation check needs a second openvcl run with -c for the source
# names, and the branch check reads the object, so the checks run once dvp-as is
# done and a failure deletes the object - otherwise the next make would take the
# bad object as up to date. The 'operand out of range' warnings from dvp-as are
# expected; see the branch check for why they are harmless.
$(BUILD_DIR)/vu/%.o: $(VCL_PATH)/%.vcl $(VCL_INCS) $(VCLPP) $(VU_CHECK)
	@mkdir -p $(dir $@)
	$(VCLPP) -I $(VCL_PATH) -Wundef -Werror -j $< $(basename $@).pp.vcl
	openvcl -o $(basename $@).vsm $(basename $@).pp.vcl
	@openvcl -c -o $(basename $@).c.vsm $(basename $@).pp.vcl
	dvp-as $(basename $@).vsm -o $@
	@python3 $(VU_CHECK) $@ || { rm -f $@; exit 1; }

# IOP modules embedded via bin2c.
$(OUTPUT_DIR)/irx/%.o: $(IRX_PATH)/%.irx
	@mkdir -p $(dir $@)
	bin2c $< $(basename $@).c $(notdir $(basename $@))_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(basename $@).c -o $@

# Host tools: each is a single self-contained .cpp compiled straight to a binary.
tools: $(TOOLS_BINS)

$(TOOLS_CXX_BINS): $(BUILD_DIR)/tools/%: $(HOST_TOOLS_PATH)/%.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(HOST_CXXFLAGS) $< -o $@

# musenc shares the ADPCM decoder with the game, so it plays back exactly what it measured.
$(BUILD_DIR)/tools/musenc: $(SRC_DIR)/ps2/audio/spu_adpcm.h

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

run: all $(OUTPUT_DIR)/baseq2
	$(PCSX2) -batch -elf $(abspath $(GAME_ELF))

# Regenerate compile_commands.json so the editor's IntelliSense uses the exact
# per-file compile flags. Run after adding/removing source files.
compiledb:
	@$(MAKE) -Bnk | python3 $(SCRIPTS_PATH)/gen_compile_commands.py

# Both configs, not just the selected one.
clean:
	rm -rf $(BUILD_DIR)/debug $(BUILD_DIR)/release $(BUILD_DIR)/vu $(BUILD_DIR)/tools

clean_vu:
	rm -rf $(BUILD_DIR)/vu

-include $(DEPS)

# Pull in the PS2SDK toolchain definitions and the C++ link rule. These provide
# EE_CC/EE_CXX/EE_STRIP, the -D_EE/-G0 defaults (the optimization and debug-info
# ones are set per-config above), EE_LDFLAGS (linkfile, max-page-size) and the
# `$(EE_BIN): $(EE_OBJS)` link recipe (links with g++).
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
