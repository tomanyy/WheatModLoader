from .common import *
from .widgets import NoWheelComboBox


class InstallationMixin:
        def selectedInstallation(self) -> dict | None:
            row = self.installTable.currentRow() if hasattr(self, "installTable") else -1
            if row < 0 or row >= self.installTable.rowCount():
                return None
            first = self.installTable.item(row, 0)
            installId = first.data(Qt.ItemDataRole.UserRole) if first else None
            return next((item for item in self.installations if item.get("id") == installId), None)

        def installationById(self, installId: str) -> dict | None:
            return next((item for item in self.installations if item.get("id") == installId), None)

        def refreshInstallations(self):
            if not hasattr(self, "installTable"):
                return
            currentId = self.SelectedInstallationId
            self.installTable.blockSignals(True)
            self.installTable.setRowCount(len(self.installations))
            for row, installation in enumerate(self.installations):
                root = Path(installation.get("path", "")).expanduser()
                markerPath = root / MARKER_NAME
                try:
                    marker = json.loads(markerPath.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    marker = {}
                if installation.get("status") == "Installing" and self.ActiveInstallationId == installation.get("id"):
                    state = "Installing"
                elif marker.get("download_complete") and gameExecutable(root, installation.get("cs2_platform", marker.get("cs2_platform", "windows"))).is_file():
                    state = "Ready"
                elif installation.get("status") == "Failed":
                    state = "Failed"
                elif root.exists():
                    state = "Incomplete"
                else:
                    state = "Missing"
                installation["status"] = state
                platform = installation.get("cs2_platform", marker.get("cs2_platform", "windows"))
                values = [str(installation.get("name", "Wheat")),
                          "Linux" if platform == "linux" else "Windows",
                          str(installation.get("cs2_version", "Latest public")),
                          str(installation.get("wheat_version", "Bundled")), state, str(root)]
                for column, value in enumerate(values):
                    cell = QTableWidgetItem(value)
                    if column == 0:
                        cell.setData(Qt.ItemDataRole.UserRole, installation.get("id"))
                    if column == 5:
                        cell.setToolTip(value)
                    self.installTable.setItem(row, column, cell)
                if installation.get("id") == currentId:
                    self.installTable.selectRow(row)
            if self.installations and self.installTable.currentRow() < 0:
                self.installTable.selectRow(0)
            self.installTable.blockSignals(False)
            selected = self.selectedInstallation()
            if selected:
                self.SelectedInstallationId = selected.get("id", "")
                self.ModsInstallationId = self.SelectedInstallationId
                self.statusLabel.setText(f"{selected.get('status', 'Missing')} · {selected.get('name')} · {selected.get('path')}")
            else:
                self.ModsInstallationId = ""
                self.statusLabel.setText("No Wheat installations yet. Choose New Installation to begin.")
            self.updateActionStates()

        def selectedInstallationChanged(self):
            installation = self.selectedInstallation()
            self.SelectedInstallationId = installation.get("id", "") if installation else ""
            self.ModsInstallationId = self.SelectedInstallationId
            self.refreshMods()
            self.refreshRuntimeLog()
            self.updateActionStates()
            if installation:
                self.saveSettings()
                self.refreshInstallations()

        def installationContextMenu(self, point):
            row = self.installTable.rowAt(point.y())
            if row < 0:
                return
            self.installTable.selectRow(row)
            installation = self.selectedInstallation()
            if not installation:
                return
            menu = QMenu(self)
            style = self.style()
            launchAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_MediaPlay), "Launch")
            steamEntryAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_DialogApplyButton), "Add / Update Steam Entry")
            modsAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_FileDialogContentsView), "Manage Mods")
            updateModsAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_BrowserReload), "Check Mod Updates")
            apiAction = menu.addAction("API Allow List…")
            logsAction = menu.addAction("Logs…")
            menu.addSeparator()
            healthAction = menu.addAction("Check Installation & Steam Health")
            exportProfileAction = menu.addAction("Export Install Profile…")
            repairAction = None
            if installation.get("cs2_source") == "latest" and self.steamcmdPath():
                repairAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_BrowserReload), "Repair / Update CS2 with SteamCMD")
            openAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_DirOpenIcon), "Open Folder")
            renameAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_FileDialogDetailedView), "Rename")
            menu.addSeparator()
            deleteAction = menu.addAction(style.standardIcon(QStyle.StandardPixmap.SP_TrashIcon), "Delete Installation")
            chosen = menu.exec(self.installTable.viewport().mapToGlobal(point))
            if chosen == launchAction:
                self.launchSelected()
            elif chosen == steamEntryAction:
                self.addInstallationToSteam(installation)
            elif chosen == modsAction:
                self.ModsInstallationId = installation.get("id", "")
                self.manageMods(installation)
            elif chosen == updateModsAction:
                self.checkModUpdates(installation)
            elif chosen == apiAction:
                self.configureApiAllowList(installation)
            elif chosen == logsAction:
                self.openInstallationLogs(installation)
            elif chosen == healthAction:
                self.checkInstallationHealth(installation)
            elif chosen == exportProfileAction:
                self.exportInstallProfile(installation)
            elif repairAction is not None and chosen == repairAction:
                self.startSteamcmdInstall(installation)
            elif chosen == openAction:
                path = Path(installation["path"])
                if path.is_dir():
                    QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))
            elif chosen == renameAction:
                name, accepted = QInputDialog.getText(self, "Rename Installation", "Name:", text=installation.get("name", "Wheat"))
                if accepted and name.strip():
                    installation["name"] = name.strip()
                    self.saveSettings()
                    self.refreshInstallations()
            elif chosen == deleteAction:
                self.deleteInstallation(installation)

        def addInstallationToSteam(self, installation: dict):
            if steamClientRunning():
                QMessageBox.information(
                    self, "Exit Steam first",
                    "Fully exit Steam, then choose Add / Update Steam Entry again. Steam keeps shortcuts.vdf in memory and may overwrite changes when it exits.",
                )
                return
            target = Path(installation.get("path", "")).expanduser()
            platform = installation.get("cs2_platform", "windows")
            if platform == "linux" and steamLinuxRuntimeExecutable() is None:
                QMessageBox.warning(
                    self, "Steam Linux Runtime not found",
                    "Install or update Steam Linux Runtime 3.0 (sniper) from Steam Tools before adding the Linux Steam entry.",
                )
                return
            runtimeExecutable = gameExecutable(target, platform)
            executable = gameLaunchExecutable(target, platform)
            proxy = gameBinFor(target, platform) / "winmm.dll"
            try:
                marker = json.loads((target / MARKER_NAME).read_text(encoding="utf-8"))
            except (OSError, ValueError):
                marker = {}
            if (not runtimeExecutable.is_file() or not executable.is_file() or not proxy.is_file()
                    or not marker.get("download_complete")):
                QMessageBox.warning(self, "Installation not ready", "This Wheat installation is incomplete.")
                return
            steamRoot = detectSteamClientRoot()
            if not steamRoot:
                QMessageBox.warning(self, "Steam not found", "Could not find the Steam client installation.")
                return
            userdata = steamRoot / "userdata"
            configs = []
            if userdata.is_dir():
                for userDir in userdata.iterdir():
                    config = userDir / "config"
                    if userDir.name.isdigit() and config.is_dir() and (config / "localconfig.vdf").is_file():
                        configs.append(config)
            if not configs:
                QMessageBox.warning(self, "Steam profile not found", f"No Steam user configuration was found under:\n{userdata}")
                return
            if len(configs) == 1:
                config = configs[0]
            else:
                labels = [f"{item.parent.name} · {item}" for item in configs]
                selection, accepted = QInputDialog.getItem(self, "Choose Steam profile", "Add the Wheat entry to:", labels, 0, False)
                if not accepted:
                    return
                config = configs[labels.index(selection)]
            shortcutsFile = config / "shortcuts.vdf"
            title = f"Counter-Strike 2 (Wheat · {installation.get('name', 'Wheat')})"
            try:
                appidFile = runtimeExecutable.parent / "steam_appid.txt"
                if not appidFile.is_file() or appidFile.read_text(encoding="ascii").strip() != APP_ID:
                    appidFile.write_text(APP_ID + "\n", encoding="ascii")
                backup = shortcutsFile.with_name("shortcuts.vdf.bak")
                if shortcutsFile.exists() and not backup.exists():
                    shutil.copy2(shortcutsFile, backup)
                layerDir = gameBinFor(target, platform) / "wheat" / "vulkan"
                layerDir = layerDir if installation.get("vulkan_layer_enabled") and (layerDir / "VK_LAYER_WHEAT_modloader.json").is_file() else None
                added, shortcutId = createOrUpdateSteamEntry(shortcutsFile, title, executable, platform, proxy, layerDir)
            except (OSError, ValueError, UnicodeError) as exc:
                QMessageBox.critical(self, "Could not prepare Steam entry", str(exc))
                return
            installation["steam_shortcut_appid"] = shortcutId
            installation["steam_shortcut_title"] = title
            installation["steam_shortcut_exe"] = str(Path(os.path.abspath(os.fspath(executable))))
            self.saveSettings()
            self.log.appendPlainText(f"{'Added' if added else 'Updated'} Steam entry for {installation.get('name')}.")
            compatibility = " Set Proton under Properties → Compatibility." if platform == "windows" and sys.platform.startswith("linux") else ""
            QMessageBox.information(self, "Steam entry ready",
                f"{'Added' if added else 'Updated'} “{title}”. Restart Steam to load it.{compatibility} The launcher starts it through Steam with -insecure.")

        def deleteInstallation(self, installation: dict):
            root = Path(installation.get("path", "")).expanduser().resolve()
            vanilla = self.gameDir().resolve()
            if root == vanilla or isInside(root, vanilla) or isInside(vanilla, root):
                QMessageBox.warning(self, "Unsafe installation path", "This path overlaps the vanilla Steam installation and cannot be deleted.")
                return
            answer = QMessageBox.question(
                self, "Delete Installation?",
                f"Permanently delete this Wheat installation and all files inside it?\n\n{root}",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
            try:
                if root.is_dir():
                    shutil.rmtree(root)
                self.installations.remove(installation)
                self.SelectedInstallationId = ""
                self.saveSettings()
                self.refreshInstallations()
            except OSError as exc:
                QMessageBox.critical(self, "Could not delete installation", str(exc))

        def wheatVersionChoices(self, platform: str = "windows") -> list[tuple[str, object]]:
            choices: list[tuple[str, object]] = []
            choices.append((WHEAT_RELEASE_LABEL, {"release": WHEAT_RELEASE_TAG, "platform": platform}))
            built = detectWheatDll(platform)
            if built:
                choices.append(("Local build (current)", str(built)))
            versionDir = WHEAT_LAUNCHER_DIR / "assets" / "wheat-versions"
            if versionDir.is_dir():
                for dll in sorted(versionDir.glob("*.dll")):
                    if isLoaderForPlatform(dll, platform) and (not built or dll.resolve() != built.resolve()):
                        choices.append((dll.stem, str(dll)))
            return choices

        def cacheReleaseWheatDll(self, platform: str) -> Path:
            cacheValue = QStandardPaths.writableLocation(
                QStandardPaths.StandardLocation.AppLocalDataLocation
            )
            cacheRoot = Path(cacheValue) if cacheValue else Path.home() / ".local/share/WheatModLoader/PlasticLauncher"
            assetName = "wheatLinux.dll" if platform == "linux" else "wheatWindows.dll"
            destination = cacheRoot / "wheat-versions" / WHEAT_RELEASE_TAG / assetName
    
            request = urllib.request.Request(WHEAT_RELEASE_API, headers={"User-Agent": "WheatModLoader", "Accept": "application/vnd.github+json"})
            temporary = destination.with_suffix(".dll.download")
            try:
                with urllib.request.urlopen(request, timeout=15) as response:
                    release = json.loads(response.read(2 * 1024 * 1024 + 1).decode("utf-8"))
                if not isinstance(release, dict) or release.get("tag_name") != WHEAT_RELEASE_TAG:
                    raise ValueError("GitHub returned unexpected release metadata.")
                asset = next((item for item in release.get("assets", [])
                              if isinstance(item, dict) and item.get("name") == assetName), None)
                if not asset or not asset.get("browser_download_url", "").startswith("https://github.com/"):
                    raise ValueError(f"The {assetName} asset is missing from the Wheat release.")
                expectedDigest = asset.get("digest", "")
                if destination.is_file() and expectedDigest.startswith("sha256:"):
                    if hashlib.sha256(destination.read_bytes()).hexdigest() == expectedDigest.removeprefix("sha256:"):
                        if isLoaderForPlatform(destination, platform):
                            return destination
            
                destination.parent.mkdir(parents=True, exist_ok=True)
                assetRequest = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "WheatModLoader"})
                with urllib.request.urlopen(assetRequest, timeout=30) as response:
                    data = response.read(32 * 1024 * 1024 + 1)
                if len(data) > 32 * 1024 * 1024:
                    raise ValueError("The release asset exceeded the 32 MiB safety limit.")
                digest = hashlib.sha256(data).hexdigest()
                if expectedDigest.startswith("sha256:") and digest != expectedDigest.removeprefix("sha256:"):
                    raise ValueError("The downloaded DLL does not match GitHub's SHA-256 digest.")
                if not (data.startswith(b"\x7fELF") if platform == "linux" else data.startswith(b"MZ")):
                    raise ValueError(f"The {assetName} asset is not a valid {platform} loader binary.")
                temporary.write_bytes(data)
                os.replace(temporary, destination)
                return destination
            except (OSError, urllib.error.URLError, TimeoutError, ValueError) as exc:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass
                raise RuntimeError(f"Could not fetch the WheatInternal winmm.dll: {exc}") from exc

        def newInstallation(self, profile: dict | None = None):
            dialog = QDialog(self)
            dialog.setWindowTitle("New Wheat Installation from Profile" if profile else "New Wheat Installation")
            dialog.setMinimumWidth(540)
            form = QFormLayout(dialog)
            defaultName = str(profile.get("name", "Wheat")) + " (Profile)" if profile else f"Wheat {len(self.installations) + 1}"
            nameEdit = QLineEdit(defaultName)
            form.addRow("Name:", nameEdit)
    
            platformCombo = NoWheelComboBox()
            if sys.platform.startswith("linux"):
                platformCombo.addItem("Windows (Proton)", "windows")
                platformCombo.addItem("Linux (native)", "linux")
            else:
                platformCombo.addItem("Windows", "windows")
            if profile:
                profilePlatformIndex = platformCombo.findData(profile.get("cs2_platform"))
                if profilePlatformIndex >= 0:
                    platformCombo.setCurrentIndex(profilePlatformIndex)
            form.addRow("CS2 Platform:", platformCombo)
    
            wheatCombo = NoWheelComboBox()
            browseVersion = QPushButton("Choose DLL…")
            versionRow = QHBoxLayout()
            versionRow.addWidget(wheatCombo, 1)
            versionRow.addWidget(browseVersion)
            form.addRow("Wheat Version:", versionRow)
            installButtonRef = {"button": None}
            customVersion: dict[str, str] = {}
            def chooseWheatDll():
                path, _ = QFileDialog.getOpenFileName(dialog, "Choose Wheat winmm.dll", str(Path.home()), "DLL files (winmm.dll *.dll)")
                if path:
                    customVersion["path"] = path
                    index = wheatCombo.findData(path)
                    if index < 0:
                        wheatCombo.addItem(Path(path).stem + " (custom)", path)
                        index = wheatCombo.count() - 1
                    wheatCombo.setCurrentIndex(index)
            browseVersion.clicked.connect(chooseWheatDll)
    
            cs2Combo = NoWheelComboBox()
            steamcmdAvailable = self.steamcmdPath() is not None
            vanilla = self.gameDir()
            form.addRow("CS2 Version / Depot:", cs2Combo)
            sourceHint = QLabel()
            sourceHint.setWordWrap(True)
            sourceHint.setObjectName("hint")
            form.addRow("", sourceHint)
    
            steamcmdNote = None
            if steamcmdAvailable:
                steamcmdNote = QLabel("SteamCMD installs the selected platform's depots. Linux uses the shared 64-bit depot together with the Linux 64-bit depot.")
                steamcmdNote.setWordWrap(True)
                form.addRow("", steamcmdNote)
    
            separateCheck = QCheckBox("Full Installation")
            separateCheck.setChecked(True)
            separateCheck.setToolTip("Keep a full copy of the CS2 depot files in this installation.")
            form.addRow("", separateCheck)
            linkCheck = QCheckBox("Link files from Vanilla")
            linkCheck.setToolTip("Use symlinks for files available in the vanilla CS2 installation.")
            form.addRow("", linkCheck)
            steamEntryCheck = QCheckBox("Add Steam entry when installation finishes")
            steamEntryCheck.setChecked(True)
            steamEntryCheck.setToolTip("Steam must be fully exited when the installation finishes. You can also add the entry later from the installation's right-click menu.")
            steamEntryRow = QHBoxLayout()
            steamEntryInfo = QLabel("ⓘ")
            steamEntryInfo.setToolTip("The Steam entry is important for Wheat to work because it launches CS2 through Steam with the required game context.")
            steamEntryRow.addWidget(steamEntryCheck)
            steamEntryRow.addWidget(steamEntryInfo)
            steamEntryRow.addStretch(1)
            form.addRow("", steamEntryRow)
            vulkanLayerCheck = QCheckBox("Enable experimental Linux Vulkan overlay layer")
            vulkanLayerCheck.setChecked(bool(profile and profile.get("vulkan_layer_enabled", False)))
            vulkanLayerCheck.setToolTip("Opt in to the Vulkan frame callback and ImGui overlay layer. This is experimental; the safe timer fallback stays enabled until the layer sees frames.")
            form.addRow("", vulkanLayerCheck)
            separateCheck.toggled.connect(lambda full: linkCheck.setChecked(not full))
            linkCheck.toggled.connect(lambda linked: separateCheck.setChecked(not linked))
            linkMode = {"active": False, "platform": None, "source": None, "loader": None, "added_platform": False, "temporary_platform": None}
            def updateInstallButton(*_):
                button = installButtonRef["button"]
                if button:
                    button.setEnabled(cs2Combo.count() > 0 and wheatCombo.count() > 0)

            def updateVulkanLayerAvailability(*_):
                platform = platformCombo.currentData()
                layerChoice = wheatCombo.currentData()
                layerPath = Path(layerChoice).with_name("libwheatVulkanLayer.so") if isinstance(layerChoice, str) else None
                canUseLayer = platform == "linux" and layerPath is not None and layerPath.is_file()
                hasLocalLayerBuild = platform == "linux" and any(
                    isinstance(wheatCombo.itemData(index), str)
                    and Path(wheatCombo.itemData(index)).with_name("libwheatVulkanLayer.so").is_file()
                    for index in range(wheatCombo.count())
                )
                vulkanLayerCheck.setVisible(platform == "linux")
                vulkanLayerCheck.setEnabled(hasLocalLayerBuild)
                vulkanLayerCheck.setToolTip(
                    "Opt in to the separate Vulkan layer for render-thread callbacks. If needed, this switches Wheat Version to the matching local build."
                    if hasLocalLayerBuild else "Build the Linux loader and libwheatVulkanLayer.so together to enable this option."
                )
                if not hasLocalLayerBuild:
                    vulkanLayerCheck.setChecked(False)

            def selectVulkanLayerBuild(enabled: bool):
                if not enabled or platformCombo.currentData() != "linux":
                    return
                selected = wheatCombo.currentData()
                if isinstance(selected, str) and Path(selected).with_name("libwheatVulkanLayer.so").is_file():
                    return
                for index in range(wheatCombo.count()):
                    candidate = wheatCombo.itemData(index)
                    if isinstance(candidate, str) and Path(candidate).with_name("libwheatVulkanLayer.so").is_file():
                        wheatCombo.setCurrentIndex(index)
                        return
                vulkanLayerCheck.setChecked(False)
    
            def updateOptions(*_):
                platform = platformCombo.currentData()
                wheatCombo.clear()
                for label, path in self.wheatVersionChoices(platform):
                    wheatCombo.addItem(label, path)
                cs2Combo.clear()
                if steamcmdAvailable:
                    cs2Combo.addItem(f"Latest public {platform.title()} version (SteamCMD)", "latest")
                vanillaMatches = gameExecutable(vanilla, platform).is_file()
                if vanillaMatches:
                    build = buildIdFor(vanilla) or "installed"
                    cs2Combo.addItem(f"Current vanilla version · build {build}", "vanilla")
                sourceHint.setText("" if cs2Combo.count() else
                                    f"No {platform} CS2 source is available. Install SteamCMD or point Vanilla game to a matching {platform} installation.")
                updateLinkRequirement()
                updateInstallButton()
                updateVulkanLayerAvailability()
    
            def updateLinkRequirement(*_):
                if linkMode["active"]:
                    return
                platform = platformCombo.currentData()
                vanillaMatches = gameExecutable(vanilla, platform).is_file()
                needsSeparate = cs2Combo.currentData() == "latest" and not vanillaMatches
                hasVanilla = gamePlatform(vanilla) is not None
                if needsSeparate:
                    separateCheck.setChecked(True)
                    separateCheck.setEnabled(False)
                    separateCheck.setToolTip(
                        f"Required because no matching {platform} vanilla installation is available."
                    )
                else:
                    separateCheck.setEnabled(True)
                    separateCheck.setToolTip("Keep a full copy of the game files in this installation instead of linking missing files from vanilla CS2.")
                linkCheck.setEnabled(hasVanilla)
                linkCheck.setToolTip("Use symlinks for the matching Vanilla CS2 files. No CS2 depot download will be performed." if hasVanilla
                                      else "A detectable Vanilla CS2 installation is required for linking.")
    
            def setLinkMode(linked: bool):
                if linked:
                    vanillaPlatform = gamePlatform(vanilla)
                    if not vanillaPlatform:
                        linkCheck.setChecked(False)
                        return
                    if not linkMode["active"]:
                        linkMode["platform"] = platformCombo.currentData()
                        linkMode["source"] = cs2Combo.currentData()
                        linkMode["loader"] = wheatCombo.currentData()
                    index = platformCombo.findData(vanillaPlatform)
                    if index < 0:
                        platformCombo.addItem("Linux (native)" if vanillaPlatform == "linux" else "Windows", vanillaPlatform)
                        index = platformCombo.count() - 1
                        linkMode["added_platform"] = True
                        linkMode["temporary_platform"] = vanillaPlatform
                    platformCombo.setCurrentIndex(index)
                    sourceIndex = cs2Combo.findData("vanilla")
                    if sourceIndex < 0:
                        updateOptions()
                        sourceIndex = cs2Combo.findData("vanilla")
                    if sourceIndex < 0:
                        linkCheck.setChecked(False)
                        return
                    cs2Combo.setCurrentIndex(sourceIndex)
                    platformCombo.setItemText(index, "Determined by Vanilla")
                    cs2Combo.setItemText(sourceIndex, "Determined by Vanilla")
                    platformCombo.setEnabled(False)
                    cs2Combo.setEnabled(False)
                    if steamcmdNote:
                        steamcmdNote.setVisible(False)
                    linkMode["active"] = True
                else:
                    if not linkMode["active"]:
                        return
                    platformCombo.setEnabled(True)
                    cs2Combo.setEnabled(True)
                    for index in range(platformCombo.count()):
                        value = platformCombo.itemData(index)
                        label = ("Windows (Proton)" if sys.platform.startswith("linux") else "Windows") if value == "windows" else "Linux (native)"
                        platformCombo.setItemText(index, label)
                    platformIndex = platformCombo.findData(linkMode["platform"])
                    if platformIndex >= 0:
                        platformCombo.setCurrentIndex(platformIndex)
                    updateOptions()
                    loaderIndex = wheatCombo.findData(linkMode["loader"])
                    if loaderIndex >= 0:
                        wheatCombo.setCurrentIndex(loaderIndex)
                    sourceIndex = cs2Combo.findData(linkMode["source"])
                    if sourceIndex >= 0:
                        cs2Combo.setCurrentIndex(sourceIndex)
                    if linkMode["added_platform"]:
                        temporaryIndex = platformCombo.findData(linkMode["temporary_platform"])
                        if temporaryIndex >= 0:
                            platformCombo.removeItem(temporaryIndex)
                    linkMode["active"] = False
                    linkMode["platform"] = None
                    linkMode["source"] = None
                    linkMode["loader"] = None
                    linkMode["added_platform"] = False
                    linkMode["temporary_platform"] = None
                    if steamcmdNote:
                        steamcmdNote.setVisible(True)
                    updateLinkRequirement()
    
            linkCheck.toggled.connect(setLinkMode)
    
            platformCombo.currentIndexChanged.connect(updateOptions)
            wheatCombo.currentIndexChanged.connect(updateVulkanLayerAvailability)
            vulkanLayerCheck.toggled.connect(selectVulkanLayerBuild)
            cs2Combo.currentIndexChanged.connect(updateLinkRequirement)
            updateOptions()
            if profile:
                requestedSource = profile.get("cs2_source")
                sourceIndex = cs2Combo.findData(requestedSource)
                if sourceIndex >= 0:
                    cs2Combo.setCurrentIndex(sourceIndex)
                requestedLoader = str(profile.get("wheat_dll", ""))
                loaderIndex = wheatCombo.findData(requestedLoader) if requestedLoader else -1
                if loaderIndex < 0 and profile.get("wheat_version"):
                    loaderIndex = wheatCombo.findText(str(profile["wheat_version"]))
                if loaderIndex >= 0:
                    wheatCombo.setCurrentIndex(loaderIndex)
                if profile.get("vulkan_layer_enabled") and vulkanLayerCheck.isEnabled():
                    vulkanLayerCheck.setChecked(True)
                if linkCheck.isEnabled():
                    separateCheck.setChecked(bool(profile.get("separate", True)))
    
            defaultRoot = Path.home() / "Games" / "Wheat" / nameEdit.text()
            pathEdit = QLineEdit(str(defaultRoot))
            browsePath = QPushButton("Browse…")
            pathRow = QHBoxLayout()
            pathRow.addWidget(pathEdit, 1)
            pathRow.addWidget(browsePath)
            form.addRow("Install Directory:", pathRow)
            pathTouched = {"value": False}
            pathEdit.textEdited.connect(lambda *_: pathTouched.__setitem__("value", True))
            nameEdit.textChanged.connect(lambda name: not pathTouched["value"] and pathEdit.setText(str(Path.home() / "Games" / "Wheat" / (name or "Wheat"))))
            def chooseInstallParent():
                parent = QFileDialog.getExistingDirectory(dialog, "Choose Installation Parent", str(Path.home()))
                if parent:
                    pathEdit.setText(str(Path(parent) / (nameEdit.text().strip() or "Wheat")))
                    pathTouched["value"] = True
            browsePath.clicked.connect(chooseInstallParent)
    
            buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
            buttons.button(QDialogButtonBox.StandardButton.Ok).setText("Install")
            installButtonRef["button"] = buttons.button(QDialogButtonBox.StandardButton.Ok)
            sizeLabel = QLabel()
            sizeLabel.setObjectName("hint")
            sizeLabel.setToolTip("Measured from the matching vanilla CS2 files on this computer. SteamCMD versions use that install as an estimate.")
            measuredSizes: dict[str, int] = {}
            def updateSizeLabel(*_):
                platform = str(platformCombo.currentData())
                sourceChoice = cs2Combo.currentData()
                isLinked = not separateCheck.isChecked()
                if sourceChoice == "vanilla" and isLinked:
                    sizeLabel.setText("CS2 files to install: 0 B (linked from Vanilla)")
                    sizeLabel.setToolTip("Vanilla CS2 game files are symlinked into this installation and do not use additional game-file storage.")
                    return
                if sourceChoice == "latest" and isLinked:
                    sizeLabel.setText("CS2 files to install: determined by SteamCMD")
                    sizeLabel.setToolTip("SteamCMD downloads depot files, then missing files are linked from Vanilla. Their exact sizes are known after depot resolution.")
                    return
                if not gameExecutable(vanilla, platform).is_file():
                    sizeLabel.setText("CS2 files to install: unavailable")
                    sizeLabel.setToolTip("A matching vanilla CS2 installation is needed to estimate the full installation size.")
                    return
                if platform not in measuredSizes:
                    measuredSizes[platform] = directorySizeBytes(vanilla)
                measured = formatSizeBytes(measuredSizes[platform])
                basis = "measured" if sourceChoice == "vanilla" else "estimated from Vanilla"
                sizeLabel.setText(f"CS2 files to install: {measured} ({basis})")
                sizeLabel.setToolTip("Full game-file size measured from the matching vanilla installation; SteamCMD versions use it as an estimate.")
            platformCombo.currentIndexChanged.connect(updateSizeLabel)
            cs2Combo.currentIndexChanged.connect(updateSizeLabel)
            separateCheck.toggled.connect(updateSizeLabel)
            linkCheck.toggled.connect(updateSizeLabel)
            updateSizeLabel()
            wheatCombo.currentIndexChanged.connect(updateInstallButton)
            cs2Combo.currentIndexChanged.connect(updateInstallButton)
            updateInstallButton()
            buttons.accepted.connect(dialog.accept)
            buttons.rejected.connect(dialog.reject)
            installRow = QWidget()
            installRowLayout = QHBoxLayout(installRow)
            installRowLayout.setContentsMargins(0, 0, 0, 0)
            installRowLayout.addStretch(1)
            installRowLayout.addWidget(sizeLabel)
            installRowLayout.addWidget(buttons)
            form.addRow(installRow)
            if dialog.exec() != QDialog.DialogCode.Accepted:
                return
            installName = nameEdit.text().strip()
            target = Path(pathEdit.text()).expanduser()
            versionChoice = wheatCombo.currentData()
            releaseChoice = isinstance(versionChoice, dict)
            versionDll = "" if releaseChoice else str(versionChoice or "")
            platform = str(platformCombo.currentData())
            if versionDll and not isLoaderForPlatform(Path(versionDll), platform):
                QMessageBox.warning(self, "Wrong runtime format", f"Choose a {platform} Wheat runtime. Linux needs an ELF preload library; Windows needs a PE DLL.")
                return
            if not installName or (not releaseChoice and not versionDll) or not pathEdit.text().strip():
                QMessageBox.warning(self, "Missing information", "Enter an installation name, choose a Wheat DLL, and set an install directory.")
                return
            if target.exists():
                QMessageBox.warning(self, "Directory already exists", "Choose a new, empty directory for this installation.")
                return
            source = self.gameDir().resolve()
            if target.resolve() == source or isInside(target, source) or isInside(source, target):
                QMessageBox.warning(self, "Invalid directory", "The Wheat installation must be outside the vanilla Steam game folder.")
                return
            if cs2Combo.currentData() == "vanilla" and separateCheck.isChecked():
                answer = QMessageBox.question(self, "Copy the full game?", "A separate installation copies the full CS2 game and can use a large amount of disk space. Continue?", QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel)
                if answer != QMessageBox.StandardButton.Yes:
                    return
            if releaseChoice:
                self.statusLabel.setText(f"Downloading and verifying {WHEAT_RELEASE_LABEL} ({platform})…")
                try:
                    versionDll = str(self.cacheReleaseWheatDll(platform))
                except RuntimeError as exc:
                    QMessageBox.critical(self, "Wheat release unavailable", str(exc))
                    self.refreshInstallations()
                    return
            installation = {
                "id": uuid.uuid4().hex,
                "name": installName,
                "path": str(target.resolve()),
                "cs2_version": cs2Combo.currentText(),
                "cs2_source": cs2Combo.currentData(),
                "cs2_platform": platform,
                "wheat_version": wheatCombo.currentText(),
                "wheat_dll": versionDll,
                "vulkan_layer": str(Path(versionDll).with_name("libwheatVulkanLayer.so")) if vulkanLayerCheck.isChecked() else "",
                "vulkan_layer_enabled": vulkanLayerCheck.isChecked(),
                "separate": separateCheck.isChecked(),
                "add_steam_entry": steamEntryCheck.isChecked(),
                "status": "Installing",
            }
            if profile:
                installation["profile_api_allow_list"] = [
                    name for name in profile.get("api_allow_list", [])
                    if name in {"console.get", "console.default", "console.allowed", "console.available", "data",
                                "imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line", "imgui.image"}
                ] if isinstance(profile.get("api_allow_list", []), list) else ["console.get", "console.default", "console.allowed", "console.available", "data",
                                                                                 "imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line", "imgui.image"]
                rawMods = profile.get("mods", {}) if isinstance(profile.get("mods", {}), dict) else {}
                safeModName = lambda name: isinstance(name, str) and bool(name) and name not in (".", "..") and "/" not in name and "\\" not in name
                installation["profile_mods"] = {
                    state: [name for name in rawMods.get(state, []) if safeModName(name)]
                    if isinstance(rawMods.get(state, []), list) else []
                    for state in ("enabled", "disabled")
                }
                installation["profile_mods_source_installation"] = str(profile.get("mods_source_installation", ""))
                installation["profile_platform"] = str(profile.get("cs2_platform", platform))
            self.installations.append(installation)
            self.SelectedInstallationId = installation["id"]
            self.refreshInstallations()
            self.saveSettings()
            if installation["cs2_source"] == "vanilla":
                self.installFromVanilla(installation)
            else:
                self.startSteamcmdInstall(installation)

        def installFromVanilla(self, installation: dict):
            source = self.gameDir()
            target = Path(installation["path"])
            sourceBin = gameBinFor(source, installation.get("cs2_platform", "windows"))
            if (sourceBin / "winmm.dll").exists() or (sourceBin / "wheat").exists():
                QMessageBox.warning(self, "Vanilla install is not clean", "Remove Wheat files from vanilla before creating an installation from it.")
                self.installations.remove(installation)
                self.refreshInstallations()
                self.saveSettings()
                return
            try:
                target.mkdir(parents=True, exist_ok=False)
                if installation["separate"]:
                    shutil.copytree(source, target, symlinks=False, dirs_exist_ok=True)
                else:
                    linkMissingFiles(source, target)
                self.completeInstallation(installation, buildIdFor(source))
            except OSError as exc:
                installation["status"] = "Failed"
                self.log.appendPlainText(f"Installation failed: {exc}")
                QMessageBox.critical(self, "Installation failed", str(exc))
                self.refreshInstallations()

        def startSteamcmdInstall(self, installation: dict):
            steamcmd = self.steamcmdPath()
            if not steamcmd:
                installation["status"] = "Failed"
                self.log.appendPlainText(
                    f"SteamCMD was not found for {installation.get('name')}. The installation record was kept for retry."
                )
                self.saveSettings()
                self.refreshInstallations()
                QMessageBox.warning(
                    self, "SteamCMD not found",
                    "SteamCMD is not installed or its executable could not be found. This installation was kept in the list. After installing SteamCMD, right-click this installation and choose Repair / Update CS2 with SteamCMD.",
                )
                return
            if self.process.state() != QProcess.ProcessState.NotRunning:
                return
            target = Path(installation["path"])
            try:
                target.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                installation["status"] = "Failed"
                self.log.appendPlainText(f"Could not create installation directory: {exc}")
                QMessageBox.critical(self, "Installation failed", str(exc))
                self.refreshInstallations()
                return
            platform = installation.get("cs2_platform", "windows")
            args = ["+@ShutdownOnFailedCommand", "1", "+@NoPromptForPassword", "1",
                    "+@sSteamCmdForcePlatformType", platform, "+force_install_dir", str(target),
                    "+login", "anonymous", "+app_update", APP_ID, "validate", "+quit"]
            self.ActiveInstallationId = installation["id"]
            self.Downloading = True
            self.SteamcmdOutput = ""
            self.installLog.clear()
            self.installLog.setVisible(False)
            self.toggleInstallLogButton.setText("Show Logs")
            self.installDialog.resize(560, 230)
            self.installTitle.setText(f"Installing {installation['name']}")
            self.installDetail.setText("Starting SteamCMD…")
            self.installProgress.setRange(0, 0)
            self.installProgress.setFormat("Downloading…")
            self.ProcessPaused = False
            self.PausedProcessIds = []
            self.CancelRequested = False
            self.CancelProcessIds = []
            self.pauseInstallButton.setText("Pause")
            self.installDialog.show()
            self.installDialog.raise_()
            self.installDialog.activateWindow()
            installation["status"] = "Installing"
            self.log.appendPlainText(f"Installing {installation['name']} from the latest public {platform} CS2 version with SteamCMD.")
            self.process.setProcessEnvironment(QProcessEnvironment.systemEnvironment())
            self.process.setWorkingDirectory(str(target))
            setsid = shutil.which("setsid") if sys.platform.startswith("linux") else None
            if setsid:
                self.process.start(setsid, ["--", str(steamcmd), *args])
            else:
                self.process.start(str(steamcmd), args)
            self.refreshInstallations()

        def completeInstallation(self, installation: dict, sourceBuild: str | None = None):
            target = Path(installation["path"])
            platform = installation.get("cs2_platform", "windows")
            exe = gameExecutable(target, platform)
            if not exe.is_file():
                raise FileNotFoundError(f"CS2 executable was not found after install: {exe}")
            if not installation.get("separate") and installation.get("cs2_source") == "latest":
                vanilla = self.gameDir()
                if not gameExecutable(vanilla, platform).is_file():
                    raise ValueError(f"Link files from Vanilla requires a {platform} CS2 source.")
                linked = linkMissingFiles(vanilla, target)
                self.log.appendPlainText(f"Linked {linked} game files missing from the latest install to vanilla CS2.")
            loader = Path(installation["wheat_dll"])
            proxy = gameBinFor(target, platform) / "winmm.dll"
            proxy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(loader, proxy)
            if installation.get("vulkan_layer_enabled"):
                layerSource = Path(installation.get("vulkan_layer", ""))
                if not layerSource.is_file():
                    raise FileNotFoundError(f"Linux Vulkan layer was not found: {layerSource}")
                layerDir = proxy.parent / "wheat" / "vulkan"
                layerDir.mkdir(parents=True, exist_ok=True)
                layerLibrary = layerDir / "libwheatVulkanLayer.so"
                shutil.copy2(layerSource, layerLibrary)
                layerManifest = {
                    "file_format_version": "1.1.0",
                    "layers": [{
                        "name": "VK_LAYER_WHEAT_modloader",
                        "type": "INSTANCE",
                        "library_path": str(layerLibrary.resolve()),
                        "api_version": "1.3.0",
                        "implementation_version": "1",
                    "description": "Wheat Linux frame callbacks and ImGui overlay",
                        "disable_environment": {"WHEAT_DISABLE_VULKAN_LAYER": "1"},
                        "functions": {"vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"},
                    }],
                }
                (layerDir / "VK_LAYER_WHEAT_modloader.json").write_text(
                    json.dumps(layerManifest, indent=2) + "\n", encoding="utf-8")
            (proxy.parent / "wheat/mods").mkdir(parents=True, exist_ok=True)
            self.applyInstallProfile(installation, platform)
            marker = {
                "app_id": APP_ID,
                "download_complete": True,
                "source_build_id": sourceBuild or buildIdFor(target),
                "cs2_version": installation.get("cs2_version"),
                "cs2_platform": platform,
                "vulkan_layer_enabled": bool(installation.get("vulkan_layer_enabled", False)),
                "wheat_version": installation.get("wheat_version"),
                "separate": bool(installation.get("separate")),
                "installation_id": installation["id"],
                "assembled_at_utc": datetime.now(timezone.utc).isoformat(),
            }
            (target / MARKER_NAME).write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
            installation["status"] = "Ready"
            self.log.appendPlainText(f"Installed {installation['name']}; Wheat launches use -insecure.")
            self.saveSettings()
            self.refreshInstallations()
            if installation.get("add_steam_entry"):
                self.addInstallationToSteam(installation)

        def applyInstallProfile(self, installation: dict, platform: str):
            wheat = gameBinFor(Path(installation["path"]), platform) / "wheat"
            wheat.mkdir(parents=True, exist_ok=True)
            allowed = {"console.get", "console.default", "console.allowed", "console.available", "data",
                       "imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line", "imgui.image"}
            if "profile_api_allow_list" in installation:
                api = [item for item in installation.get("profile_api_allow_list", []) if item in allowed]
            else:
                api = ["console.get", "console.default", "console.allowed", "console.available", "data",
                       "imgui.begin", "imgui.text", "imgui.button", "imgui.finish", "imgui.separator", "imgui.same_line", "imgui.image"]
            (wheat / "apiAllowList.txt").write_text("".join(item + "\n" for item in api), encoding="utf-8")
            if "profile_api_allow_list" not in installation:
                return
            sourceRoot = Path(installation.get("profile_mods_source_installation", "")).expanduser()
            sourcePlatform = installation.get("profile_platform", platform)
            sourceWheat = gameBinFor(sourceRoot, sourcePlatform) / "wheat"
            if sourcePlatform != platform or not sourceWheat.is_dir():
                if installation.get("profile_mods", {}).get("enabled") or installation.get("profile_mods", {}).get("disabled"):
                    self.log.appendPlainText("Install profile API permissions were applied, but its mod source is unavailable or uses another platform.")
                return
            copied = 0
            for state, sourceName, destinationName in (("enabled", "mods", "mods"), ("disabled", "mods-disabled", "mods-disabled")):
                sourceDir = sourceWheat / sourceName
                destinationDir = wheat / destinationName
                destinationDir.mkdir(parents=True, exist_ok=True)
                for modName in installation.get("profile_mods", {}).get(state, []):
                    sourceMod = sourceDir / modName
                    destinationMod = destinationDir / modName
                    try:
                        if sourceMod.is_dir() and not sourceMod.is_symlink() and not destinationMod.exists():
                            shutil.copytree(sourceMod, destinationMod, symlinks=True)
                            copied += 1
                    except OSError as exc:
                        self.log.appendPlainText(f"Could not copy profile mod {modName}: {exc}")
            self.log.appendPlainText(f"Applied install profile API permissions and copied {copied} mod(s).")
