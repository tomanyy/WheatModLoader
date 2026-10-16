from .common import *
from PyQt6.QtWidgets import QListWidgetItem


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
                    manifestName = self.modManifestFields(modLocations[name][0])["name"]
                    label = manifestName if manifestName != name else name
                    if label != name:
                        label += f" ({name})"
                    item = QListWidgetItem(("● " if enabled else "○ ") + label)
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
                info = self.modManifestFields(folder)
                if not info["description"]:
                    info["description"] = "No description provided."
                icon = next((p for p in folder.iterdir() if p.is_file() and p.name.lower() in ("icon.png", "icon.jpg", "icon.jpeg", "icon.gif")), None)
                imageHtml = f'<p><img src="{icon.as_uri()}" width="96" height="96"></p>' if icon else ""
                details.setHtml(f"{imageHtml}<h2>{html.escape(info['name'])}</h2><p>{html.escape(info['description'])}</p>"
                                f"<p>Version: {html.escape(info['version'] or 'unknown')}<br>Author: {html.escape(info['author'] or 'unknown')}<br>"
                                f"Internal ID: {html.escape(name)}<br>Status: {'Enabled' if enabled else 'Disabled'}</p><p>{html.escape(str(folder))}</p>")
    
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
                manifest = json.loads((modDir / "manifest.json").read_text(encoding="utf-8"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                return fields
            if not isinstance(manifest, dict):
                return fields
            for key in fields:
                value = manifest.get(key)
                if isinstance(value, str):
                    normalized = value.strip()
                    if normalized or key != "name":
                        fields[key] = normalized
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
            installationReport = ["Installation"] + [
                ("PASS" if okay else "ISSUE") + "  " + description for okay, description in checks
            ]
            steamChecks = self.steamIntegrationChecks(installation)
            steamReport = [
                ("PASS" if okay else "ISSUE") + "  " + description for okay, description in steamChecks
            ]
            QMessageBox.information(
                self,
                "Installation & Steam Health",
                "\n".join(installationReport + ["", "Steam Integration"] + steamReport),
            )

        def steamIntegrationChecks(self, installation: dict) -> list[tuple[bool, str]]:
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
            launchExe = gameLaunchExecutable(root, platform)
            shortcutExe = installation.get("steam_shortcut_exe")
            checks.append((bool(shortcutId), "Steam shortcut is registered in launcher settings"))
            checks.append((shortcutExe == os.path.abspath(os.fspath(launchExe)), "Shortcut points to this installation"))
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
                           == os.path.normcase(os.path.abspath(os.fspath(launchExe))), "Steam shortcut executable points to this CS2 installation"))
            requiredOption = ("LD_PRELOAD=" if platform == "linux" else
                              "WINEDLLOVERRIDES=winmm=n,b" if sys.platform.startswith("linux") else "")
            launchTargetOk = ((not requiredOption or requiredOption in shortcutOptions)
                                and "-steam" in shortcutOptions and "-insecure" in shortcutOptions
                                and (platform != "linux" or "bin/linuxsteamrt64/winmm.dll" in shortcutOptions))
            launchRequirements = ([requiredOption.rstrip("=")] if requiredOption else []) + ["-steam", "-insecure"]
            checks.append((launchTargetOk, "Steam launch options include " + ", ".join(launchRequirements)))
            return checks

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
                "vulkan_layer_enabled": bool(installation.get("vulkan_layer_enabled", False)),
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
            consoleApis = ["console.get", "console.default", "console.allowed", "console.available"]
            imguiApis = ["imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line"]
            choices = consoleApis + ["data"] + imguiApis
            wheat = gameBinFor(Path(installation["path"]), installation.get("cs2_platform", "windows")) / "wheat"
            path = wheat / "apiAllowList.txt"
            try:
                current = set(path.read_text(encoding="utf-8").splitlines()) if path.exists() else set(choices)
            except OSError as exc:
                QMessageBox.warning(self, "Could not read API permissions", str(exc))
                return
            access = {api: api in current for api in choices}
            originalAccess = access.copy()
            dialog = QDialog(self)
            dialog.setWindowTitle(f"API Allow List · {installation.get('name', 'Wheat')}")
            dialog.resize(520, 380)
            layout = QVBoxLayout(dialog)
            layout.addWidget(QLabel("Expand a category to manage access. Engine status reflects the last CS2 startup; permission changes apply after restart."))
            search = QLineEdit()
            search.setPlaceholderText("Search APIs…")
            layout.addWidget(search)
            tree = QTreeWidget()
            tree.setColumnCount(3)
            tree.setHeaderLabels(["API", "Access", "Engine status"])
            tree.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
            tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
            tree.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
            tree.header().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
            layout.addWidget(tree, 1)

            categories = {}
            apiItems = {}
            categoryApis = {"console": consoleApis, "data": ["data"], "imgui": imguiApis}
            for category, apis in categoryApis.items():
                parent = QTreeWidgetItem([category, "", ""])
                parent.setData(0, Qt.ItemDataRole.UserRole, "category:" + category)
                tree.addTopLevelItem(parent)
                categories[category] = parent
                for api in apis:
                    label = "table" if api == "data" else api + "()"
                    child = QTreeWidgetItem([label, "", ""])
                    child.setData(0, Qt.ItemDataRole.UserRole, "api:" + api)
                    parent.addChild(child)
                    apiItems[api] = child
                parent.setExpanded(False)

            permissionActions = QHBoxLayout()
            allowButton = QPushButton("Allow")
            blockButton = QPushButton("Block")
            permissionActions.addWidget(allowButton)
            permissionActions.addWidget(blockButton)
            permissionActions.addStretch(1)
            layout.addLayout(permissionActions)

            originalCategoryStatus = {
                category: "Allowed" if all(originalAccess[api] for api in apis)
                else "Blocked" if not any(originalAccess[api] for api in apis)
                else "Partial"
                for category, apis in categoryApis.items()
            }

            def consoleRuntimeStatus() -> str:
                if any(access[api] != originalAccess[api] for api in consoleApis):
                    return "Pending save and restart"
                if not path.is_file():
                    return "Inactive — save an allow list"
                try:
                    savedApis = set(path.read_text(encoding="utf-8").splitlines())
                    if not any(api in savedApis for api in consoleApis):
                        return "Blocked by allow list"
                    reportPath = wheat / "consoleReport.txt"
                    if not reportPath.is_file():
                        return "Inactive — start CS2"
                    report = reportPath.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    return "Status unavailable"
                try:
                    if path.stat().st_mtime_ns > reportPath.stat().st_mtime_ns:
                        return "Restart required"
                except OSError:
                    return "Status unavailable"
                if "RESULT: console API enabled (read-only)" in report or "RESULT: Linux engine interface and convar records calibrated" in report:
                    return "Ready (last startup)"
                if "FAILED:" in report or "RESULT: console API stays off" in report:
                    return "Unavailable — see consoleReport.txt"
                return "Not initialized — see consoleReport.txt"

            def refreshTree():
                for category, apis in categoryApis.items():
                    parent = categories[category]
                    count = sum(access[api] for api in apis)
                    status = "Blocked" if count == 0 else "Allowed" if count == len(apis) else "Partial"
                    parent.setText(1, status)
                    parent.setText(2, consoleRuntimeStatus() if category == "console" else "—")
                    for column in range(2):
                        font = parent.font(column)
                        font.setBold(column == 0 or status != originalCategoryStatus[category])
                        parent.setFont(column, font)
                    for api in apis:
                        item = apiItems[api]
                        item.setText(1, "Allowed" if access[api] else "Blocked")
                        item.setText(2, "—")
                        for column in range(2):
                            font = item.font(column)
                            font.setBold(access[api] != originalAccess[api])
                            item.setFont(column, font)
                updatePermissionActions()
                filterApiRows(search.text())

            def selectedItemApi(item):
                if item is None:
                    return ""
                return str(item.data(0, Qt.ItemDataRole.UserRole) or "")

            def setSelectedAccess(permission: str):
                item = tree.currentItem()
                selected = selectedItemApi(item)
                if selected.startswith("category:"):
                    category = selected.split(":", 1)[1]
                    for api in categoryApis[category]:
                        access[api] = permission == "Allowed"
                elif selected.startswith("api:"):
                    api = selected.split(":", 1)[1]
                    access[api] = permission == "Allowed"
                refreshTree()

            def toggleSelectedAccess(item, column: int):
                selected = selectedItemApi(item)
                if selected.startswith("category:"):
                    category = selected.split(":", 1)[1]
                    apis = categoryApis[category]
                    status = "Allowed" if all(access[api] for api in apis) else "Blocked"
                else:
                    api = selected.split(":", 1)[1]
                    status = "Allowed" if access[api] else "Blocked"
                setSelectedAccess("Blocked" if status == "Allowed" else "Allowed")

            def filterApiRows(text: str):
                query = text.strip().casefold()
                for category, apis in categoryApis.items():
                    parent = categories[category]
                    categoryStatus = parent.text(1).casefold()
                    parentMatch = not query or query in category.casefold() or query in categoryStatus
                    matchingChildren = []
                    for api in apis:
                        item = apiItems[api]
                        status = "allowed" if access[api] else "blocked"
                        label = item.text(0).casefold()
                        visible = not query or parentMatch or query in api.casefold() or query in label or query in status
                        item.setHidden(not visible)
                        if visible:
                            matchingChildren.append(item)
                    parent.setHidden(bool(query) and not parentMatch and not matchingChildren)
                    if query and not parent.isHidden():
                        parent.setExpanded(True)
                    elif not query:
                        parent.setExpanded(False)

            allowButton.clicked.connect(lambda: setSelectedAccess("Allowed"))
            blockButton.clicked.connect(lambda: setSelectedAccess("Blocked"))
            tree.itemDoubleClicked.connect(toggleSelectedAccess)
            search.textChanged.connect(filterApiRows)

            def updatePermissionActions():
                item = tree.currentItem()
                selected = selectedItemApi(item)
                if selected.startswith("category:"):
                    category = selected.split(":", 1)[1]
                    apis = categoryApis[category]
                    allowButton.setEnabled(not all(access[api] for api in apis))
                    blockButton.setEnabled(any(access[api] for api in apis))
                elif selected.startswith("api:"):
                    enabled = access[selected.split(":", 1)[1]]
                    allowButton.setEnabled(not enabled)
                    blockButton.setEnabled(enabled)
                else:
                    allowButton.setEnabled(False)
                    blockButton.setEnabled(False)

            tree.itemSelectionChanged.connect(updatePermissionActions)
            refreshTree()
            updatePermissionActions()
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
            buttons.accepted.connect(dialog.accept)
            buttons.rejected.connect(dialog.reject)
            layout.addWidget(buttons)
            if dialog.exec() != QDialog.DialogCode.Accepted:
                return
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("".join(api + "\n" for api in choices if access[api]), encoding="utf-8")
            except OSError as exc:
                QMessageBox.warning(self, "Could not save API permissions", str(exc))

        def refreshMods(self, *_):
            return
