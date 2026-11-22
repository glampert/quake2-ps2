#!/usr/bin/env bash
#
# Builds the GitHub release packages from a clean tree: `make clean`, then
# `make`, `make release` and the disc builds' ELFs (`make BUILD=iso_debug`,
# `make BUILD=iso_release`), then github_rel_<version>/ at the repo root with
# one folder and one zip per config:
#
#   q2ps2_<config>_<version>/
#     quake2.elf
#     quake2_unstripped.elf  only with -u
#     quake2_disc.elf        the config's disc build (build/iso_<config>/)
#     quake2_disc_unstripped.elf  only with -u
#     make_iso.py            makes a disc image from quake2_disc.elf and the
#                            player's baseq2/
#     LICENSE                the GPL, which the binaries must ship with
#     RELEASE_NOTES.txt      misc/RELEASE_NOTES.txt, @VERSION@ and @COMMIT@ filled in
#     baseq2/
#       config.cfg      the committed default (HEAD), not the working copy
#       music/          empty, for the player's game data
#       players/
#       video/
#
# For a disc image the player runs
# `python3 make_iso.py --elf quake2_disc.elf --data baseq2 -o quake2.iso` in the
# package's folder. The zips are what gets attached to the GitHub release.

set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [-f] [-u] [version]

  version   Release version. Defaults to VERSION in src/common/q_common.h.
  -f        Replace an existing github_rel_<version>/ folder.
  -u        Also package quake2_unstripped.elf (the symbols, for addr2line).
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

force=0
unstripped=0
version=""
for arg in "$@"; do
    case "$arg" in
        -f|--force) force=1 ;;
        -u|--unstripped) unstripped=1 ;;
        -h|--help)  usage; exit 0 ;;
        -*)         usage >&2; die "unknown option '$arg'" ;;
        *)
            [ -z "$version" ] || die "more than one version given"
            version="$arg"
            ;;
    esac
done

if [ -z "$version" ]; then
    version="$(awk '$1 == "#define" && $2 == "VERSION" { print $3 }' src/common/q_common.h)"
    [ -n "$version" ] || die "no VERSION in src/common/q_common.h; pass the version explicitly"
fi
[[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || die "version '$version' has characters unfit for a file name"

out_dir="github_rel_$version"
if [ -e "$out_dir" ] && [ "$force" -eq 0 ]; then
    die "$out_dir already exists; remove it or pass -f to replace it"
fi

release_notes="misc/RELEASE_NOTES.txt"
make_iso="src/tools/scripts/make_iso.py"
for file in LICENSE "$release_notes" "$make_iso"; do
    [ -f "$file" ] || die "$file is missing"
done

commit="$(git rev-parse HEAD)"

# Not fatal: a release cut from uncommitted changes is sometimes intended.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "warning: uncommitted changes in tracked files; the ELFs are built from the working tree," \
         "not from commit $commit that the release notes point to." >&2
fi
if ! git diff --quiet HEAD -- baseq2/config.cfg; then
    echo "warning: baseq2/config.cfg differs from HEAD; the packages ship the committed one." >&2
fi

echo "==> make clean"
make clean
echo "==> make (debug)"
make || die "debug build failed"
echo "==> make release"
make release || die "release build failed"
# The ELFs alone: `make iso`/`make iso_dbg` would also make an image from the repo's baseq2/.
echo "==> make BUILD=iso_debug (disc build, debug)"
make BUILD=iso_debug || die "debug disc build failed"
echo "==> make BUILD=iso_release (disc build, release)"
make BUILD=iso_release || die "release disc build failed"

elfs=(quake2.elf)
[ "$unstripped" -eq 0 ] || elfs+=(quake2_unstripped.elf)

for config in debug release iso_debug iso_release; do
    for elf in "${elfs[@]}"; do
        [ -f "build/$config/$elf" ] || die "build/$config/$elf is missing after the build"
    done
done

# Only now that every build succeeded, so a failed run leaves the old packages alone.
rm -rf "$out_dir"
mkdir -p "$out_dir"

for config in debug release; do
    name="q2ps2_${config}_${version}"
    pkg="$out_dir/$name"

    echo "==> packaging $name"
    mkdir -p "$pkg/baseq2/music" "$pkg/baseq2/players" "$pkg/baseq2/video"
    for elf in "${elfs[@]}"; do
        cp "build/$config/$elf" "$pkg/"
        cp "build/iso_$config/$elf" "$pkg/${elf/quake2/quake2_disc}"
    done
    cp "$make_iso" "$pkg/"
    cp LICENSE "$pkg/"
    sed -e "s/@VERSION@/$version/g" -e "s/@COMMIT@/$commit/g" "$release_notes" > "$pkg/RELEASE_NOTES.txt"
    git show HEAD:baseq2/config.cfg > "$pkg/baseq2/config.cfg"

    (cd "$out_dir" && zip -r -9 -q "$name.zip" "$name" -x "*.DS_Store" -x "__MACOSX/*")
done

echo
echo "Release $version packaged in $out_dir/:"
ls -l "$out_dir"/*.zip
