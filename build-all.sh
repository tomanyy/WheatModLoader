#!/usr/bin/env sh
set -eu

repoRoot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! command -v cmake >/dev/null 2>&1; then
    printf '%s\n' "Error: CMake is required to build Wheat." >&2
    exit 1
fi

if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    printf '%s\n' "Error: install the x86_64 MinGW-w64 C++ compiler to cross-build Windows from Linux." >&2
    exit 1
fi

printf '%s\n' "Building Linux runtime..."
sdkImage=${WHEAT_SNIPER_SDK_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}
if command -v podman >/dev/null 2>&1; then
    containerRuntime=podman
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    containerRuntime=docker
else
    printf '%s\n' "Error: no usable container runtime was found." >&2
    printf '%s\n' "Install Podman, or start the Docker daemon (for example: sudo systemctl start docker), then rerun this script." >&2
    exit 1
fi

"$containerRuntime" run --rm --platform linux/amd64 \
    --user 0:0 \
    --volume "$repoRoot:/work" \
    --workdir /work \
    "$sdkImage" \
    sh -lc 'rm -rf -- build-linux-sniper build-win'

"$containerRuntime" run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --volume "$repoRoot:/work" \
    --workdir /work \
    "$sdkImage" \
    sh -lc 'cmake -S . -B build-linux-sniper -DwheatVendorLua=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux-sniper --config Release'

printf '%s\n' "Building Windows runtime..."
cmake -S "$repoRoot" -B "$repoRoot/build-win" \
    -DCMAKE_TOOLCHAIN_FILE="$repoRoot/cmake/mingw-w64-x86_64.cmake" \
    -DwheatVendorLua=ON
cmake --build "$repoRoot/build-win" --config Release

printf '%s\n' "Builds complete:"
printf '  Linux:   %s\n' "$repoRoot/build-linux-sniper/wheatLinux.dll"
printf '  Vulkan:  %s\n' "$repoRoot/build-linux-sniper/libwheatVulkanLayer.so"
printf '  Windows: %s\n' "$repoRoot/build-win/wheatWindows.dll"
