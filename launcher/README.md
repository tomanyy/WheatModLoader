# Wheat Launcher

This PyQt6 launcher uses Qt Fusion widgets and shared light and dark palettes so its controls stay consistent across Linux and Windows. Its installation list adapts Fleasion's Replacer-tab layout for Wheat Mod Loader. The layout adaptation is licensed under GPL-3.0; see the About dialog, repository LICENSE, and [Fleasion's GUI source](https://github.com/fleasion/Fleasion/tree/main/src/fleasion/gui).

## Install

```sh
python3 -m pip install -r requirements-launcher.txt
python3 launcher/wheatLauncher.py
```

SteamCMD is optional. When an executable is detected or configured, the launcher shows the latest public Windows or Linux CS2 install option, SteamCMD path, repair/update action, and SteamCMD input controls. SteamCMD selects platform depots for the requested platform: Linux uses shared 64-bit depot 2347770 together with Linux 64-bit depot 2347773. SteamCMD uses anonymous login for the public CS2 app. The launcher does not use DepotDownloader or steamctl.

## New Installation

Click **New Installation** and choose:

- **CS2 Platform**: choose Windows (run through Proton on Linux) or native Linux. The platform is shown in the installations list.
- **Wheat Version**: Windows installations use a PE `winmm.dll`. Native Linux installations use the ELF preload library built by CMake and named `winmm.dll`; Linux Steam does not load a Windows PE DLL.
- **CS2 Version / Depot**: choose the latest public build with SteamCMD or the matching platform's current vanilla installation. SteamCMD selects platform-compatible depots for the same public build.
- **Full Installation / Link files from Vanilla**: keep a full copy or symlink the files from the detected vanilla game. Link mode derives the platform and version from Vanilla, locks both selectors to **Determined by Vanilla**, and skips the CS2 SteamCMD download. Only the selected Wheat loader is copied or downloaded; game files are symlinked into the new installation. Each installation has its own Wheat workspace.
- **Install Directory**: choose a new folder. Each installation is stored independently and appears in the installations list.

Right-click an installation to add or update its separate Steam Library entry, open its logs in a separate window, manage its mods, check mod updates, edit its API allow list, run installation or Steam integration health checks, repair/update a SteamCMD installation, open its folder, rename it, export a JSON install profile, or delete it. The **News** tab loads the latest published release notes from GitHub. **New from Profile** imports a JSON install profile. Profiles store the platform, source preference, full/link setting, loader choice, API allow list, and enabled/disabled mod names. When the original installation is still available, its listed mod folders are copied into the new install. The profile does not bundle mod files, CS2 depots, or the loader binary.

Mod update checks read an optional `repository` or `update_url` field from each mod's `manifest.lua`. GitHub repositories use their latest release tag. A custom `update_url` must be an HTTPS JSON endpoint returning `version` and optionally `release_url`. The launcher reports versions and links to releases; it does not download or replace mod files. API Allow List controls `console.get`, `console.default`, `console.allowed`, `console.available`, and `data`; changes apply after restarting CS2. To add the Steam entry, fully exit Steam first, then restart it after the launcher confirms it was added. Windows entries use Proton and the Wine DLL override; Linux entries use `LD_PRELOAD` to load the ELF `winmm.dll` into native CS2. Both start through Steam with `-insecure`.

If SteamCMD cannot be found while starting an install, the launcher keeps that installation record marked Failed instead of removing it. Installations created from vanilla remain available.

The launcher never places Wheat's `winmm.dll` in the vanilla Steam game folder. Mods and API permissions are scoped to each installation. Activity displays SteamCMD output and accepts interactive input. A download is accepted only when SteamCMD reports app 730 fully installed or already up to date; a zero exit code alone is not treated as success. For launch, Steam must be open and signed in; the Steam entry routes CS2 through Steam so Steamworks can initialize. Linux frame callbacks use a 10 Hz timer because the current renderer hook is Windows D3D11-only. The Linux console API is unavailable, and there is not yet an ImGui API.

Build the Linux preload runtime from the repository root with `cmake -S . -B build-linux -DwheatVendorLua=ON` and `cmake --build build-linux`. This produces an ELF shared library at `build-linux/wheatLinux.dll`; the launcher installs it as `winmm.dll`.

At install time, the launcher records the linked Steam build ID when available. If a non-separated installation uses vanilla files and that build changes, create a new installation from the updated game before launching.

CS2's official guidance says insecure mode cannot join VAC-secure servers. [Steam Support: CS2 Trusted Mode](https://help.steampowered.com/en/faqs/view/09A0-4879-4353-EF95)
