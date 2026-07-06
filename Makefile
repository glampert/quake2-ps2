# ============================================================================
#  Quake II PS2 - build system
# ----------------------------------------------------------------------------
#  Produces build/quake2.elf for the PS2 EE, using the modern ps2dev toolchain
#  (mips64r5900el-ps2-elf-*). Built on top of the PS2SDK sample makefiles.
#
#    make            -> build build/quake2.elf   (VSCode: Shift+Cmd+B)
#    make run        -> build + launch in PCSX2  (VSCode: F5)
#    make clean      -> remove build artifacts
#    make clean_vu   -> remove only assembled VU microprograms
#
#  Header dependencies are tracked automatically (-MMD), so editing a header
#  rebuilds just the affected objects; no more `make clean` after header edits.
# ============================================================================

# Toolchain / tool locations (override from the environment if needed):
PS2DEV ?= /Users/guilherme/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
PCSX2  ?= /Applications/PCSX2.app/Contents/MacOS/PCSX2

SRC_DIR    = src
OUTPUT_DIR = build

EE_BIN = $(OUTPUT_DIR)/quake2.elf

# ----------------------------------------------------------------------------
#  Source files
# ----------------------------------------------------------------------------

# New PS2 backend, modern C++ (clean-slate rewrite of src/ps2):
PS2_CXX_SRC = \
	ps2/system/main.cpp     \
	ps2/system/sys.cpp      \
	ps2/system/heap.cpp     \
	ps2/math/math.cpp       \
	ps2/net/net.cpp         \
	ps2/renderer/gs.cpp     \
	ps2/renderer/vid.cpp    \
	ps2/renderer/ref.cpp    \
	ps2/debug/scr_print.cpp

# Doug Lea's allocator + a small amount of embedded data kept as plain C:
PS2_C_SRC = \
	ps2/system/dlmalloc/dlmalloc.c \
	ps2/builtin/palette.c

# Stock Quake II engine / game / server - untouched C, statically linked.
# The null/* stubs stand in for sound, input and CD audio until real PS2
# backends land.
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
	game/g_utils.c     game/g_weapon.c                                         \
	game/m_actor.c     game/m_berserk.c   game/m_boss2.c     game/m_boss3.c    \
	game/m_boss31.c    game/m_boss32.c    game/m_brain.c     game/m_chick.c    \
	game/m_flash.c     game/m_flipper.c   game/m_float.c     game/m_flyer.c    \
	game/m_gladiator.c game/m_gunner.c    game/m_hover.c     game/m_infantry.c \
	game/m_insane.c    game/m_medic.c     game/m_move.c      game/m_mutant.c   \
	game/m_parasite.c  game/m_soldier.c   game/m_supertank.c game/m_tank.c     \
	game/p_client.c    game/p_hud.c       game/p_trail.c     game/p_view.c     \
	game/p_weapon.c    game/q_shared.c                                         \
	null/cd_null.c     null/in_null.c     null/snddma_null.c                   \
	server/sv_ccmds.c  server/sv_ents.c   server/sv_game.c   server/sv_init.c  \
	server/sv_main.c   server/sv_send.c   server/sv_user.c   server/sv_world.c

C_SRC   = $(PS2_C_SRC) $(ENGINE_C_SRC)
CXX_SRC = $(PS2_CXX_SRC)

C_OBJS   = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(C_SRC:.c=.o))
CXX_OBJS = $(addprefix $(OUTPUT_DIR)/$(SRC_DIR)/, $(CXX_SRC:.cpp=.o))

# VU microprograms: vclpp -> openvcl -> dvp-as
# The 2D boot path references no VU program yet; the VU1 pipeline (and its
# microprograms, emitting UVs for texturing) is rewritten in Phase 2. The old
# color_triangles_clip_tris.vcl uses a clipw form the current openvcl rejects, so
# it is left out of the build for now. The assembly rule below stays ready.
VCL_PATH  = $(SRC_DIR)/ps2/vu1progs
VCL_FILES =
VU_OBJS   = $(addprefix $(OUTPUT_DIR)/vu/, $(VCL_FILES:.vcl=.o))

# IOP/IRX modules embedded into the ELF. None are needed for the current
# boot path; add e.g. usbd.irx here when USB mass-storage loading is wired up.
IRX_PATH  = $(PS2SDK)/iop/irx
IRX_FILES =
IRX_OBJS  = $(addprefix $(OUTPUT_DIR)/irx/, $(IRX_FILES:.irx=.o))

EE_OBJS = $(C_OBJS) $(CXX_OBJS) $(VU_OBJS) $(IRX_OBJS)
DEPS    = $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)

# ----------------------------------------------------------------------------
#  Compiler / linker flags (appended to the SDK defaults from Makefile.eeglobal)
# ----------------------------------------------------------------------------

COMMON_DEFS = -DGAME_HARD_LINKED -DPS2_QUAKE

EE_INCS += -I$(SRC_DIR)

# id's C89 engine sources under GCC 15: force C89, restore -fcommon, and
# downgrade the constructs GCC 14+ promoted to hard errors so the untouched
# engine still compiles.
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

EE_LIBS += -ldraw -lgraph -lmath3d -lpacket -ldma -lpad -lpatches -lkernel

# ----------------------------------------------------------------------------
#  Rules
# ----------------------------------------------------------------------------

.PHONY: all run clean clean_vu compiledb

all: $(EE_BIN)

# Out-of-tree object rules. These static-pattern rules take precedence over the
# generic %.o rules from Makefile.eeglobal so objects land under build/ mirroring
# the src/ tree. ($(EE_BIN) link rule is provided by Makefile.eeglobal_cpp.)
$(C_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

$(CXX_OBJS): $(OUTPUT_DIR)/$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(EE_CXX) $(EE_CXXFLAGS) $(EE_INCS) -c $< -o $@

# VU1 microprograms.
$(OUTPUT_DIR)/vu/%.o: $(VCL_PATH)/%.vcl
	@mkdir -p $(dir $@)
	vclpp $< $(basename $@).pp.vcl -j
	openvcl -o $(basename $@).vsm $(basename $@).pp.vcl
	dvp-as $(basename $@).vsm -o $@

# IOP modules embedded via bin2c.
$(OUTPUT_DIR)/irx/%.o: $(IRX_PATH)/%.irx
	@mkdir -p $(dir $@)
	bin2c $< $(basename $@).c $(notdir $(basename $@))_irx
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $(basename $@).c -o $@

run: all
	$(PCSX2) -batch -elf $(abspath $(EE_BIN))

# Regenerate compile_commands.json so the editor's IntelliSense uses the exact
# per-file compile flags. Run after adding/removing source files.
compiledb:
	@$(MAKE) -Bnk | python3 scripts/gen_compile_commands.py

clean:
	rm -rf $(OUTPUT_DIR)/src $(OUTPUT_DIR)/vu $(OUTPUT_DIR)/irx $(EE_BIN)

clean_vu:
	rm -rf $(OUTPUT_DIR)/vu

-include $(DEPS)

# Pull in the PS2SDK toolchain definitions and the C++ link rule. These provide
# EE_CC/EE_CXX/EE_STRIP, the -D_EE/-G0/-O2 defaults, EE_LDFLAGS (linkfile,
# max-page-size) and the `$(EE_BIN): $(EE_OBJS)` link recipe (links with g++).
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
