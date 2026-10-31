# audsrv (the game's copy)

ps2sdk's audsrv IOP module, from `iop/sound/audsrv` at ps2sdk `329e2bb` (the SDK CI builds
with), licensed under the LGPL v2 like the original (see each file's header). The game embeds
this build in place of the SDK's `audsrv.irx`. The EE side is still the SDK's `libaudsrv`, so
the RPC interface must stay the same.

## The change

One: the RPC thread's priority comes from a module argument, `rpc_prio=N`, instead of the
literal 40 that `rpc_server.c` passed to `create_thread`. Without the argument it is 40, as
before. Every changed line is marked `[PS2_QUAKE]`. The greeting the module prints at start
also gives the priority, so the IOP's output shows which build is running.

`ps2/system/iop_boot.cpp` starts it with `rpc_prio=39`. At 40 it shared the priority of
fileXio's RPC thread, and the IOP has no time slicing, so `audsrv_available` and
`audsrv_play_audio` waited out any file read already in progress, which on the HDD was up to
65 ms of audsrv's ~106 ms ring (console, 2026-10-31). At 39, the play thread's priority, a
request preempts the read when it arrives. The console HDD capture of 2026-11-01 put the
slowest free-space call at 0.77 ms. `.claude/rules/audio.md` has the measurements.

## Building and updating

The top-level Makefile builds it into `build/iop/audsrv/` with the SDK's rules for out-of-tree
IOP modules (`$(PS2SDK)/samples/Makefile.iopglobal`). Without the change those rules give a
module byte-identical to the SDK's own `audsrv.irx`. They track no header dependencies: after
editing a header, `rm -rf build/iop`.

To take a newer ps2sdk's audsrv, copy its `src/` and `include/` over these, apply the
`[PS2_QUAKE]` changes again, and update the commit above. Without the changes, the build should
match that SDK's `audsrv.irx` byte for byte.
