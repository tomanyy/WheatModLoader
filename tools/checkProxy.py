#!/usr/bin/env python3
import os, re, shutil, subprocess, sys

def findObjdump():
    for name in ("x86_64-w64-mingw32-objdump", "objdump"):
        if shutil.which(name):
            return name
    sys.exit("objdump not found")

def dump(path):
    if not os.path.isfile(path):
        sys.exit(f"file not found: {path}")
    result = subprocess.run([findObjdump(), "-p", path], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"objdump failed on {path}: {result.stderr.strip()}")
    return result.stdout

def importsFrom(path, dllName):
    wanted = dllName.lower() + ".dll"
    names, grabbing = set(), False
    for line in dump(path).splitlines():
        m = re.match(r"\s*DLL Name:\s*(\S+)", line)
        if m:
            grabbing = m.group(1).lower() == wanted
            continue
        if grabbing:
            m = re.match(r"\s+[0-9a-fA-F]+\s+(\d+)\s+(\S+)\s*$", line)
            if m:
                names.add(m.group(2))
    return names

def exportsOf(path):
    out = dump(path)
    if "[Ordinal/Name Pointer] Table" not in out:
        return set()
    section = out[out.index("[Ordinal/Name Pointer] Table"):]
    return {m.group(2) for m in re.finditer(r"^\s+\[\s*(\d+)\]\s+(\S+)\s*$", section, re.M)}

if len(sys.argv) != 4:
    sys.exit("usage: checkProxy.py <game.dll|exe> <proxy.dll> <proxiedDllName>")
gameFile, proxyFile, dllName = sys.argv[1:]
needed, available = importsFrom(gameFile, dllName), exportsOf(proxyFile)
missing = sorted(needed - available)
print(f"{gameFile}: {len(needed)} import(s) from {dllName}.dll: {' '.join(sorted(needed)) or '(none)'}")
if missing:
    print("MISSING from proxy:", " ".join(missing))
    sys.exit(1)
print("OK")
