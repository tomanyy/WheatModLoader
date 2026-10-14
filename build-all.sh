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
cmake -S "$repoRoot" -B "$repoRoot/build-linux" -DwheatVendorLua=ON
cmake --build "$repoRoot/build-linux" --config Release

printf '%s\n' "Building Windows runtime..."
cmake -S "$repoRoot" -B "$repoRoot/build-win" \
    -DCMAKE_TOOLCHAIN_FILE="$repoRoot/cmake/mingw-w64-x86_64.cmake" \
    -DwheatVendorLua=ON
cmake --build "$repoRoot/build-win" --config Release

printf '%s\n' "Builds complete:"
printf '  Linux:   %s\n' "$repoRoot/build-linux/wheatLinux.dll"
printf '  Windows: %s\n' "$repoRoot/build-win/wheatWindows.dll"
