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
- A Windows PE `winmm.dll` proxy and a Linux ELF preload library (also named `winmm.dll`); each activates only in CS2 when the insecure launch gate passes.
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

On Linux, install CMake, a C/C++ compiler, and MinGW-w64, then build both platform runtimes with one command:

```sh
./build-all.sh
```

This removes the previous `build-linux-sniper` and `build-win` output directories, then writes fresh `build-linux-sniper/wheatLinux.dll` and `build-win/wheatWindows.dll` artifacts. The Linux runtime is built in Valve's Sniper SDK container to keep it compatible with CS2's Steam Linux Runtime; Docker with its daemon running or Podman is required. The Windows artifact is cross-compiled; the script needs `x86_64-w64-mingw32-g++` available in `PATH`.

### Requirements

- CMake 3.20 or newer.
- Vulkan development headers and loader package for Linux builds.
- A C and C++ compiler with C++20 support.
- Docker with its daemon running, or Podman, to build the Linux runtime in Valve's Sniper SDK container.
- For a Windows loader DLL: 64-bit MinGW-w64. The proxy build is enabled when CMake detects MinGW.
- For a non-vendored Lua build: Lua 5.4 development files.
- Internet access during a vendored Lua build, so CMake can fetch Lua 5.4.8.

### Windows DLL (MinGW-w64)

From an x86_64 MinGW-w64 shell, configure and build:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DwheatVendorLua=ON
cmake --build build --config Release
```

The Windows proxy is produced as `build/wheatWindows.dll`. The launcher renames it to `winmm.dll` when installing it into a Wheat installation. Launch with `-insecure`.

### Linux preload library

`./build-all.sh` builds the preload library inside Valve's Sniper SDK container using vendored Lua so it remains compatible with CS2's Steam runtime. The Linux artifact is an ELF shared object at `build-linux-sniper/wheatLinux.dll`; the launcher installs it as `winmm.dll`. The build also creates a separate `libwheatVulkanLayer.so`. Linux callbacks use a 10 Hz timer by default; a new installation can opt in to the separate Vulkan layer for render-thread callbacks and the experimental ImGui overlay. The layer is not included in existing release downloads yet. Linux console reads are experimental and enable through the installation API Allow List after runtime calibration. See [`docs/modApi.md`](docs/modApi.md) for Linux ImGui usage. For cross-compiling to Windows, see [`cmake/mingw-w64-x86_64.cmake`](cmake/mingw-w64-x86_64.cmake).

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
