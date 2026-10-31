# audsrv.irx, the game's copy of ps2sdk's audsrv (README.md). Built by the top-level Makefile,
# which passes IOP_OBJS_DIR and IOP_BIN to keep the outputs under build/; on its own,
# `make -C src/ps2/iop/audsrv` builds into obj/ and audsrv.irx here. The rules are the SDK's for
# out-of-tree IOP modules (samples/Makefile.iopglobal); with the same sources they give a module
# byte-identical to the SDK's own build of it.

IOP_BIN     ?= audsrv.irx
IOP_OBJS    = audsrv.o upsamplers.o hw.o rpc_server.o rpc_client.o common.o cdrom.o imports.o exports.o adpcm.o
IOP_SRC_DIR = src/
IOP_INC_DIR = include/
IOP_INCS   += -Isrc

all: $(IOP_BIN)

clean:
	rm -rf $(IOP_OBJS_DIR) $(IOP_BIN) $(IOP_BIN:.irx=.notiopmod.elf) $(IOP_BIN:.irx=.notiopmod.stripped.elf)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
