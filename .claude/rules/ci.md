---
paths:
  - ".github/**"
---

# CI (GitHub Actions)

- **`ps2.yml`** runs `make`, then `make release`, on Linux inside the ps2dev project's
  `ghcr.io/ps2dev/ps2sdk` image, for every push or PR that changes more than the docs or
  editor settings. So it also builds vclpp, the VU programs (with their checks) and the host
  tools. It is a compile check and keeps no ELF: releases are packaged from local builds
  (`make_github_release.sh`), and the IOP modules an ELF embeds come from the SDK that built it,
  but audsrv, which `make` builds from `src/ps2/iop/audsrv` with the image's IOP compiler and the
  SDK's `samples/Makefile.iopglobal`.
- **`tools.yml`** builds the host tools alone, with GCC, Apple clang and MSVC, when they or the
  headers they share with the game change. It keeps the Windows `.exe` files as an artifact.
- The two jobs see different hosts. `tools.yml` gets whatever GCC `ubuntu-latest` ships, on
  glibc with `_FORTIFY_SOURCE` (see [backend-cpp.md](backend-cpp.md)). The label moves from
  Ubuntu 24.04 to 26.04 between 2026-10-19 and 11-19. `ps2.yml` gets the GCC and make of the
  Alpine release under the pinned image, on musl, where the Mac has make 3.81. Linux file
  names are case-sensitive and this Mac's APFS volume isn't, so an include or Makefile path in
  the wrong case builds locally and fails CI.
- The first `ps2.yml` run (2026-10-19, `0e1e997`) passed in 82 s: 14 s to pull the image,
  38 s for the debug build, 14 s for the release one. Host tools and vclpp built warning-free
  under the image's GCC 15.2.

## The ps2sdk image

- Alpine, with the EE, IOP and dvp toolchains (openvcl and masp included) and the PS2SDK
  under `/usr/local/ps2dev`. Its environment sets `PS2DEV`, `PS2SDK` and `PATH`, and the
  Makefile's `?=` defaults yield to them. It has nothing for the host side: the job adds
  `build-base cmake python3 git` with apk. `ghcr.io/ps2dev/ps2dev` is the same plus
  ps2sdk-ports, gsKit and ps2-packer (290 MB compressed, against 195), none of which the game
  links.
- **Pinned by digest** (the multi-arch index), not `:latest`, so a new upstream GCC or SDK
  can't turn CI red with nothing changed here. GHCR keeps superseded digests: builds from July
  and August 2026 still resolved in October.
- **`v2.0.0`, the only release tag since 2021, can't build this tree.** openvcl and masp
  joined ps2toolchain-dvp on 2026-05-18, two days after it. Every `latest` since has them.
- The pin, as of 2026-10-19: `latest` as built on 2026-10-13, which is ps2sdk `329e2bb`,
  GCC 15.2.0, binutils 2.45.1, newlib 4.6.0 (`8881dc4`, with `HAVE_BLKSIZE`), openvcl 0.4.0,
  on Alpine 3.24. The local install was rebuilt the same day from the same sources (see the
  update recipe in AGENTS.md).
- **Moving the pin:** read the new digest, check that it builds (below), then update the
  digest and the comment above it in `ps2.yml`. The registry answers without Docker or a
  GitHub login:
  ```sh
  TOK=$(curl -s "https://ghcr.io/token?scope=repository:ps2dev/ps2sdk:pull" |
        python3 -c 'import sys, json; print(json.load(sys.stdin)["token"])')
  curl -sI -H "Authorization: Bearer $TOK" -H "Accept: application/vnd.oci.image.index.v1+json" \
       https://ghcr.io/v2/ps2dev/ps2sdk/manifests/latest | grep -i docker-content-digest
  ```
  `manifests/<digest>` then lists the per-platform manifests. Each of those names its layers
  and a config blob (`blobs/<digest>`), whose `org.opencontainers.image.revision` label is the
  ps2sdk commit. `gh run list -R ps2dev/ps2sdk --workflow docker.yml` lists the image builds.
  Listing GHCR package versions through the GitHub API needs the `read:packages` scope, which
  the local token lacks.

## Checking a Linux build without Docker

This Mac has no Docker, but the image's target files (headers, libraries, IRX modules, the
sample makefiles) build fine with the local toolchain while its versions match the image's.

1. Download the amd64 layers through the registry, as above, and extract
   `usr/local/ps2dev/ps2sdk` from the larger one. Its `bin/bin2c` is a musl ELF, so copy the
   local one over it. Diff its `samples/Makefile.*` against the local ones before using it.
2. Make a case-sensitive volume: `hdiutil create -size 3g -fs 'Case-sensitive APFS' -type
   SPARSE -volname Q2CS <scratch>/q2cs`, then
   `hdiutil attach -nobrowse -mountpoint <scratch>/q2cs_mnt <scratch>/q2cs.sparseimage`.
3. Put the SDK on it, clone the repo onto it with `--recursive`, and run
   `make PS2SDK=<sdk>` and `make release PS2SDK=<sdk>`. Check the exit status. Finish with
   `hdiutil detach <scratch>/q2cs_mnt`.

On 2026-10-19 HEAD built this way in both configs, with no compiler warnings, against the
2026-10-13 SDK and against v2.0.0's. It can't check the Linux binaries themselves, musl or
make 4.4. For Alpine's GCC 15 warnings, the `-mabi=64` stand-in in backend-cpp.md found
nothing in the host tools or vclpp.

## Traps

- **JavaScript actions run inside an Alpine container only on x64 runners.** The runner ships
  `node20_alpine`/`node24_alpine` for linux-x64 alone, and on arm64 it fails with "JavaScript
  Actions in Alpine containers are only supported on x64 Linux runners".
- **Install git before `actions/checkout`.** Without git 2.18 or newer on `PATH`, it downloads
  a tarball through the REST API instead, with no submodules.
