from .common import *


class ModManagementMixin:
        def manageMods(self, installation: dict):
            root = Path(installation["path"])
            wheat = gameBinFor(root, installation.get("cs2_platform", "windows")) / "wheat"
            activeRoot, disabledRoot = wheat / "mods", wheat / "mods-disabled"
            if self.process.state() != QProcess.ProcessState.NotRunning:
                QMessageBox.information(self, "Game or install running", "Wait for the current process before changing mod folders.")
                return
            activeRoot.mkdir(parents=True, exist_ok=True)
            dialog = QDialog(self)
            dialog.setWindowTitle(f"Manage Mods · {installation.get('name', 'Wheat')}")
            dialog.resize(720, 480)
            layout = QVBoxLayout(dialog)
            layout.addWidget(QLabel(str(activeRoot)))
            body = QHBoxLayout()
            listing = QListWidget()
            details = QTextBrowser()
            details.setOpenExternalLinks(False)
            body.addWidget(listing, 1)
            body.addWidget(details, 2)
            layout.addLayout(body, 1)
            actions = QHBoxLayout()
            stateButton = QPushButton("Disable")
            deleteButton = QPushButton("Delete Mod")
            refreshButton = QPushButton("Refresh")
            actions.addWidget(stateButton)
            actions.addWidget(deleteButton)
            actions.addStretch(1)
            actions.addWidget(refreshButton)
            layout.addLayout(actions)
            modLocations: dict[str, tuple[Path, bool]] = {}
    
            def refresh():
                listing.clear()
                modLocations.clear()
                for folder, enabled in ((activeRoot, True), (disabledRoot, False)):
                    try:
                        for entry in folder.iterdir():
                            if entry.is_dir() and not entry.is_symlink():
                                modLocations[entry.name] = (entry, enabled)
                    except OSError:
                        pass
                for name, (_, enabled) in sorted(modLocations.items(), key=lambda pair: pair[0].casefold()):
                    item = QListWidgetItem(("● " if enabled else "○ ") + name)
                    item.setData(Qt.ItemDataRole.UserRole, name)
                    listing.addItem(item)
                if not modLocations:
                    details.setPlainText("No mod folders found for this installation.")
    
            def selected():
                item = listing.currentItem()
                name = item.data(Qt.ItemDataRole.UserRole) if item else None
                return (name, modLocations.get(name)) if name else (None, None)
    
            def showDetails(*_):
                name, location = selected()
                if not name or not location:
                    stateButton.setEnabled(False)
                    deleteButton.setEnabled(False)
                    return
                folder, enabled = location
                stateButton.setEnabled(True)
                stateButton.setText("Disable" if enabled else "Enable")
                deleteButton.setEnabled(True)
                info = {"name": name, "description": "No description provided.", "author": "", "version": ""}
                try:
                    manifest = (folder / "manifest.lua").read_text(encoding="utf-8", errors="replace")
                    for key in info:
                        match = re.search(rf"{key}\s*=\s*['\"]([^'\"]*)['\"]", manifest)
                        if match:
                            info[key] = match.group(1)
                except OSError:
                    pass
                icon = next((p for p in folder.iterdir() if p.is_file() and p.name.lower() in ("icon.png", "icon.jpg", "icon.jpeg", "icon.gif")), None)
                imageHtml = f'<p><img src="{icon.asUri()}" width="96" height="96"></p>' if icon else ""
                details.setHtml(f"{imageHtml}<h2>{html.escape(info['name'])}</h2><p>{html.escape(info['description'])}</p>"
                                f"<p>Version: {html.escape(info['version'] or 'unknown')}<br>Author: {html.escape(info['author'] or 'unknown')}<br>"
                                f"Status: {'Enabled' if enabled else 'Disabled'}</p><p>{html.escape(str(folder))}</p>")
    
            def moveSelected():
                name, location = selected()
                if not name or not location:
                    return
                source, enabled = location
                destination = (disabledRoot if enabled else activeRoot) / name
                try:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    source.rename(destination)
                except OSError as exc:
                    QMessageBox.warning(dialog, "Could not change mod state", str(exc))
                refresh()
    
            def deleteSelected():
                name, location = selected()
                if not name or not location:
                    return
                answer = QMessageBox.question(dialog, "Delete Mod", f"Permanently delete '{name}' and all its files?",
                                             QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel)
                if answer != QMessageBox.StandardButton.Yes:
                    return
                try:
                    shutil.rmtree(location[0])
                except OSError as exc:
                    QMessageBox.warning(dialog, "Could not delete mod", str(exc))
                refresh()
    
            listing.currentItemChanged.connect(showDetails)
            stateButton.clicked.connect(moveSelected)
            deleteButton.clicked.connect(deleteSelected)
            refreshButton.clicked.connect(refresh)
            refresh()
            dialog.exec()

        @staticmethod
        def modManifestFields(modDir: Path) -> dict[str, str]:
            fields = {"name": modDir.name, "version": "", "description": "", "author": "", "repository": "", "update_url": ""}
            try:
                manifest = (modDir / "manifest.lua").read_text(encoding="utf-8", errors="replace")
            except OSError:
                return fields
            for key in fields:
                match = re.search(rf"\b{key}\s*=\s*['\"]([^'\"]*)['\"]", manifest)
                if match:
                    fields[key] = match.group(1).strip()
            return fields

        @staticmethod
        def checkOneModUpdate(modName: str, localVersion: str, updateUrl: str, repository: str) -> tuple[str, str, str, str]:
            endpoint = updateUrl
            if not endpoint and repository:
                match = re.search(r"github\.com[:/]([^/\s]+)/([^/#\s]+)", repository, re.IGNORECASE)
                if match:
                    owner, repo = match.groups()
                    endpoint = f"https://api.github.com/repos/{owner}/{repo.removesuffix('.git')}/releases/latest"
            if not endpoint:
                return modName, localVersion or "unknown", "", "No update source in manifest"
            if not endpoint.startswith("https://"):
                return modName, localVersion or "unknown", "", "Update source must use HTTPS"
            request = urllib.request.Request(endpoint, headers={"User-Agent": "WheatModLoader", "Accept": "application/json"})
            try:
                with urllib.request.urlopen(request, timeout=8) as response:
                    payload = json.loads(response.read(1_000_001).decode("utf-8"))
                if not isinstance(payload, dict):
                    raise ValueError("update metadata is not a JSON object")
                latest = str(payload.get("version") or payload.get("tag_name") or "").strip()
                releaseUrl = str(payload.get("release_url") or payload.get("html_url") or endpoint)
                if not latest:
                    raise ValueError("metadata has no version or tag_name")
                localNums = tuple(int(part) for part in re.findall(r"\d+", localVersion))
                latestNums = tuple(int(part) for part in re.findall(r"\d+", latest))
                width = max(len(localNums), len(latestNums))
                localKey = localNums + (0,) * (width - len(localNums))
                latestKey = latestNums + (0,) * (width - len(latestNums))
                if latestKey > localKey:
                    status = "Update available"
                elif latestKey == localKey and localNums:
                    status = "Up to date"
                else:
                    status = "Remote version is older or incomparable"
                if releaseUrl and not releaseUrl.startswith("https://"):
                    releaseUrl = ""
                return modName, localVersion or "unknown", latest, f"{status}\t{releaseUrl}"
            except Exception as exc:
                return modName, localVersion or "unknown", "", f"Check failed: {exc}"

        def checkModUpdates(self, installation: dict):
            root = Path(installation["path"])
            wheat = gameBinFor(root, installation.get("cs2_platform", "windows")) / "wheat"
            mods = []
            for folder in (wheat / "mods", wheat / "mods-disabled"):
                try:
                    entries = list(folder.iterdir())
                except OSError:
                    continue
                for entry in entries:
                    if entry.is_dir() and not entry.is_symlink():
                        info = self.modManifestFields(entry)
                        mods.append((info["name"], info["version"], info["update_url"], info["repository"]))
            if not mods:
                QMessageBox.information(self, "Mod Updates", "No installed mods were found for this installation.")
                return
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(self.checkOneModUpdate, *mod) for mod in mods]
                results = [future.result() for future in futures]
            rows = []
            for name, local, latest, status in sorted(results, key=lambda row: row[0].casefold()):
                statusText, _, releaseUrl = status.partition("\t")
                link = f' · <a href="{html.escape(releaseUrl, quote=True)}">Release</a>' if releaseUrl else ""
                latestText = latest or "—"
                rows.append(f"<tr><td>{html.escape(name)}</td><td>{html.escape(local)}</td>"
                            f"<td>{html.escape(latestText)}</td><td>{html.escape(statusText)}{link}</td></tr>")
            dialog = QDialog(self)
            dialog.setWindowTitle(f"Mod Updates · {installation.get('name', 'Wheat')}")
            dialog.resize(850, 420)
            layout = QVBoxLayout(dialog)
            view = QTextBrowser()
            view.setOpenExternalLinks(True)
            view.setHtml("<table width='100%' cellspacing='8'><tr><th align='left'>Mod</th><th align='left'>Installed</th>"
                         "<th align='left'>Latest</th><th align='left'>Status</th></tr>" + "".join(rows) + "</table>")
            layout.addWidget(view)
            closeButton = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
            closeButton.rejected.connect(dialog.reject)
            layout.addWidget(closeButton)
            dialog.exec()

        def checkInstallationHealth(self, installation: dict):
            root = Path(installation["path"]).expanduser()
            platform = installation.get("cs2_platform", "windows")
            exe = gameExecutable(root, platform)
            proxy = gameBinFor(root, platform) / "winmm.dll"
            wheat = proxy.parent / "wheat"
            checks: list[tuple[bool, str]] = []
            checks.append((root.is_dir(), f"Installation folder exists: {root}"))
            try:
                marker = json.loads((root / MARKER_NAME).read_text(encoding="utf-8"))
                checks.append((isinstance(marker, dict) and bool(marker.get("download_complete")), "Installation marker is valid and complete"))
            except (OSError, ValueError):
                marker = {}
                checks.append((False, "Installation marker is missing or invalid"))
            checks.append((exe.is_file(), f"CS2 executable exists: {exe.name}"))
            checks.append((marker.get("cs2_platform", platform) == platform, "Installation platform matches its marker"))
            checks.append((proxy.is_file() and isLoaderForPlatform(proxy, platform), f"Wheat loader matches {platform} format"))
            checks.append(((wheat / "mods").is_dir(), "Wheat mod directory exists"))
            apiFile = wheat / "apiAllowList.txt"
            if apiFile.exists():
                supported = {"console.get", "console.default", "console.allowed", "console.available", "data"}
                try:
                    unknown = set(apiFile.read_text(encoding="utf-8").splitlines()) - supported
                    checks.append((not unknown, "API allow list contains only supported APIs" if not unknown else f"Unknown API entries: {', '.join(sorted(unknown))}"))
                except OSError as exc:
                    checks.append((False, f"API allow list cannot be read: {exc}"))
            for folder in (wheat / "mods", wheat / "mods-disabled"):
                try:
                    entries = [path for path in folder.iterdir() if path.is_dir() and not path.is_symlink()]
                except OSError:
                    entries = []
                for mod in entries:
                    checks.append(((mod / "main.lua").is_file(), f"Mod {mod.name} has main.lua"))
            brokenLinks = 0
            if root.is_dir():
                for current, dirs, files in os.walk(root):
                    currentPath = Path(current)
                    for name in dirs + files:
                        path = currentPath / name
                        if path.is_symlink() and not path.exists():
                            brokenLinks += 1
                    dirs[:] = [name for name in dirs if not (currentPath / name).is_symlink()]
            checks.append((brokenLinks == 0, f"Broken symlinks: {brokenLinks}"))
            text = "\n".join(("PASS" if okay else "ISSUE") + "  " + description for okay, description in checks)
            QMessageBox.information(self, "Installation Health Check", text)

        def checkSteamIntegration(self, installation: dict):
            root = Path(installation["path"]).expanduser()
            platform = installation.get("cs2_platform", "windows")
            exe = gameExecutable(root, platform)
            proxy = gameBinFor(root, platform) / "winmm.dll"
            checks = [
                (steamClientRunning(), "Steam desktop client is running"),
                (detectSteamClientRoot() is not None, "Steam client installation was found"),
                (exe.is_file(), "Installation executable exists"),
                (proxy.is_file(), "Wheat loader exists in the game directory"),
            ]
            shortcutId = installation.get("steam_shortcut_appid")
            shortcutExe = installation.get("steam_shortcut_exe")
            checks.append((bool(shortcutId), "Steam shortcut is registered in launcher settings"))
            checks.append((shortcutExe == os.path.abspath(os.fspath(exe)), "Shortcut points to this installation"))
            matchingShortcut = False
            shortcutOptions = ""
            shortcutExecutable = ""
            steamRoot = detectSteamClientRoot()
            if steamRoot:
                for file in (steamRoot / "userdata").glob("*/config/shortcuts.vdf"):
                    try:
                        top = parseBinaryVdf(file.read_bytes())
                        shortcutField = next((value for key, value in top if key == "shortcuts"), None)
                        if not shortcutField or shortcutField[0] != 0:
                            continue
                        for _, typed in shortcutField[1]:
                            if typed[0] != 0:
                                continue
                            fields = typed[1]
                            rawAppid = vdfValue(fields, "appid")
                            if not isinstance(rawAppid, bytes) or len(rawAppid) != 4:
                                continue
                            appid = int.from_bytes(rawAppid, "little", signed=True) & 0xFFFFFFFF
                            if shortcutId and appid == int(shortcutId):
                                matchingShortcut = True
                                shortcutOptions = str(vdfValue(fields, "LaunchOptions") or "")
                                shortcutExecutable = str(vdfValue(fields, "Exe") or "")
                    except (OSError, ValueError, TypeError):
                        continue
            checks.append((matchingShortcut, "Steam shortcuts.vdf contains the Wheat entry"))
            actualShortcutExe = shortcutExecutable.strip('"')
            checks.append((bool(actualShortcutExe) and os.path.normcase(os.path.abspath(actualShortcutExe))
                           == os.path.normcase(os.path.abspath(os.fspath(exe))), "Steam shortcut executable points to this CS2 installation"))
            requiredOption = "LD_PRELOAD=" if platform == "linux" else "WINEDLLOVERRIDES=winmm=n,b"
            launchTargetOk = (requiredOption in shortcutOptions and "-insecure" in shortcutOptions
                                and (platform != "linux" or str(proxy) in shortcutOptions))
            checks.append((launchTargetOk,
                           f"Steam launch options include {requiredOption.rstrip('=')} and -insecure"))
            report = "\n".join(("PASS" if okay else "ISSUE") + "  " + text for okay, text in checks)
            QMessageBox.information(self, "Steam Integration Check", report)

        def exportInstallProfile(self, installation: dict):
            root = Path(installation["path"]).expanduser()
            platform = installation.get("cs2_platform", "windows")
            wheat = gameBinFor(root, platform) / "wheat"
            apiPath = wheat / "apiAllowList.txt"
            try:
                api = apiPath.read_text(encoding="utf-8").splitlines() if apiPath.exists() else ["console.get", "console.default", "console.allowed", "console.available", "data"]
            except OSError:
                api = []
            mods = {"enabled": [], "disabled": []}
            for state, folder in (("enabled", wheat / "mods"), ("disabled", wheat / "mods-disabled")):
                try:
                    mods[state] = sorted(entry.name for entry in folder.iterdir() if entry.is_dir() and not entry.is_symlink())
                except OSError:
                    pass
            profile = {
                "format": "wheat-install-profile",
                "schema_version": 1,
                "name": installation.get("name", "Wheat"),
                "cs2_platform": platform,
                "cs2_source": installation.get("cs2_source", "latest"),
                "cs2_version": installation.get("cs2_version", ""),
                "source_build_id": buildIdFor(root),
                "separate": bool(installation.get("separate", True)),
                "wheat_version": installation.get("wheat_version", ""),
                "wheat_dll": installation.get("wheat_dll", ""),
                "api_allow_list": api,
                "mods": mods,
                "mods_source_installation": str(root),
            }
            filename, _ = QFileDialog.getSaveFileName(self, "Export Install Profile", f"{profile['name']}.json", "Wheat install profile (*.json)")
            if not filename:
                return
            if Path(filename).suffix.lower() != ".json":
                filename += ".json"
            try:
                Path(filename).write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
                QMessageBox.information(self, "Profile Exported", f"Install profile saved to:\n{filename}")
            except OSError as exc:
                QMessageBox.warning(self, "Could not export profile", str(exc))

        def newInstallationFromProfile(self):
            filename, _ = QFileDialog.getOpenFileName(self, "Open Install Profile", str(Path.home()), "Wheat install profile (*.json)")
            if not filename:
                return
            try:
                profile = json.loads(Path(filename).read_text(encoding="utf-8"))
                if (not isinstance(profile, dict) or profile.get("format") != "wheat-install-profile"
                        or profile.get("schema_version") != 1 or profile.get("cs2_platform") not in ("windows", "linux")):
                    raise ValueError("This is not a supported Wheat install profile.")
            except (OSError, ValueError, TypeError) as exc:
                QMessageBox.warning(self, "Invalid Install Profile", str(exc))
                return
            self.newInstallation(profile)

        def configureApiAllowList(self, installation: dict):
            choices = ["console.get", "console.default", "console.allowed", "console.available", "data"]
            path = gameBinFor(Path(installation["path"]), installation.get("cs2_platform", "windows")) / "wheat/apiAllowList.txt"
            try:
                current = set(path.read_text(encoding="utf-8").splitlines()) if path.exists() else set(choices)
            except OSError as exc:
                QMessageBox.warning(self, "Could not read API permissions", str(exc))
                return
            dialog = QDialog(self)
            dialog.setWindowTitle(f"API Allow List · {installation.get('name', 'Wheat')}")
            dialog.resize(520, 380)
            layout = QVBoxLayout(dialog)
            layout.addWidget(QLabel("Choose which APIs mods can access. Changes apply when CS2 next starts."))
            search = QLineEdit()
            search.setPlaceholderText("Search APIs…")
            layout.addWidget(search)
            table = QTableWidget(len(choices), 2)
            table.setHorizontalHeaderLabels(["API", "Access"])
            table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
            table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
            table.verticalHeader().setVisible(False)
            table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
            table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
            originalAccess = {api: "Allowed" if api in current else "Blocked" for api in choices}
            for row, api in enumerate(choices):
                table.setItem(row, 0, QTableWidgetItem(api))
                table.setItem(row, 1, QTableWidgetItem(originalAccess[api]))
            layout.addWidget(table, 1)
            permissionActions = QHBoxLayout()
            allowButton = QPushButton("Allow")
            blockButton = QPushButton("Block")
            permissionActions.addWidget(allowButton)
            permissionActions.addWidget(blockButton)
            permissionActions.addStretch(1)
            layout.addLayout(permissionActions)
    
            def selectedApiRow() -> int:
                return table.currentRow()
    
            def setSelectedAccess(access: str):
                row = selectedApiRow()
                if row >= 0:
                    table.item(row, 1).setText(access)
                    updateChangedRow(row)
                    filterApiRows(search.text())
    
            def updateChangedRow(row: int):
                api = table.item(row, 0).text()
                changed = table.item(row, 1).text() != originalAccess[api]
                for column in range(2):
                    font = table.item(row, column).font()
                    font.setBold(changed)
                    table.item(row, column).setFont(font)
    
            def toggleSelectedAccess(row: int, Column: int):
                status = table.item(row, 1).text()
                setSelectedAccess("Blocked" if status == "Allowed" else "Allowed")
    
            def filterApiRows(text: str):
                query = text.strip().casefold()
                for row in range(table.rowCount()):
                    api = table.item(row, 0).text().casefold()
                    status = table.item(row, 1).text().casefold()
                    table.setRowHidden(row, query not in api and query not in status)
    
            allowButton.clicked.connect(lambda: setSelectedAccess("Allowed"))
            blockButton.clicked.connect(lambda: setSelectedAccess("Blocked"))
            table.cellDoubleClicked.connect(toggleSelectedAccess)
            search.textChanged.connect(filterApiRows)
    
            def updatePermissionActions():
                row = selectedApiRow()
                allowButton.setEnabled(row >= 0 and table.item(row, 1).text() != "Allowed")
                blockButton.setEnabled(row >= 0 and table.item(row, 1).text() != "Blocked")
    
            table.itemSelectionChanged.connect(updatePermissionActions)
            table.itemChanged.connect(updatePermissionActions)
            updatePermissionActions()
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
            buttons.accepted.connect(dialog.accept)
            buttons.rejected.connect(dialog.reject)
            layout.addWidget(buttons)
            if dialog.exec() != QDialog.DialogCode.Accepted:
                return
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("".join(table.item(row, 0).text() + "\n" for row in range(table.rowCount())
                                        if table.item(row, 1).text() == "Allowed"), encoding="utf-8")
            except OSError as exc:
                QMessageBox.warning(self, "Could not save API permissions", str(exc))

        def refreshMods(self, *_):
            return
