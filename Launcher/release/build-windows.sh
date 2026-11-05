#!/usr/bin/env bash
# Cross-compiles the LAN game for Windows inside Docker (Dockerfile.windows: llvm-mingw, plus
# Wine for the C++/WinRT headers). Shares the translation with build-linux.sh; the native build
# goes to WORKSPACE/native-build-windows.
#
#   Launcher/release/build-windows.sh [--workspace DIR] [--output-dir DIR]
#
# Needs the same workspace inputs as build-linux.sh (Assets/, the portable Linux toolchain for
# cmake/ninja, a .NET 8 SDK).
#
# Smoke test under Wine (no GPU in the container, so Dawn falls back to its Null backend; the run
# log lands in the package's UserData/Logs):
#   docker run --rm -e WINEPREFIX=/tmp/wine -e WINEDLLOVERRIDES="mscoree,mshtml=" -v PKG:/t \
#     mkwlan-build-windows:22.04 xvfb-run -a bash -c \
#     'cd /t/windows && timeout 100 wine WiiCompiled.exe --headless --race-cpu'
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd)
output_dir=""
dotnet_root=${DOTNET_ROOT:-$HOME/.dotnet}
image=mkwlan-build-windows:22.04

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workspace) workspace=$(cd "$2" && pwd); shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --dotnet-root) dotnet_root=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done
output_dir=${output_dir:-$workspace/out/release-windows}
tools=/work/Launcher/artifacts/portable-tools/toolchain-x86_64/bin
mingw=/opt/llvm-mingw/bin

[[ -f "$workspace/Assets/main.dol" ]] || { echo "missing $workspace/Assets/main.dol" >&2; exit 1; }
[[ -x "$workspace/Launcher/artifacts/portable-tools/toolchain-x86_64/bin/cmake" ]] ||
    { echo "missing portable toolchain: run Launcher/prepare-portable-tools.sh --arch x86_64" >&2; exit 1; }
[[ -x "$dotnet_root/dotnet" ]] || { echo "missing .NET 8 SDK at $dotnet_root (set DOTNET_ROOT)" >&2; exit 1; }

docker build -t "$image" -f "$script_dir/Dockerfile.windows" "$script_dir"
mkdir -p "$output_dir" "$workspace/.release-home"
# The .NET CLI refuses to start for a user id without a passwd entry (named-mutex setup fails).
printf 'root:x:0:0:root:/root:/bin/bash\nbuilder:x:%s:%s:builder:/tmp/home:/bin/bash\n' \
    "$(id -u)" "$(id -g)" > "$workspace/.release-home/passwd"

docker run --rm --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    -v "$workspace/.release-home/passwd:/etc/passwd:ro" \
    -e HOME=/tmp/home -e NUGET_PACKAGES=/work/.release-home/nuget \
    -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 -e DOTNET_ROOT=/opt/dotnet \
    -e MKW_ASM_TARGET=windows \
    -e PATH="/opt/dotnet:$mingw:$tools:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -v "$workspace:/work" -v "$dotnet_root:/opt/dotnet:ro" -v "$(cd "$output_dir" && pwd):/output" \
    -w /work "$image" \
    bash -c 'mkdir -p "$HOME" &&
             Launcher/release/mingw-case-headers.sh /work /work/.release-home/mingw-case-include &&
             exec "$@"' build \
    bash Launcher/local-build.sh --lan --output-dir /output --build-dir /work/native-build-windows \
        --cc "$mingw/x86_64-w64-mingw32-clang" --cxx "$mingw/x86_64-w64-mingw32-clang++" \
        --cmake "$tools/cmake" --ninja "$tools/ninja" --dotnet /opt/dotnet/dotnet \
        --x86-cpu-profile v3 \
        --cmake-arg -DCMAKE_TOOLCHAIN_FILE=/work/Launcher/release/mingw-w64-x86_64.cmake \
        --cmake-arg -DAURORA_DAWN_PROVIDER=package --cmake-arg -DAURORA_SDL3_PROVIDER=vendor \
        --cmake-arg -DMKW_CPPWINRT_INCLUDE_DIR=/opt/cppwinrt

echo "Windows build in $output_dir"
