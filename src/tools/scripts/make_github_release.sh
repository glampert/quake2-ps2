#!/usr/bin/env bash
#
# Builds the GitHub release packages from a clean tree: `make clean`, then
# `make` and `make release`, then github_rel_<version>/ at the repo root with
# one folder and one zip per config:
#
#   q2ps2_<config>_<version>/
#     quake2.elf
#     quake2_unstripped.elf
#     baseq2/
#       config.cfg      the committed default (HEAD), not the working copy
#       music/          empty, for the player's game data
#       players/
#       save/
#       video/
#
# The zips are what gets attached to the GitHub release.

set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [-f] [version]

  version   Release version. Defaults to VERSION in src/common/q_common.h.
  -f        Replace an existing github_rel_<version>/ folder.
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

force=0
version=""
for arg in "$@"; do
    case "$arg" in
        -f|--force) force=1 ;;
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

# Not fatal: a release cut from uncommitted changes is sometimes intended.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "warning: uncommitted changes in tracked files; the ELFs are built from the working tree." >&2
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

for config in debug release; do
    for elf in quake2.elf quake2_unstripped.elf; do
        [ -f "build/$config/$elf" ] || die "build/$config/$elf is missing after the build"
    done
done

# Only now that both builds succeeded, so a failed run leaves the old packages alone.
rm -rf "$out_dir"
mkdir -p "$out_dir"

for config in debug release; do
    name="q2ps2_${config}_${version}"
    pkg="$out_dir/$name"

    echo "==> packaging $name"
    mkdir -p "$pkg/baseq2/music" "$pkg/baseq2/players" "$pkg/baseq2/save" "$pkg/baseq2/video"
    cp "build/$config/quake2.elf" "build/$config/quake2_unstripped.elf" "$pkg/"
    git show HEAD:baseq2/config.cfg > "$pkg/baseq2/config.cfg"

    (cd "$out_dir" && zip -r -9 -q "$name.zip" "$name" -x "*.DS_Store" -x "__MACOSX/*")
done

echo
echo "Release $version packaged in $out_dir/:"
ls -l "$out_dir"/*.zip
