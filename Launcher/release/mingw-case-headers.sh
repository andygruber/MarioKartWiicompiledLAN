#!/usr/bin/env bash
# Windows headers are case-insensitive; mingw-w64 ships them in lower case (windows.h) while the
# sources include e.g. <Windows.h>. Creates symlinks for every such include the sources use, in an
# extra include directory that mingw-w64-x86_64.cmake adds with -idirafter.
#   mingw-case-headers.sh <workspace> <output include dir>
set -euo pipefail
workspace=$1 out=$2
headers=/opt/llvm-mingw/generic-w64-mingw32/include
rm -rf "$out" && mkdir -p "$out"
grep -rhoE '#[[:space:]]*include[[:space:]]*[<"][A-Za-z0-9_/.]+\.[hH]' \
        "$workspace/runtime" "$workspace/aurora-main/lib" "$workspace/aurora-main/include" \
        "$workspace/aurora-main/extern" "$workspace/generated" 2>/dev/null |
    sed -E 's/.*[<"]//' | grep '[A-Z]' | sort -u |
    while read -r include; do
        [[ -e "$headers/$include" ]] && continue
        match=$(find "$headers" -ipath "$headers/$include" -print -quit)
        [[ -n "$match" ]] || continue
        mkdir -p "$out/$(dirname "$include")"
        ln -sf "$match" "$out/$include"
        echo "case alias: $include -> ${match#$headers/}"
    done
