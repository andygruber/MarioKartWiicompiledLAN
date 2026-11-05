#!/usr/bin/env bash
# Builds the LAN game for Linux inside Docker (Ubuntu 22.04, glibc 2.35) so the binary runs on
# older distributions too. libstdc++/libgcc are linked statically; the only system libraries
# needed are glibc, zlib and libpng (plus SDL's runtime-loaded X11/Wayland/audio/Vulkan).
#
#   Launcher/release/build-linux.sh [--workspace DIR] [--output-dir DIR] [--x86-cpu-profile v2|v3]
#
# --x86-cpu-profile  v3 (default): AVX2, Intel Haswell (2013) / AMD Excavator (2015) and newer.
#                    v2: SSE4.2, also runs on older CPUs (Intel Nehalem 2008, AMD Bulldozer 2011).
#                    v3 builds in WORKSPACE/native-build, v2 in native-build-v2.
#
# The workspace needs Assets/ (main.dol, StaticR.rel, DATA/) and
# Launcher/artifacts/portable-tools/toolchain-x86_64 (Launcher/prepare-portable-tools.sh).
# Use a separate checkout from your day-to-day build: the native build cache is per workspace.
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd)
output_dir=""
dotnet_root=${DOTNET_ROOT:-$HOME/.dotnet}
image=mkwlan-build-linux:22.04
cpu_profile=v3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workspace) workspace=$(cd "$2" && pwd); shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --dotnet-root) dotnet_root=$2; shift 2 ;;
        --x86-cpu-profile) cpu_profile=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done
[[ "$cpu_profile" == v2 || "$cpu_profile" == v3 ]] || { echo "--x86-cpu-profile must be v2 or v3" >&2; exit 1; }
output_dir=${output_dir:-$workspace/out/release-linux}
# One build directory per profile, so switching does not rebuild everything each time.
build_dir=/work/native-build
[[ "$cpu_profile" == v3 ]] || build_dir+=-$cpu_profile
toolchain=/work/Launcher/artifacts/portable-tools/toolchain-x86_64/bin

[[ -f "$workspace/Assets/main.dol" ]] || { echo "missing $workspace/Assets/main.dol" >&2; exit 1; }
[[ -x "$workspace/Launcher/artifacts/portable-tools/toolchain-x86_64/bin/clang" ]] ||
    { echo "missing portable toolchain: run Launcher/prepare-portable-tools.sh --arch x86_64" >&2; exit 1; }
[[ -x "$dotnet_root/dotnet" ]] || { echo "missing .NET 8 SDK at $dotnet_root (set DOTNET_ROOT)" >&2; exit 1; }

docker build -t "$image" -f "$script_dir/Dockerfile.linux" "$script_dir"
mkdir -p "$output_dir" "$workspace/.release-home"
# The .NET CLI refuses to start for a user id without a passwd entry (named-mutex setup fails).
printf 'root:x:0:0:root:/root:/bin/bash\nbuilder:x:%s:%s:builder:/tmp/home:/bin/bash\n' \
    "$(id -u)" "$(id -g)" > "$workspace/.release-home/passwd"

docker run --rm --security-opt label=disable \
    --user "$(id -u):$(id -g)" \
    -v "$workspace/.release-home/passwd:/etc/passwd:ro" \
    -e HOME=/tmp/home -e NUGET_PACKAGES=/work/.release-home/nuget \
    -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 \
    -e PATH="/opt/dotnet:$toolchain:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -e DOTNET_ROOT=/opt/dotnet \
    -v "$workspace:/work" -v "$dotnet_root:/opt/dotnet:ro" -v "$(cd "$output_dir" && pwd):/output" \
    -w /work "$image" \
    bash -c 'mkdir -p "$HOME" && exec "$@"' build \
    bash Launcher/local-build.sh --lan --output-dir /output --build-dir "$build_dir" \
        --cc "$toolchain/clang" --cxx "$toolchain/clang++" \
        --cmake "$toolchain/cmake" --ninja "$toolchain/ninja" --dotnet /opt/dotnet/dotnet \
        --x86-cpu-profile "$cpu_profile" \
        --cmake-arg "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" \
        --cmake-arg -DMKW_GLIBC_C23_COMPAT=ON

# The bundled zlib leaves an RPATH into the build tree; the system libz.so.1 is used instead.
docker run --rm --security-opt label=disable --user "$(id -u):$(id -g)" \
    -v "$(cd "$output_dir" && pwd):/output" "$image" patchelf --remove-rpath /output/WiiCompiled

echo "Linux build (x86-64-$cpu_profile) in $output_dir"
