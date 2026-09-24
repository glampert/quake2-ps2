# PS2 VCL/VU Assembly syntax highlighting for VSCode

Syntax highlighting for the PS2 VU microprograms in this repository:
`.vcl` sources (openvcl/vclpp dialect, including the `#macro`/`#vuprog`
preprocessor directives), the `.i` files they `#include`, and the assembled
`.vsm` output.

The includes are matched by path, as `**/vu1progs/*.i`, rather than by
extension: `.i` is also preprocessed C, which VSCode's built-in C support
claims, and this extension is installed for every workspace. A path pattern
outranks an extension match, so it wins here and nowhere else. An include
kept anywhere other than a `vu1progs/` directory needs adding to
`filenamePatterns` in `package.json`.

## Installing

Run `Developer: Install Extension from Location...` from the command palette
and point it at this folder. VSCode registers it where it lies, so edits here
are what it loads.

A symlink into `~/.vscode/extensions` also works, but its name has to be the
extension's id - `<publisher>.<name>-<version>`, lowercased, which is
`ps2_quake.ps2-vcl-0.1.0` - and VSCode has to have registered it. One named
after anything else loads under an id VSCode's registry does not know, and it
marks the extension removed at every start (`Marked extension as removed` in
the shared process log).

## After editing it

VSCode caches extension manifests. The first reload after changing
`package.json` still runs the cached copy, notices it is stale a moment later
and drops it (`Invalidating Cache` in the window's log) - so it takes a second
`Developer: Reload Window` before a change there shows.
