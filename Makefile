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
#    make tools       -> build host tools (imgdump, unpak) into build/tools/
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
	ps2/math/math.cpp                 \
	ps2/math/vec_mat.cpp              \
	ps2/net/net.cpp                   \
	ps2/input/input.cpp               \
	ps2/input/keyboard.cpp            \
	ps2/input/pad.cpp                 \
	ps2/audio/snd.cpp                 \
	ps2/audio/audsrv_device.cpp       \
	ps2/audio/mix_ring.cpp            \
	ps2/renderer/gs.cpp               \
	ps2/renderer/vram.cpp             \
	ps2/renderer/texture.cpp          \
	ps2/renderer/image_load.cpp       \
	ps2/renderer/model.cpp            \
	ps2/renderer/model_load.cpp       \
	ps2/renderer/lightmap.cpp         \
	ps2/renderer/scrap_atlas.cpp      \
	ps2/renderer/cinematic.cpp        \
	ps2/renderer/render_view.cpp      \
	ps2/renderer/render_md2.cpp       \
	ps2/renderer/render_sky.cpp       \
	ps2/renderer/vid.cpp              \
	ps2/renderer/ref.cpp              \
	ps2/renderer/vu1.cpp              \
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/exception_handler.cpp

# Doug Lea's allocator + a small amount of embedded data kept as plain C:
PS2_C_SRC = \
	ps2/system/dlmalloc/dlmalloc.c    \
	ps2/builtin/palette.c             \
	ps2/builtin/conchars.c            \
	ps2/builtin/conback.c             \
	ps2/builtin/backtile.c

# Stock Quake II engine / game / server - untouched C, statically linked.
# The null/* stub stands in for CD audio: Quake II streams its music off CDDA tracks
# and this port has no CDVD path at all - game data comes from host: or mass:.
# Sound output itself is implemented, see ps2/audio/.
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
	null/cd_null.c                                                             \
	server/sv_ccmds.c  server/sv_ents.c   server/sv_game.c   server/sv_init.c  \
	server/sv_main.c   server/sv_send.c   server/sv_user.c   server/sv_world.c

C_SRC   = $(PS2_C_SRC) $(ENGINE_C_SRC)
CXX_SRC = $(PS2_CXX_SRC)

# Backend sources that run at load time or not at all in a normal frame: asset
# parsing, IOP module boot, device setup, the debug screen printer. None of them
# are on the per-frame path, so they are built for size instead of speed - worth
# ~9 KB of .text, which is RAM the levels get to use instead.
#
# The hot renderer (render_view/render_md2/render_sky/gs/vu1/vram/lightmap/ref),
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
	ps2/renderer/vid.cpp              \
	ps2/tests/draw_cube.cpp           \
	ps2/tests/cinematics.cpp          \
	ps2/tests/map_cycle.cpp           \
	ps2/debug/scr_print.cpp           \
	ps2/debug/stack_trace.cpp         \
	ps2/debug/exception_handler.cpp

SIZE_OPT_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(SIZE_OPT_CXX_SRC:.cpp=.o))

C_OBJS   = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(C_SRC:.c=.o))
CXX_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(CXX_SRC:.cpp=.o))

# VU microprograms: vclpp -> openvcl -> dvp-as
# Each .vcl assembles into .vudata with <name>_CodeStart/_CodeEnd link symbols
# (see PS2_DECLARE_VU_MICROPROGRAM in ps2/renderer/vu1.h).
# None of the EE compiler flags reach this toolchain, so the output is identical
# in both configs: build it once into build/vu/ and share it.
VCL_PATH  = $(SRC_DIR)/ps2/renderer/vu1progs
VCL_FILES = textured_triangles.vcl lerped_triangles.vcl
VU_OBJS   = $(addprefix $(BUILD_DIR)/vu/, $(VCL_FILES:.vcl=.o))

# Standalone command line tools under src/tools, built with the HOST compiler
# (not the EE toolchain) since they run on the development machine. Being host
# binaries they are config-independent, so they live outside build/<config>/.
TOOLS_PATH    = $(SRC_DIR)/tools
TOOLS_CC_BINS = $(addprefix $(BUILD_DIR)/tools/, imgdump unpak bspinfo)
TOOLS_PY_BINS = $(addprefix $(BUILD_DIR)/tools/, symbolize)
TOOLS_BINS    = $(TOOLS_CC_BINS) $(TOOLS_PY_BINS)
HOST_CC      ?= cc
HOST_CFLAGS  ?= -O2 -Wall

# IOP/IRX modules embedded into the ELF: the BDM USB mass-storage stack, booted
# by ps2/system/iop_boot.cpp when the game data isn't on host: (real hardware),
# the USB keyboard driver started on demand by ps2/input/keyboard.cpp, and the
# sound driver pair (libsd under audsrv) started by ps2/audio/audsrv_device.cpp.
IRX_PATH  = $(PS2SDK)/iop/irx
IRX_FILES = iomanX.irx fileXio.irx \
            bdm.irx bdmfs_fatfs.irx usbd.irx usbmass_bd.irx \
            ps2kbd.irx libsd.irx audsrv.irx

IRX_OBJS  = $(addprefix $(OUTPUT_DIR)/irx/, $(IRX_FILES:.irx=.o))

EE_OBJS = $(C_OBJS) $(CXX_OBJS) $(VU_OBJS) $(IRX_OBJS)
DEPS    = $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)

# ----------------------------------------------------------------------------
#  Compiler / linker flags (appended to the SDK defaults from Makefile.eeglobal)
# ----------------------------------------------------------------------------

# Per-config flags. EE_OPTFLAGS and EE_DBGINFOFLAGS are the SDK's own knobs:
# Makefile.eeglobal_cpp defaults them with ?= (to -O2 and -gdwarf-2 -gz), so
# whatever is set here wins - including setting the debug info to empty.
#
# PS2_QUAKE_DEBUG and PS2_QUAKE_ASSERTS are always defined to 0 or 1, never undefined.
ifeq ($(BUILD),release)
    EE_OPTFLAGS     = -O3
    EE_DBGINFOFLAGS =
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=0 -DPS2_QUAKE_ASSERTS=0 -DNDEBUG
else
    EE_OPTFLAGS     = -O2
    EE_DBGINFOFLAGS = -gdwarf-2 -gz
    CONFIG_DEFS     = -DPS2_QUAKE_DEBUG=1 -DPS2_QUAKE_ASSERTS=1
endif

COMMON_DEFS = -DGAME_HARD_LINKED -DPS2_QUAKE $(CONFIG_DEFS)

EE_INCS += -I$(SRC_DIR)

# id's C89 engine sources under GCC 15: force C89, restore -fcommon, and
# downgrade the constructs GCC 14+ promoted to hard errors so the untouched
# engine still compiles.
#
# TODO: Look into addressing and fixing some of these warnings, some are likely real bugs
# that need patching (aggressive-loop-optimizations, maybe-uninitialized, dangling-else, etc).
#
EE_CFLAGS += -std=gnu89 -fcommon -fno-strict-aliasing $(COMMON_DEFS) \
	-Wno-implicit-function-declaration -Wno-implicit-int -Wno-maybe-uninitialized \
	-Wno-int-conversion -Wno-int-to-pointer-cast -Wno-pointer-sign \
	-Wno-pointer-to-int-cast -Wno-missing-braces -Wno-unused-variable \
	-Wno-stringop-truncation -Wno-unused-but-set-variable -Wno-parentheses \
	-Wno-aggressive-loop-optimizations -Wno-switch -Wno-dangling-else \
	-Wno-unused-function -Wno-address -Wno-restrict -Wno-return-type \
	-Wno-stringop-overflow \
	-MMD -MP

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
EE_CXX_SYSINCS = -isystem $(PS2SDK)/ee/include -isystem $(PS2SDK)/common/include

# Lean, embedded C++ for the new backend.
EE_CXXFLAGS += -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-strict-aliasing $(COMMON_DEFS) \
	$(EE_CXX_WARNFLAGS) $(EE_CXX_SYSINCS) \
	-MMD -MP

# -leedebug supplies the level 1 exception vector that src/ps2/debug/exception_handler.cpp
# hangs its post-mortem off. It contributes nothing to a release build - the whole
# handler is behind PS2_QUAKE_DEBUG - but the linker only pulls in what is referenced,
# so leaving it on the line for both configs costs nothing.
EE_LIBS += -lkernel -ldraw -lgraph -lpacket2 -ldma -lpad -lkbd -laudsrv -lpatches -lfileXio -leedebug

# ----------------------------------------------------------------------------
#  Rules
# ----------------------------------------------------------------------------

.PHONY: all release run tools clean clean_vu compiledb

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

# VU1 microprograms.
# TODO: vclpp has to be made a project dependency and added to the repo sync (https://github.com/glampert/vclpp).
$(BUILD_DIR)/vu/%.o: $(VCL_PATH)/%.vcl
	@mkdir -p $(dir $@)
	vclpp $< $(basename $@).pp.vcl -j
	openvcl -o $(basename $@).vsm $(basename $@).pp.vcl
	dvp-as $(basename $@).vsm -o $@

# IOP modules embedded via bin2c.
$(OUTPUT_DIR)/irx/%.o: $(IRX_PATH)/%.irx
	@mkdir -p $(dir $@)
	bin2c $< $(basename $@).c $(notdir $(basename $@))_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(basename $@).c -o $@

# Host tools: each is a single self-contained .c compiled straight to a binary.
tools: $(TOOLS_BINS)

$(TOOLS_CC_BINS): $(BUILD_DIR)/tools/%: $(TOOLS_PATH)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@

# Script tools are published into build/tools/ under the same extensionless names
# as the compiled ones, so everything in there is invoked the same way.
$(TOOLS_PY_BINS): $(BUILD_DIR)/tools/%: $(TOOLS_PATH)/%.py
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
	@$(MAKE) -Bnk | python3 scripts/gen_compile_commands.py

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
