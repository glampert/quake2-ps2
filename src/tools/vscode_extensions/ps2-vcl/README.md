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

Symlink this folder into your local VSCode extensions directory and
reload the editor:

```bash
ln -s "$(pwd)/src/tools/vscode_extensions/ps2-vcl" ~/.vscode/extensions/<user_name>.ps2-vcl-0.1.0
```

Then run `Developer: Reload Window` from the command palette. If the
language doesn't switch, run `Developer: Install Extension from Location...`
and point it at this folder instead. Restart VSCode.
