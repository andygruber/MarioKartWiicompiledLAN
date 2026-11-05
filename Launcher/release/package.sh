#!/usr/bin/env bash
# Assembles a ready-to-play portable package (game files included) and zips it.
#
#   Launcher/release/package.sh --game-data DIR --linux-build DIR --linux-player FILE \
#       [--windows-build DIR --windows-player FILE] [--version V] [--output-dir DIR]
#
# --game-data      the extracted disc's DATA directory (contains sys/ and files/)
# --*-build        output of build-linux.sh / build-windows.sh
# --*-player       LanPlayerSetup published for linux-x64 / win-x64
#
# Layout: game/DATA, linux/, windows/, UserData/Config.toml (dvd_root = ../game/DATA) and
# portable.txt, which makes the runtime keep settings and saves in UserData.
# The package contains Nintendo's game data: keep it to people who own the game.
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd)
game_data="" linux_build="" linux_player="" windows_build="" windows_player=""
version=$(date +%Y.%m.%d)
output_dir=$workspace/out/packages

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-data) game_data=$2; shift 2 ;;
        --linux-build) linux_build=$2; shift 2 ;;
        --linux-player) linux_player=$2; shift 2 ;;
        --windows-build) windows_build=$2; shift 2 ;;
        --windows-player) windows_player=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done
[[ -f "$game_data/sys/main.dol" ]] || { echo "--game-data must be an extracted DATA directory" >&2; exit 1; }
[[ -x "$linux_build/WiiCompiled" && -f "$linux_player" ]] || { echo "--linux-build/--linux-player missing" >&2; exit 1; }
with_windows=0
if [[ -n "$windows_build" ]]; then
    [[ -f "$windows_build/WiiCompiled.exe" && -f "$windows_player" ]] || { echo "--windows-build/--windows-player missing" >&2; exit 1; }
    with_windows=1
fi

variant=linux; (( with_windows )) && variant=windows-linux
name=MarioKartWiiLAN
stage=$output_dir/stage-$variant
zip_path=$output_dir/$name-$version-$variant.zip
rm -rf "$stage" && mkdir -p "$stage/$name/UserData" "$stage/$name/game"
root=$stage/$name

# The disc is copied (reflinked where the filesystem allows) so the archive holds real files.
cp -a --reflink=auto "$game_data" "$root/game/DATA"

copy_product() {  # copy_product <build dir> <player> <destination>
    local build=$1 player=$2 destination=$3
    mkdir -p "$destination"
    find "$build" -maxdepth 1 -type f ! -name local-build.json -exec cp -a {} "$destination/" \;
    [[ -d "$build/wii_bootstrap" ]] && cp -a "$build/wii_bootstrap" "$destination/"
    [[ -d "$build/config" ]] && cp -a "$build/config" "$destination/"
    cp -a "$player" "$destination/"
    cp "$workspace/LICENSE" "$destination/LICENSE.txt"
    cp "$workspace/THIRD-PARTY-NOTICES.md" "$destination/"
}
copy_product "$linux_build" "$linux_player" "$root/linux"
(( with_windows )) && copy_product "$windows_build" "$windows_player" "$root/windows"

printf 'This file makes the game portable: settings, saves and logs are kept in UserData next to it.\n' \
    > "$root/portable.txt"
printf '# Settings for this portable copy (see README.txt).\n[paths]\n# The extracted game, relative to this file.\ndvd_root = "../game/DATA"\n' \
    > "$root/UserData/Config.toml"

# README: keep the @IF_LINUX@ / @IF_WINDOWS@ blocks this variant contains.
awk -v windows="$with_windows" -v version="$version" '
    BEGIN { keep = -1 }
    /^@IF_LINUX@$/ { keep = 1; next }
    /^@IF_WINDOWS@$/ { keep = windows; next }
    /^@END@$/ { keep = -1; next }
    keep == 0 { next }
    { gsub(/@VERSION@/, version); print }
' "$script_dir/README-package.txt" > "$root/README.txt"
if (( with_windows )); then
    # Windows users open README.txt in Notepad, which is happy with either line ending; CRLF
    # keeps older editors happy too.
    sed -i 's/$/\r/' "$root/README.txt"
fi

rm -f "$zip_path"
(cd "$stage" && zip -q -r "$zip_path" "$name")
rm -rf "$stage"
echo "$zip_path"
