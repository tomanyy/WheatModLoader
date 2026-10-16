

from __future__ import annotations

import json
import hashlib
import html
import os
import re
import shutil
import shlex
import signal
import sys
import zlib
import concurrent.futures
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path

from PyQt6.QtCore import QProcess, QProcessEnvironment, QSettings, QStandardPaths, Qt, QUrl, QTimer, QObject, QRunnable, QThreadPool, pyqtSignal
from PyQt6.QtGui import QColor, QDesktopServices, QFont, QPalette, QTextCursor
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHeaderView,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QMenu,
    QPlainTextEdit,
    QTextBrowser,
    QProgressBar,
    QPushButton,
    QStyle,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)


APP_ID = "730"
MARKER_NAME = ".wheat-launcher.json"
WHEAT_LAUNCHER_DIR = Path(__file__).resolve().parent
REPO_ROOT = WHEAT_LAUNCHER_DIR.parent
WHEAT_RELEASE_LABEL = "Wheat v1.1.0-preRelease"
WHEAT_RELEASE_TAG = "WheatInternal_v1.1.0-preRelease"
WHEAT_RELEASE_API = f"https://api.github.com/repos/tomanyy/WheatModLoader/releases/tags/{WHEAT_RELEASE_TAG}"
WHEAT_RELEASES_API = "https://api.github.com/repos/tomanyy/WheatModLoader/releases?per_page=20"


def buildIdFor(gameDir: Path) -> str | None:
    manifest = gameDir.parent.parent / "appmanifest_730.acf"
    try:
        text = manifest.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r'"buildid"\s*"(\d+)"', text, re.IGNORECASE)
    return match.group(1) if match else None


def steamLibraries() -> list[Path]:
    roots = [Path.home() / ".steam/steam", Path.home() / ".local/share/Steam"]
    roots.extend(Path(p) for p in ("/usr/share/steam", "/mnt/games/SteamLibrary"))
    found: list[Path] = []
    for root in roots:
        for library in (root,):
            if library.is_dir() and library not in found:
                found.append(library)
        vdf = root / "steamapps/libraryfolders.vdf"
        try:
            content = vdf.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for raw in re.findall(r'"path"\s*"([^"]+)"', content):
            library = Path(raw.replace("\\\\", "\\")).expanduser()
            if library.is_dir() and library not in found:
                found.append(library)
    return found


def detectSteamClientRoot() -> Path | None:
    candidates = (
        Path.home() / ".steam/steam",
        Path.home() / ".steam/debian-installation",
        Path.home() / ".local/share/Steam",
        Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
        Path("/usr/share/steam"),
    )
    checked: set[Path] = set()
    for candidate in candidates:
        try:
            resolved = candidate.expanduser().resolve()
        except OSError:
            continue
        if resolved in checked:
            continue
        checked.add(resolved)
        if not (resolved / "steamapps").is_dir():
            continue
        if any((resolved / name).exists() for name in (
            "steam", "steam.sh", "steamclient.so", "ubuntu12_32/steamclient.so",
        )):
            return resolved
    return None


def steamClientRunning() -> bool:
    if not sys.platform.startswith("linux"):
        return True
    procRoot = Path("/proc")
    try:
        processes = procRoot.iterdir()
    except OSError:
        return False
    for entry in processes:
        if not entry.name.isdigit():
            continue
        try:
            name = (entry / "comm").read_text(encoding="utf-8").strip().lower()
        except OSError:
            continue
        if name == "steam":
            return True
    return False


def detectVanillaGame() -> Path | None:
    for library in steamLibraries():
        candidate = library / "steamapps/common/Counter-Strike Global Offensive"
        if gameExecutable(candidate, "windows").is_file() or gameExecutable(candidate, "linux").is_file():
            return candidate
    return None


def gameBinFor(gameDir: Path, platform: str) -> Path:
    return gameDir / ("game/bin/linuxsteamrt64" if platform == "linux" else "game/bin/win64")


def gameExecutable(gameDir: Path, platform: str) -> Path:
    return gameBinFor(gameDir, platform) / ("cs2" if platform == "linux" else "cs2.exe")


def steamLinuxRuntimeExecutable() -> Path | None:
    for library in steamLibraries():
        runtime = library / "steamapps/common/SteamLinuxRuntime_sniper/run"
        if runtime.is_file() and os.access(runtime, os.X_OK):
            return runtime
    return None


def gameLaunchExecutable(gameDir: Path, platform: str) -> Path:
    if platform == "linux":
        return steamLinuxRuntimeExecutable() or gameDir / "game/cs2.sh"
    return gameExecutable(gameDir, platform)


def gamePlatform(gameDir: Path) -> str | None:
    if gameExecutable(gameDir, "windows").is_file():
        return "windows"
    if gameExecutable(gameDir, "linux").is_file():
        return "linux"
    return None


def detectSteamcmd() -> Path | None:
    for candidate in (
        shutil.which("steamcmd"), shutil.which("steamcmd.sh"),
        "/usr/bin/steamcmd", "/usr/games/steamcmd",
        str(Path.home() / ".steam/steamcmd/steamcmd.sh"),
        str(Path.home() / ".local/share/Steam/steamcmd/steamcmd.sh"),
        str(Path.home() / ".local/share/steamcmd/steamcmd.sh"),
        str(Path.home() / "Steam/steamcmd/steamcmd.sh"),
        str(Path.home() / "steamcmd/steamcmd.sh"),
    ):
        if candidate and Path(candidate).is_file() and (os.name == "nt" or os.access(candidate, os.X_OK)):
            return Path(candidate)
    return None


def isLoaderForPlatform(path: Path, platform: str) -> bool:
    try:
        with path.open("rb") as loaderFile:
            signature = loaderFile.read(4)
    except OSError:
        return False
    return signature.startswith(b"\x7fELF") if platform == "linux" else signature.startswith(b"MZ")


def detectWheatDll(platform: str | None = None) -> Path | None:
    selectedPlatform = platform or ("linux" if sys.platform.startswith("linux") else "windows")
    artifactName = "wheatLinux.dll" if selectedPlatform == "linux" else "wheatWindows.dll"
    buildDirectories = ("build-linux-sniper", "build-linux", "build", "build/Release", "Release") if selectedPlatform == "linux" else ("build-win", "build", "build/Release", "Release")
    candidates = tuple(REPO_ROOT / directory / artifactName for directory in buildDirectories) + (REPO_ROOT / artifactName,)
    for candidate in candidates:
        if candidate.is_file() and (platform is None or isLoaderForPlatform(candidate, platform)):
            return candidate
    return None


def readCstring(data: bytes, offset: int) -> tuple[str, int]:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("Invalid Steam shortcuts file")
    return data[offset:end].decode("utf-8", errors="replace"), end + 1


def parseBinaryVdf(data: bytes) -> list[tuple[str, tuple[int, object]]]:
    def parseMap(offset: int) -> tuple[list[tuple[str, tuple[int, object]]], int]:
        entries = []
        while offset < len(data):
            kind = data[offset]
            offset += 1
            if kind == 8:
                return entries, offset
            key, offset = readCstring(data, offset)
            if kind == 0:
                value, offset = parseMap(offset)
            elif kind == 1:
                value, offset = readCstring(data, offset)
            elif kind in (2, 3, 4, 6):
                if offset + 4 > len(data):
                    raise ValueError("Truncated Steam shortcuts file")
                value = data[offset:offset + 4]
                offset += 4
            elif kind == 7:
                if offset + 8 > len(data):
                    raise ValueError("Truncated Steam shortcuts file")
                value = data[offset:offset + 8]
                offset += 8
            elif kind == 5:
                end = offset
                while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
                    end += 2
                if end + 1 >= len(data):
                    raise ValueError("Truncated Steam shortcuts file")
                value = data[offset:end + 2]
                offset = end + 2
            else:
                raise ValueError(f"Unknown Steam VDF field type {kind}")
            entries.append((key, (kind, value)))
        raise ValueError("Steam shortcuts file has no map terminator")

    if not data:
        return []
    parsed, end = parseMap(0)
    if end != len(data):
        raise ValueError("Steam shortcuts file has unexpected trailing bytes")
    return parsed


def encodeBinaryVdf(entries: list[tuple[str, tuple[int, object]]]) -> bytes:
    out = bytearray()
    for key, (kind, value) in entries:
        out.append(kind)
        out.extend(key.encode("utf-8") + b"\0")
        if kind == 0:
            out.extend(encodeBinaryVdf(value))
        elif kind == 1:
            out.extend(str(value).encode("utf-8") + b"\0")
        elif kind in (2, 3, 4, 5, 6, 7):
            out.extend(value)
        else:
            raise ValueError(f"Unsupported Steam VDF field type {kind}")
    out.append(8)
    return bytes(out)


def steamVdfString(value: str) -> tuple[int, str]:
    return 1, value


def steamVdfInt(value: int) -> tuple[int, bytes]:
    return 2, int(value).to_bytes(4, "little", signed=True)


def vdfValue(entries: list[tuple[str, tuple[int, object]]], key: str):
    return next((typed[1] for name, typed in entries if name == key), None)


def createOrUpdateSteamEntry(shortcutsFile: Path, title: str, executable: Path,
                                 platform: str = "windows", proxy: Path | None = None,
                                 vulkanLayerDir: Path | None = None) -> tuple[bool, int]:
    root = parseBinaryVdf(shortcutsFile.read_bytes()) if shortcutsFile.exists() else []
    shortcutsField = next((typed for key, typed in root if key == "shortcuts"), None)
    if shortcutsField is not None and shortcutsField[0] != 0:
        raise ValueError("Steam shortcuts root has an unexpected format")
    shortcuts = shortcutsField[1] if shortcutsField else None
    if shortcuts is None:
        shortcuts = []
        root.append(("shortcuts", (0, shortcuts)))
    executable = Path(os.path.abspath(os.fspath(executable.expanduser())))
    exePath = str(executable)
    exe = f'"{exePath}"'
    workingDirectory = proxy.parent.parent.parent if platform == "linux" and proxy is not None else executable.parent
    startDir = f'"{workingDirectory}"'
    shortcutId = zlib.crc32((exe + title).encode("utf-8")) | 0x80000000
    signedShortcutId = shortcutId - 0x100000000
    shortcut = None
    for _, typed in shortcuts:
        if typed[0] != 0:
            continue
        fields = typed[1]
        oldExe = vdfValue(fields, "Exe")
        oldTitle = vdfValue(fields, "AppName")
        oldExePath = oldExe[1:-1] if isinstance(oldExe, str) and len(oldExe) >= 2 and oldExe[0] == oldExe[-1] == '"' else oldExe
        sameExecutable = oldExe == exe or oldTitle == title
        if isinstance(oldExePath, str):
            try:
                sameExecutable = sameExecutable or Path(oldExePath).resolve(strict=False) == executable.resolve(strict=False)
            except OSError:
                pass
        if sameExecutable:
            shortcut = fields
            break
    added = shortcut is None
    if shortcut is None:
        shortcut = []
        shortcuts.append((str(len(shortcuts)), (0, shortcut)))
    launchOptions = "-steam -insecure"
    if platform == "linux" and proxy is not None:
        proxyPath = os.path.relpath(proxy, workingDirectory)
        layerOptions = ""
        if vulkanLayerDir is not None:
            layerPath = os.path.abspath(os.fspath(vulkanLayerDir))
            layerOptions = f"WHEAT_ENABLE_VULKAN_LAYER=1 VK_ADD_IMPLICIT_LAYER_PATH={shlex.quote(layerPath)} "
        launchOptions = f"{layerOptions}LD_PRELOAD={proxyPath} %command% -- ./cs2.sh -steam -insecure"
    elif platform == "windows" and sys.platform.startswith("linux"):
        launchOptions = "WINEDLLOVERRIDES=winmm=n,b %command% -steam -insecure"
    values = {
        "appid": steamVdfInt(signedShortcutId),
        "AppName": steamVdfString(title), "Exe": steamVdfString(exe),
        "StartDir": steamVdfString(startDir), "icon": steamVdfString(""),
        "ShortcutPath": steamVdfString(""),
        "LaunchOptions": steamVdfString(launchOptions),
        "IsHidden": steamVdfInt(0), "AllowDesktopConfig": steamVdfInt(1),
        "AllowOverlay": steamVdfInt(1), "OpenVR": steamVdfInt(0),
        "Devkit": steamVdfInt(0), "DevkitGameID": steamVdfString(""),
        "LastPlayTime": steamVdfInt(0), "FlatpakAppID": steamVdfString(""),
    }
    for key, typed in values.items():
        index = next((i for i, (field, _) in enumerate(shortcut) if field == key), None)
        if index is None:
            shortcut.append((key, typed))
        else:
            shortcut[index] = (key, typed)
    if vdfValue(shortcut, "tags") is None:
        shortcut.append(("tags", (0, [])))
    shortcutsFile.parent.mkdir(parents=True, exist_ok=True)
    temporary = shortcutsFile.with_name(shortcutsFile.name + ".tmp")
    temporary.write_bytes(encodeBinaryVdf(root))
    temporary.replace(shortcutsFile)
    return added, shortcutId


def applyFleasionStyle(app: QApplication, theme: str = "Light") -> None:
    app.setStyle("Fusion")
    palette = QPalette()
    if theme == "Dark":
        colors = {
            QPalette.ColorRole.Window: "#252525", QPalette.ColorRole.WindowText: "#eeeeee",
            QPalette.ColorRole.Base: "#191919", QPalette.ColorRole.AlternateBase: "#303030",
            QPalette.ColorRole.ToolTipBase: "#303030", QPalette.ColorRole.ToolTipText: "#eeeeee",
            QPalette.ColorRole.Text: "#eeeeee", QPalette.ColorRole.Button: "#343434",
            QPalette.ColorRole.ButtonText: "#eeeeee", QPalette.ColorRole.BrightText: "#ffffff",
            QPalette.ColorRole.Light: "#454545", QPalette.ColorRole.Midlight: "#3c3c3c",
            QPalette.ColorRole.Dark: "#171717", QPalette.ColorRole.Mid: "#414141",
            QPalette.ColorRole.Shadow: "#101010", QPalette.ColorRole.Highlight: "#2878a8",
            QPalette.ColorRole.HighlightedText: "#ffffff", QPalette.ColorRole.PlaceholderText: "#aaaaaa",
            QPalette.ColorRole.Link: "#73b9e6",
        }
    else:
        colors = {
            QPalette.ColorRole.Window: "#efefef", QPalette.ColorRole.WindowText: "#202020",
            QPalette.ColorRole.Base: "#ffffff", QPalette.ColorRole.AlternateBase: "#f5f5f5",
            QPalette.ColorRole.ToolTipBase: "#ffffdc", QPalette.ColorRole.ToolTipText: "#202020",
            QPalette.ColorRole.Text: "#202020", QPalette.ColorRole.Button: "#efefef",
            QPalette.ColorRole.ButtonText: "#202020", QPalette.ColorRole.BrightText: "#ffffff",
            QPalette.ColorRole.Light: "#ffffff", QPalette.ColorRole.Midlight: "#e5e5e5",
            QPalette.ColorRole.Dark: "#8f8f8f", QPalette.ColorRole.Mid: "#b8b8b8",
            QPalette.ColorRole.Shadow: "#606060", QPalette.ColorRole.Highlight: "#308cc6",
            QPalette.ColorRole.HighlightedText: "#ffffff", QPalette.ColorRole.PlaceholderText: "#777777",
            QPalette.ColorRole.Link: "#245f91",
        }
    for role, color in colors.items():
        palette.setColor(role, QColor(color))
    app.setPalette(palette)
    app.setFont(QFont("Arial", 9))
    if theme == "Dark":
        app.setStyleSheet(
            "QPushButton:disabled { color: #8b8b8b; background-color: #3a3a3a; border: 1px solid #494949; }"
            "QTabBar::tab:disabled { color: #858585; background-color: #303030; }"
        )
    else:
        app.setStyleSheet(
            "QPushButton:disabled { color: #777777; background-color: #d4d4d4; border: 1px solid #b8b8b8; }"
            "QTabBar::tab:disabled { color: #888888; background-color: #dddddd; }"
        )


def isInside(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def mergeTree(source: Path, destination: Path) -> int:
    if not source.is_dir():
        raise FileNotFoundError(f"Downloaded depot folder not found: {source}")
    count = 0
    for root, dirs, files in os.walk(source):
        rootPath = Path(root)
        relative = rootPath.relative_to(source)
        targetDir = destination / relative
        targetDir.mkdir(parents=True, exist_ok=True)
        dirs[:] = [name for name in dirs if not (rootPath / name).is_symlink()]
        for name in files:
            src = rootPath / name
            if src.is_symlink():
                continue
            dst = targetDir / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst.is_symlink():
                dst.unlink()
            shutil.copy2(src, dst)
            count += 1
    return count


def directorySizeBytes(directory: Path) -> int:
    total = 0
    for root, dirs, files in os.walk(directory):
        rootPath = Path(root)
        dirs[:] = [name for name in dirs if not (rootPath / name).is_symlink()]
        for name in files:
            path = rootPath / name
            try:
                if not path.is_symlink():
                    total += path.stat().st_size
            except OSError:
                continue
    return total


def formatSizeBytes(size: int) -> str:
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024 or unit == "TiB":
            return f"{value:.1f} {unit}" if unit != "B" else f"{int(value)} B"
        value /= 1024


def linkMissingFiles(source: Path, destination: Path) -> int:
    count = 0
    for root, dirs, files in os.walk(source):
        rootPath = Path(root)
        relativeRoot = rootPath.relative_to(source)
        dirs[:] = [name for name in dirs if name.lower() != "wheat" and not (rootPath / name).is_symlink()]
        targetDir = destination / relativeRoot
        targetDir.mkdir(parents=True, exist_ok=True)
        for name in files:
            relative = relativeRoot / name
            if str(relative).replace("\\", "/").lower() in (
                "game/bin/win64/winmm.dll", "game/bin/linuxsteamrt64/winmm.dll",
            ):
                continue
            src = rootPath / name
            dst = destination / relative
            if not src.exists() or dst.exists() or dst.is_symlink():
                continue
            try:
                os.symlink(src.resolve(), dst)
                count += 1
            except OSError as exc:
                raise OSError(f"Could not link {src} to {dst}: {exc}") from exc
    return count



__all__ = [name for name in globals() if not name.startswith("__")]
