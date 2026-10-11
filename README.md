# Wheat Mod Loader
Wheat Mod Loader (WML) is an experimental Lua 5.4 mod loader for Counter-Strike 2. On Windows, it loads through a `winmm.dll` proxy, hosts Lua mods in separate restricted sandboxes, and dispatches `mods_loaded` and `frame` events.

> [!WARNING]
> WML only starts when CS2's command line contains `-insecure` and does not contain `-secure`. It does not add `-insecure` for you. Use the loader only in an environment where you accept the restrictions and consequences of running CS2 in insecure mode; secure matchmaking is unavailable.
>
> WML intentionally refuses to run if `-secure` is present. Do not use or contribute code that removes or bypasses this check.

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-blue.svg)](LICENSE)
[![Latest release](https://img.shields.io/github/v/release/tomanyy/WheatModLoader?label=latest%20release)](https://github.com/tomanyy/WheatModLoader/releases/latest)
[![Issues](https://img.shields.io/github/issues/tomanyy/WheatModLoader)](https://github.com/tomanyy/WheatModLoader/issues)

## Features

- Lua 5.4.8 runtime, vendored by default for Windows builds.
- A separate sandbox for each mod, with memory, instruction, pattern-work, and execution-time limits.
- Per-mod log rate limiting and deterministic mod loading order.
- `mods_loaded` and `frame` events. The frame event provides elapsed seconds and a frame number.
- A small built-in `wheat` Lua API: `api_version`, `mod_name`, and monotonic `now()`.
- A Windows `winmm.dll` proxy that forwards exports to the system DLL and only activates in `cs2.exe` when the insecure launch gate passes.
- A Wine launch helper that sets the `winmm` DLL override when `-insecure` is supplied.

## Install a release

Download the latest build from [GitHub Releases](https://github.com/tomanyy/WheatModLoader/releases) and follow any release-specific packaging notes. Launch CS2 with `-insecure` enabled in its launch options. WML creates a `wheat` directory beside the game executable, with a `mods` folder and `loader.log` inside it.

To install a Lua mod, create a folder directly inside `wheat/mods` and put its entry point at `main.lua`:

```text
Counter-Strike Global Offensive/game/bin/win64/
└── wheat/
    └── mods/
        └── example/
            └── main.lua
```

Mod folder names may contain letters, digits, `_`, and `-`, up to 64 characters. WML loads immediate subfolders in alphabetical order; folders without `main.lua` are skipped. Mods are loaded at startup.

## Build from source

### Requirements

- CMake 3.20 or newer.
- A C and C++ compiler with C++20 support.
- For a Windows loader DLL: 64-bit MinGW-w64. The proxy build is enabled when CMake detects MinGW.
- For a non-vendored Lua build: Lua 5.4 development files.
- Internet access during a vendored Lua build, so CMake can fetch Lua 5.4.8.

### Windows DLL (MinGW-w64)

From an x86_64 MinGW-w64 shell, configure and build:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DwheatVendorLua=ON
cmake --build build --config Release
```

The proxy DLL is produced as `build/winmm.dll`. Place it in the game's `game/bin/win64` directory (alongside the game executable's runtime files), then launch with `-insecure`.

### Linux / sandbox libraries

On Linux, CMake builds the reusable Lua sandbox and mod-host static libraries; the Windows proxy target is only built for MinGW. With Lua 5.4 development files installed, configure and build using:

```sh
cmake -S . -B build -DwheatVendorLua=OFF
cmake --build build
```

Set `-DwheatVendorLua=ON` to have CMake fetch and build Lua 5.4.8 instead. For cross-compiling to Windows, see [`cmake/mingw-w64-x86_64.cmake`](cmake/mingw-w64-x86_64.cmake); set `CMAKE_TOOLCHAIN_FILE` to that file when configuring.

## Wine launch helper

`launch/wheat-launch.sh` wraps a command, enables the Wine `winmm` native/builtin override only when the arguments include `-insecure` and not `-secure`, and records the decision in `$HOME/wheatLaunch.log`.

```sh
./launch/wheat-launch.sh wine /path/to/steam.exe
```

Pass the CS2 launch arguments to the wrapped command through your usual Steam/Wine launch setup. The helper checks the arguments it receives; it does not add `-insecure` itself.

## Logs and troubleshooting

- WML writes its runtime log to `wheat/loader.log` beside the game executable. The log is capped at 8 MiB.
- If WML does not start, confirm the process is `cs2.exe`, `-insecure` appears as a standalone launch argument, and `-secure` is absent.
- If a mod does not load, check for `wheat/mods/<mod-name>/main.lua` and review `loader.log` for its error.
- The Wine helper's launch decision is written to `$HOME/wheatLaunch.log`.

## Project status

WML is experimental. The loader currently exposes only the small `wheat` API listed above; game interaction APIs are not yet documented or included in this repository's built-in host API.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidance. Changes must preserve the `-insecure` launch gate and must not attempt to bypass it.

## License
<<<<<<< HEAD
GPL-3.0 license
=======

Wheat Mod Loader is distributed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
>>>>>>> 31083d9 (Update README.md)
