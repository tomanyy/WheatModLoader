#!/usr/bin/env python3
import argparse, os, re, shutil, sys

signatures = (b"wheat loader started", b"frame hook installed")
ourDllNames = ("winmm.dll", "version.dll", "winmm.dll.off", "version.dll.off")
wheatDirMarkers = ("loader.log", "mods", "consoleReport.txt", "consoleApi.on", "dumpConvars", "convars.txt")
maxDllScanBytes = 8 * 1024 * 1024


def findInstallRoots():
    steamDirs = [os.path.expanduser(p) for p in ("~/.local/share/Steam", "~/.steam/steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam")]
    libraries = set()
    for steam in steamDirs:
        libraries.add(steam)
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            with open(vdf, errors="ignore") as f:
                libraries.update(re.findall(r'"path"\s+"([^"]+)"', f.read()))
    roots = []
    for library in sorted(libraries):
        root = os.path.join(library, "steamapps", "common", "Counter-Strike Global Offensive")
        if os.path.isdir(root) and root not in roots:
            roots.append(root)
    return roots


def looksLikeCs2(root):
    return os.path.isdir(os.path.join(root, "game", "csgo")) and os.path.isdir(os.path.join(root, "game", "bin"))


def hasSignature(path):
    try:
        if os.path.getsize(path) > maxDllScanBytes:
            return False
        with open(path, "rb") as f:
            data = f.read()
        return any(sig in data for sig in signatures)
    except OSError:
        return False


def isOurWheatDir(path):
    return any(os.path.exists(os.path.join(path, marker)) for marker in wheatDirMarkers)


def scan(root):
    ours, leftAlone = [], []
    for current, dirs, files in os.walk(root, followlinks=False):
        for name in list(dirs):
            full = os.path.join(current, name)
            if name.lower() == "wheat":
                dirs.remove(name)
                if os.path.islink(full):
                    leftAlone.append((full, "a symlink named wheat; not touched"))
                elif isOurWheatDir(full):
                    ours.append((full, "wheat data folder"))
                else:
                    leftAlone.append((full, "folder named wheat without our files; not touched"))
            elif os.path.islink(full):
                dirs.remove(name)
        for name in files:
            full = os.path.join(current, name)
            lowered = name.lower()
            if os.path.islink(full):
                continue
            if lowered in ourDllNames or lowered.endswith(".dll.off"):
                if hasSignature(full):
                    ours.append((full, "loader DLL"))
                elif lowered in ourDllNames:
                    leftAlone.append((full, "same name as the loader DLL but without its signature; not touched"))
            elif "wheat" in lowered:
                leftAlone.append((full, "name contains 'wheat'; not touched"))
            elif lowered.endswith(".dll") and "bin" in current.split(os.sep) and hasSignature(full):
                ours.append((full, "renamed copy of the loader DLL"))
    return ours, leftAlone


def delete(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)


def main():
    parser = argparse.ArgumentParser(description="Find and remove Wheat loader files from a CS2 install. Dry run unless --apply is given.")
    parser.add_argument("--root", help="CS2 install folder (the one containing game/). Detected from Steam if omitted.")
    parser.add_argument("--apply", action="store_true", help="actually delete the files that are definitely ours")
    args = parser.parse_args()

    roots = [args.root] if args.root else findInstallRoots()
    if not roots:
        sys.exit("could not find a CS2 install; pass --root")
    leftovers = 0
    for root in roots:
        root = os.path.abspath(root)
        if not looksLikeCs2(root):
            print(f"skipping {root}: it does not look like a CS2 install (no game/csgo and game/bin)")
            continue
        print(f"scanning {root}")
        ours, leftAlone = scan(root)
        for path, why in ours:
            print(f"  {'DELETE' if args.apply else 'would delete'}: {os.path.relpath(path, root)}  ({why})")
            if args.apply:
                delete(path)
        for path, why in leftAlone:
            print(f"  left alone:  {os.path.relpath(path, root)}  ({why})")
        if not ours and not leftAlone:
            print("  nothing of ours found")
        leftovers += len(ours) if not args.apply else 0

    print()
    if args.apply:
        print("Done. Also: clear CS2's Launch Options in Steam, then right-click CS2 > Properties > Installed Files > Verify integrity.")
    elif leftovers:
        print("Dry run: nothing was deleted. Run again with --apply to remove the files marked 'would delete'.")
    else:
        print("Nothing to remove.")
    sys.exit(2 if leftovers else 0)


main()
