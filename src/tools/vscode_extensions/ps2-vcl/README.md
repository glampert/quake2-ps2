# PS2 VCL/VU Assembly syntax highlighting for VSCode

Syntax highlighting for the PS2 VU microprograms in this repository:
`.vcl` sources (openvcl/vclpp dialect, including the `#macro`/`#vuprog`
preprocessor directives) and the assembled `.vsm` output.

## Installing

Symlink this folder into your local VSCode extensions directory and
reload the editor:

```bash
ln -s "$(pwd)/src/tools/vscode_extensions/ps2-vcl" ~/.vscode/extensions/<user_name>.ps2-vcl-0.1.0
```

Then run `Developer: Reload Window` from the command palette. If the
language doesn't switch, run `Developer: Install Extension from Location...`
and point it at this folder instead. Restart VSCode.
