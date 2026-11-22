#!/usr/bin/env bash
#
# Erases a USB drive on macOS and formats it FAT32 with 32 KB clusters, for Open PS2 Loader (OPL)
# and for the game's own USB boot. OPL wants a disc image in one piece on the drive, so a fresh
# file system before each copy is the simple way to get one. Disk Utility can't choose the
# cluster size, and macOS picks 8 KB for a 16 GB stick; the PS2's FAT driver reads one cluster
# per USB command (README.md, "From USB"), so newfs_msdos makes it 32 KB instead.
#
#   format_usb_drive.sh [-n NAME] [-i IMAGE] [-y] diskN
#
# Steps: diskutil eraseDisk (an MBR partition table, one FAT32 partition), then newfs_msdos on that
# partition (sudo), then OPL's CD/ and DVD/ folders. With -i, the image is copied into CD/ (700 MiB
# or less) or DVD/. It refuses anything but a whole, external USB disk, and asks you to type the
# disk's name before erasing it, unless -y.

set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [-n NAME] [-i IMAGE] [-y] diskN

  diskN     The USB drive, as 'diskutil list' names it (disk4 or /dev/disk4).
  -n NAME   Volume name, up to 11 letters, digits and _ (default PS2GAMES).
  -i IMAGE  Copy this disc image (.iso) into CD/ or DVD/ afterwards.
  -y        Don't ask before erasing.

Everything on the drive is erased.
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

[ "$(uname -s)" = "Darwin" ] || die "this script uses macOS's diskutil and newfs_msdos"

name="PS2GAMES"
image=""
assume_yes=0
while getopts ":n:i:yh" opt; do
    case "$opt" in
        n) name="$OPTARG" ;;
        i) image="$OPTARG" ;;
        y) assume_yes=1 ;;
        h) usage; exit 0 ;;
        :) usage >&2; die "-$OPTARG needs a value" ;;
        *) usage >&2; die "unknown option -$OPTARG" ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { usage >&2; die "give exactly one disk"; }

disk="${1#/dev/}"
[[ "$disk" =~ ^disk[0-9]+$ ]] || die "'$1' is not a whole disk (diskN, not diskNsM)"
[[ "$name" =~ ^[A-Za-z0-9_]{1,11}$ ]] || die "volume name '$name' must be 1-11 letters, digits or _"
name="$(echo "$name" | tr '[:lower:]' '[:upper:]')"

# The image is checked before anything is erased.
if [ -n "$image" ]; then
    [ -f "$image" ] || die "no image at '$image'"
    image_bytes=$(stat -f %z "$image")
    [ "$image_bytes" -le 4294967295 ] || die "'$image' is over 4 GiB, more than a FAT32 file can hold"
fi

# ------------------------------------------------------------------------------------------------
# Is it the right disk?
# ------------------------------------------------------------------------------------------------

info_plist="$(diskutil info -plist "/dev/$disk" 2>/dev/null)" || die "no disk /dev/$disk ('diskutil list' shows the disks)"

info() {
    plutil -extract "$1" raw -o - - <<<"$info_plist" 2>/dev/null || echo ""
}

[ "$(info WholeDisk)" = "true" ] || die "/dev/$disk is not a whole disk"
[ "$(info Internal)" = "false" ] || die "/dev/$disk is an internal disk; refusing"
[ "$(info BusProtocol)" = "USB" ] || die "/dev/$disk is on '$(info BusProtocol)', not USB; refusing"
[ "$(info VirtualOrPhysical)" != "Virtual" ] || die "/dev/$disk is a virtual disk (a disk image?); refusing"

boot_disk="$(diskutil info -plist / | plutil -extract ParentWholeDisk raw -o - - 2>/dev/null || echo "")"
[ "$disk" != "$boot_disk" ] || die "/dev/$disk holds the running system; refusing"

size_bytes="$(info TotalSize)"
media="$(info MediaName)"

echo "About to ERASE /dev/$disk: ${media:-unnamed}, $((size_bytes / 1000000000)) GB, USB."
diskutil list "/dev/$disk"
echo

if [ "$assume_yes" -ne 1 ]; then
    read -r -p "Type '$disk' to erase it, anything else to stop: " answer
    [ "$answer" = "$disk" ] || die "stopped; nothing was changed"
fi

# ------------------------------------------------------------------------------------------------
# Erase and format
# ------------------------------------------------------------------------------------------------

echo "Partitioning /dev/$disk (MBR, one FAT32 partition)..."
diskutil eraseDisk FAT32 "$name" MBR "/dev/$disk"

partition="${disk}s1"
diskutil unmountDisk "/dev/$disk"

# 64 sectors of 512 bytes: 32 KB clusters. The raw device (rdisk) writes much faster.
echo "Formatting /dev/$partition with 32 KB clusters (needs sudo)..."
sudo newfs_msdos -F 32 -c 64 -v "$name" "/dev/r$partition"

diskutil mountDisk "/dev/$disk"

mount_point="$(diskutil info -plist "/dev/$partition" | plutil -extract MountPoint raw -o - - 2>/dev/null || echo "")"
[ -n "$mount_point" ] && [ -d "$mount_point" ] || die "formatted, but /dev/$partition didn't mount"

mkdir -p "$mount_point/CD" "$mount_point/DVD"
echo "Formatted: $mount_point (FAT32, 32 KB clusters), with OPL's CD/ and DVD/ folders."

# ------------------------------------------------------------------------------------------------
# The image
# ------------------------------------------------------------------------------------------------

if [ -n "$image" ]; then
    # OPL takes an image over a CD's 700 MiB from DVD/ only.
    folder="CD"
    [ "$image_bytes" -le $((700 * 1024 * 1024)) ] || folder="DVD"
    destination="$mount_point/$folder/$(basename "$image")"

    echo "Copying $(basename "$image") ($((image_bytes / 1048576)) MiB) into $folder/..."
    cp "$image" "$destination"
    sync
    [ "$(stat -f %z "$destination")" = "$image_bytes" ] || die "the copy on the drive has the wrong size"
    cmp -s "$image" "$destination" || die "the copy on the drive differs from '$image'"
    echo "Copied and verified: $destination"
fi

echo "Eject with: diskutil eject /dev/$disk"
