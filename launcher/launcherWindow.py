from .common import *
from .widgets import PathRow, NoWheelComboBox, WorkerSignals, FunctionWorker
from .installationMixin import InstallationMixin
from .modManagementMixin import ModManagementMixin
from .logNewsMixin import LogNewsMixin
from .steamCmdMixin import SteamCmdMixin


class Launcher(InstallationMixin, ModManagementMixin, LogNewsMixin, SteamCmdMixin, QMainWindow):
        def __init__(self):
            super().__init__()
            self.settings = QSettings("WheatModLoader", "PlasticLauncher")
            self.process = QProcess(self)
            self.process.readyReadStandardOutput.connect(self.readProcessOutput)
            self.process.readyReadStandardError.connect(self.readProcessError)
            self.process.finished.connect(self.processFinished)
            self.process.errorOccurred.connect(self.processError)
            self.process.started.connect(self.processStarted)
            self.process.stateChanged.connect(self.updateActionStates)
            self.Downloading = False
            self.ActiveInstallationId = ""
            self.SteamcmdOutput = ""
            self.ProcessPaused = False
            self.PausedProcessIds: list[int] = []
            self.SteamcmdPgid: int | None = None
            self.CancelProcessIds: list[int] = []
            self.CancelRequested = False
            self.LogWindows: dict[str, dict] = {}
            self.NewsLoaded = False
            self.NewsWorker: FunctionWorker | None = None
            self.setWindowTitle("Wheat Launcher")
            self.resize(1200, 700)
            self.setMinimumSize(800, 620)
    
            root = QWidget()
            self.setCentralWidget(root)
            layout = QVBoxLayout(root)
            layout.setContentsMargins(8, 6, 8, 4)
            layout.setSpacing(4)
    
            self.tabs = QTabWidget()
            layout.addWidget(self.tabs, 1)
    
            dashboardPage = QWidget()
            dashboardLayout = QVBoxLayout(dashboardPage)
            dashboardLayout.setContentsMargins(3, 4, 3, 3)
            dashboardLayout.setSpacing(5)
    
            installationsHeader = QHBoxLayout()
            installationsHeader.addWidget(QLabel("Installations"))
            installationsHeader.addStretch(1)
            self.profileInstallButton = QPushButton("New from Profile…")
            self.profileInstallButton.clicked.connect(self.newInstallationFromProfile)
            installationsHeader.addWidget(self.profileInstallButton)
            self.newInstallationButton = QPushButton("New Installation")
            self.newInstallationButton.clicked.connect(self.newInstallation)
            installationsHeader.addWidget(self.newInstallationButton)
            dashboardLayout.addLayout(installationsHeader)
    
            self.installTable = QTableWidget(0, 6)
            self.installTable.setHorizontalHeaderLabels(["Name", "CS2 Platform", "CS2 Version", "Wheat Version", "Status", "Location"])
            self.installTable.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
            self.installTable.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
            self.installTable.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
            self.installTable.setAlternatingRowColors(True)
            self.installTable.verticalHeader().setVisible(False)
            header = self.installTable.horizontalHeader()
            for column in range(5):
                header.setSectionResizeMode(column, QHeaderView.ResizeMode.ResizeToContents)
            header.setSectionResizeMode(5, QHeaderView.ResizeMode.Stretch)
            self.installTable.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            self.installTable.customContextMenuRequested.connect(self.installationContextMenu)
            self.installTable.itemDoubleClicked.connect(lambda *_: self.launchSelected())
            self.installTable.itemSelectionChanged.connect(self.selectedInstallationChanged)
            dashboardLayout.addWidget(self.installTable, 1)
    
            installActions = QHBoxLayout()
            installActions.addStretch(1)
            self.launchSelectedButton = QPushButton("Launch Selected")
            self.launchSelectedButton.clicked.connect(self.launchSelected)
            installActions.addWidget(self.launchSelectedButton)
            dashboardLayout.addLayout(installActions)
            self.statusLabel = QLabel("No Wheat installations yet. Choose New Installation to begin.")
            self.statusLabel.setObjectName("hint")
            self.statusLabel.setWordWrap(True)
            layout.addWidget(self.statusLabel)
    
            settingsPage = QWidget()
            settingsLayout = QVBoxLayout(settingsPage)
            settingsLayout.setContentsMargins(4, 4, 4, 4)
            installGroup = QGroupBox("Install locations")
            installForm = QFormLayout(installGroup)
            self.sourceRow = PathRow("vanilla CS2 folder", "/.../steamapps/common/Counter-Strike Global Offensive", True)
            installForm.addRow("Vanilla game:", self.sourceRow)
            settingsLayout.addWidget(installGroup)
    
            toolsGroup = QGroupBox("Tools")
            toolsForm = QFormLayout(toolsGroup)
            self.steamcmdRow = PathRow("SteamCMD", "/path/to/steamcmd", False)
            self.steamcmdLabel = QLabel("SteamCMD:")
            self.loaderRow = PathRow("Wheat winmm.dll", "/path/to/build/winmm.dll", False)
            toolsForm.addRow(self.steamcmdLabel, self.steamcmdRow)
            toolsForm.addRow("Wheat proxy DLL:", self.loaderRow)
            settingsLayout.addWidget(toolsGroup)
    
            runtimeGroup = QGroupBox("Game runtime")
            runtimeForm = QVBoxLayout(runtimeGroup)
            self.runtimeHint = QLabel(
                "Wheat launches through Steam so Steamworks can connect. Windows installations need a Proton version selected "
                "under Properties → Compatibility. Native Linux installations use the ELF preload runtime directly."
            )
            self.runtimeHint.setObjectName("hint")
            self.runtimeHint.setWordWrap(True)
            runtimeForm.addWidget(self.runtimeHint)
            settingsLayout.addWidget(runtimeGroup)
    
            launcherGroup = QGroupBox("Launcher settings")
            launcherForm = QFormLayout(launcherGroup)
            self.themeCombo = NoWheelComboBox()
            self.themeCombo.addItems(["Light", "Dark"])
            self.themeCombo.currentTextChanged.connect(self.applyThemeSetting)
            self.followLogsCheck = QCheckBox("Refresh runtime logs automatically")
            self.followLogsCheck.toggled.connect(self.setLogFollowing)
            launcherForm.addRow("Appearance:", self.themeCombo)
            launcherForm.addRow("Logs:", self.followLogsCheck)
            settingsLayout.addWidget(launcherGroup)
    
            detectRow = QHBoxLayout()
            detectRow.addStretch(1)
            self.detectPathsButton = QPushButton("Detect paths")
            self.detectPathsButton.clicked.connect(self.detectPaths)
            detectRow.addWidget(self.detectPathsButton)
            settingsLayout.addLayout(detectRow)
            settingsLayout.addStretch(1)
            self.tabs.addTab(dashboardPage, "Launcher")
            self.tabs.addTab(settingsPage, "Settings")
    
            newsPage = QWidget()
            newsLayout = QVBoxLayout(newsPage)
            newsToolbar = QHBoxLayout()
            self.newsStatus = QLabel("Release notes from WheatModLoader releases.")
            self.newsStatus.setObjectName("hint")
            newsToolbar.addWidget(self.newsStatus, 1)
            self.refreshNewsButton = QPushButton("Refresh")
            self.refreshNewsButton.clicked.connect(lambda: self.loadNews(force=True))
            newsToolbar.addWidget(self.refreshNewsButton)
            newsLayout.addLayout(newsToolbar)
            self.newsView = QTextBrowser()
            self.newsView.setOpenExternalLinks(True)
            newsLayout.addWidget(self.newsView, 1)
            self.tabs.addTab(newsPage, "News")
            self.tabs.currentChanged.connect(self.tabChanged)
            self.logTimer = QTimer(self)
            self.logTimer.setInterval(1500)
            self.logTimer.timeout.connect(self.refreshRuntimeLog)
    
            logCard, logLayout = self.card("Activity", "")
            self.log = QPlainTextEdit()
            self.log.setReadOnly(True)
            self.log.setMaximumBlockCount(1500)
            self.log.setMinimumHeight(180)
            self.log.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
            self.log.setFont(QFont("Monospace", 9))
            logLayout.addWidget(self.log)
            inputRow = QHBoxLayout()
            self.stdinEdit = QLineEdit()
            self.stdinEdit.setPlaceholderText("SteamCMD input (not stored)")
            self.stdinEdit.setEchoMode(QLineEdit.EchoMode.Normal)
            self.stdinEdit.returnPressed.connect(self.sendProcessInput)
            self.sendButton = QPushButton("Send")
            self.sendButton.setObjectName("secondaryButton")
            self.sendButton.clicked.connect(self.sendProcessInput)
            inputRow.addWidget(self.stdinEdit, 1)
            inputRow.addWidget(self.sendButton)
            logLayout.addLayout(inputRow)
            activityPage = QWidget()
            self.activityPage = activityPage
            activityLayout = QVBoxLayout(activityPage)
            activityLayout.setContentsMargins(4, 4, 4, 4)
            activityLayout.addWidget(logCard, 1)
            self.tabs.addTab(activityPage, "Activity")
            self.activityPage = activityPage
            self.activityLogCard = logCard
            self.activityInputRow = inputRow
    
            self.installDialog = QDialog(self)
            self.installDialog.setWindowTitle("Wheat Installation")
            self.installDialog.resize(560, 230)
            self.installDialog.setMinimumSize(480, 190)
            installLayout = QVBoxLayout(self.installDialog)
            installLayout.setContentsMargins(10, 10, 10, 10)
            installCard, installCardLayout = self.card("Installing", "")
            self.installTitle = QLabel("Waiting for installation")
            self.installTitle.setObjectName("installTitle")
            installCardLayout.addWidget(self.installTitle)
            self.installDetail = QLabel("SteamCMD output will appear below.")
            self.installDetail.setObjectName("hint")
            self.installDetail.setWordWrap(True)
            installCardLayout.addWidget(self.installDetail)
            self.installProgress = QProgressBar()
            self.installProgress.setRange(0, 0)
            self.installProgress.setFormat("Waiting for SteamCMD…")
            installCardLayout.addWidget(self.installProgress)
            self.installLog = QPlainTextEdit()
            self.installLog.setReadOnly(True)
            self.installLog.setMaximumBlockCount(12000)
            self.installLog.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
            self.installLog.setFont(QFont("Monospace", 9))
            self.installLog.setVisible(False)
            installCardLayout.addWidget(self.installLog, 1)
            installLayout.addWidget(installCard, 1)
            installControls = QHBoxLayout()
            self.toggleInstallLogButton = QPushButton("Show Logs")
            self.toggleInstallLogButton.clicked.connect(self.toggleInstallLogs)
            installControls.addWidget(self.toggleInstallLogButton)
            installControls.addStretch(1)
            self.pauseInstallButton = QPushButton("Pause")
            self.pauseInstallButton.clicked.connect(self.toggleInstallPause)
            installControls.addWidget(self.pauseInstallButton)
            self.cancelInstallButton = QPushButton("Cancel")
            self.cancelInstallButton.clicked.connect(self.cancelInstallation)
            installControls.addWidget(self.cancelInstallButton)
            installLayout.addLayout(installControls)
            self.installations: list[dict] = []
            self.restoreSettings()
            for row in (self.sourceRow, self.steamcmdRow, self.loaderRow):
                row.edit.textChanged.connect(self.updateActionStates)
            geometry = self.settings.value("window_geometry")
            if geometry:
                self.restoreGeometry(geometry)
            restoredTab = int(self.settings.value("selected_tab", 0))
            self.tabs.setCurrentIndex(min(max(restoredTab, 0), self.tabs.count() - 1))
            self.refreshStatus()

        def card(self, heading: str, detail: str) -> tuple[QGroupBox, QVBoxLayout]:
            group = QGroupBox(heading)
            column = QVBoxLayout(group)
            column.setContentsMargins(10, 8, 10, 9)
            column.setSpacing(6)
            if detail:
                hint = QLabel(detail)
                hint.setObjectName("hint")
                hint.setWordWrap(True)
                column.addWidget(hint)
            return group, column

        def restoreSettings(self):
            self.sourceRow.edit.setText(self.settings.value("vanilla", ""))
            self.steamcmdRow.edit.setText(self.settings.value("steamcmd", ""))
            self.loaderRow.edit.setText(self.settings.value("loader", ""))
            if not self.sourceRow.edit.text():
                detected = detectVanillaGame()
                if detected:
                    self.sourceRow.edit.setText(str(detected))
            if not self.steamcmdRow.edit.text():
                detected = detectSteamcmd()
                if detected:
                    self.steamcmdRow.edit.setText(str(detected))
            if not self.loaderRow.edit.text():
                detected = detectWheatDll()
                if detected:
                    self.loaderRow.edit.setText(str(detected))
    
            try:
                saved = json.loads(self.settings.value("installations", "[]"))
            except (TypeError, ValueError):
                saved = []
            if isinstance(saved, list):
                self.installations = [item for item in saved if isinstance(item, dict) and item.get("path")]
            legacyPath = self.settings.value("workspace", "")
            if not self.installations and legacyPath and Path(legacyPath).is_dir():
                markerFile = Path(legacyPath) / MARKER_NAME
                try:
                    marker = json.loads(markerFile.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    marker = {}
                self.installations.append({
                    "id": "legacy-" + str(abs(hash(str(Path(legacyPath).resolve())))),
                    "name": Path(legacyPath).name or "Wheat",
                    "path": str(Path(legacyPath).resolve()),
                    "wheat_version": "Existing installation",
                    "wheat_dll": marker.get("wheat_dll", str(detectWheatDll() or "")),
                    "cs2_version": "Existing CS2 files",
                    "separate": False,
                    "status": "Ready" if marker.get("download_complete") else "Needs repair",
                })
            self.SelectedInstallationId = str(self.settings.value("selected_installation", ""))
            self.ModsInstallationId = self.SelectedInstallationId
            self.themeCombo.setCurrentText(self.settings.value("theme", "Light"))
            applyFleasionStyle(QApplication.instance(), self.themeCombo.currentText())
            self.followLogsCheck.setChecked(self.settings.value("follow_logs", True, type=bool))
            self.refreshInstallations()
            self.refreshMods()
            self.refreshRuntimeLog()

        def saveSettings(self):
            self.settings.setValue("vanilla", self.sourceRow.edit.text())
            self.settings.setValue("steamcmd", self.steamcmdRow.edit.text())
            self.settings.setValue("loader", self.loaderRow.edit.text())
            self.settings.setValue("installations", json.dumps(self.installations))
            self.settings.setValue("selected_installation", self.SelectedInstallationId)
            self.settings.setValue("theme", self.themeCombo.currentText())
            self.settings.setValue("follow_logs", self.followLogsCheck.isChecked())

        def applyThemeSetting(self, theme: str):
            app = QApplication.instance()
            if app:
                applyFleasionStyle(app, theme)

        def steamcmdPath(self) -> Path | None:
            configured = self.steamcmdRow.path()
            if configured.is_file() and (os.name == "nt" or os.access(configured, os.X_OK)):
                return configured
            detected = detectSteamcmd()
            if detected:
                if self.steamcmdRow.edit.text() != str(detected):
                    self.steamcmdRow.edit.setText(str(detected))
                return detected
            return None

        def updateSteamcmdVisibility(self):
            available = self.steamcmdPath() is not None
            self.steamcmdRow.setVisible(available)
            self.steamcmdLabel.setVisible(available)
            self.stdinEdit.setVisible(available)
            self.sendButton.setVisible(available)
            return available

        def detectPaths(self):
            source = detectVanillaGame()
            steamcmd = detectSteamcmd()
            dll = detectWheatDll()
            if source:
                self.sourceRow.edit.setText(str(source))
            if steamcmd:
                self.steamcmdRow.edit.setText(str(steamcmd))
            if dll:
                self.loaderRow.edit.setText(str(dll))
            self.refreshStatus()
            self.refreshMods()
            self.refreshRuntimeLog()
            found = [name for name, path in (("vanilla CS2", source), ("SteamCMD", steamcmd),
                                             ("Wheat DLL", dll)) if path]
            QMessageBox.information(self, "Path detection", "Detected: " + (", ".join(found) if found else "no known paths; enter them manually."))

        def gameDir(self) -> Path:
            return self.sourceRow.path()

        def workspace(self) -> Path:
            installation = self.selectedInstallation() if hasattr(self, "installTable") else None
            return Path(installation["path"]).expanduser() if installation else Path.home() / ".wheat-no-install-selected"

        def markerPath(self) -> Path:
            return self.workspace() / MARKER_NAME

        def readMarker(self) -> dict:
            try:
                data = json.loads(self.markerPath().read_text(encoding="utf-8"))
                return data if isinstance(data, dict) else {}
            except (OSError, ValueError):
                return {}

        def launchSelected(self):
            installation = self.selectedInstallation()
            if not installation:
                QMessageBox.information(self, "No installation selected", "Choose New Installation first.")
                return
            self.launchWheat(installation)

        def showAbout(self):
            QMessageBox.about(
                self,
                "About Wheat Launcher",
                "Wheat Launcher UI layout adapted from Fleasion's ReplacerConfigWindow.\n"
                "Copyright Fleasion contributors; licensed under GNU GPL-3.0.\n"
                "Modified for Wheat Mod Loader. No warranty is provided.\n\n"
                "Source: https://github.com/fleasion/Fleasion/tree/main/src/fleasion/gui\n"
                "See the repository LICENSE for the full license.",
            )

        def refreshStatus(self, *_):
            self.refreshInstallations()

        def updateActionStates(self, *_):
            if not hasattr(self, "installTable"):
                return
            self.updateSteamcmdVisibility()
            busy = self.process.state() != QProcess.ProcessState.NotRunning
            installation = self.selectedInstallation()
            ready = False
            if installation:
                root = Path(installation.get("path", ""))
                try:
                    marker = json.loads((root / MARKER_NAME).read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    marker = {}
                platform = installation.get("cs2_platform", marker.get("cs2_platform", "windows"))
                runtime = gameBinFor(root, platform) / "winmm.dll"
                ready = bool(marker.get("download_complete") and gameExecutable(root, platform).is_file() and runtime.is_file())
            self.newInstallationButton.setEnabled(not busy)
            self.launchSelectedButton.setEnabled(not busy and ready)
            self.detectPathsButton.setEnabled(not busy)
            self.sendButton.setEnabled(busy)
            self.pauseInstallButton.setEnabled(
                busy and bool(self.process.processId()) and sys.platform.startswith("linux") and not self.CancelRequested
            )
            self.pauseInstallButton.setText("Resume" if self.ProcessPaused else "Pause")
            self.pauseInstallButton.setToolTip(
                "Pause and resume suspend the SteamCMD process and its child processes on Linux."
                if sys.platform.startswith("linux") else "Pausing is currently supported on Linux only."
            )
            self.cancelInstallButton.setEnabled(busy and not self.CancelRequested)
            if not installation:
                self.statusLabel.setText("No Wheat installations yet. Choose New Installation to begin.")
            elif not ready:
                self.statusLabel.setText(f"{installation.get('status', 'Incomplete')} · {installation.get('name')} · {installation.get('path')}")

        def launchWheat(self, installation: dict):
            target = Path(installation["path"]).expanduser()
            try:
                marker = json.loads((target / MARKER_NAME).read_text(encoding="utf-8"))
            except (OSError, ValueError):
                marker = {}
            current = buildIdFor(self.gameDir())
            recorded = marker.get("source_build_id")
            if (not installation.get("separate") and installation.get("cs2_source") == "vanilla"
                    and recorded and current and recorded != current):
                QMessageBox.warning(self, "CS2 update required", "The linked vanilla CS2 build changed. Create a new installation from the current game version.")
                self.refreshInstallations()
                return
            platform = installation.get("cs2_platform", marker.get("cs2_platform", "windows"))
            executable = gameExecutable(target, platform)
            proxy = gameBinFor(target, platform) / "winmm.dll"
            if not executable.is_file() or not proxy.is_file() or not marker.get("download_complete"):
                QMessageBox.warning(self, "Installation not ready", "This Wheat installation is incomplete.")
                return
            if not steamClientRunning():
                QMessageBox.warning(
                    self, "Steam is not running",
                    "Start the Steam desktop client and sign in before launching Wheat. Steam must launch the entry to provide its Steamworks session.",
                )
                return
            shortcutId = installation.get("steam_shortcut_appid")
            if not shortcutId:
                QMessageBox.information(
                    self, "Steam entry required",
                    "Right-click this installation and choose Add / Update Steam Entry. Fully exit Steam first, then restart it to load the new entry.",
                )
                return
            expectedExe = os.path.abspath(os.fspath(executable))
            if installation.get("steam_shortcut_exe") != expectedExe:
                QMessageBox.information(
                    self, "Update Steam entry",
                    "This installation's Steam entry may still point to the vanilla game folder. Fully exit Steam, then right-click this installation and choose Add / Update Steam Entry. Restart Steam after updating it.",
                )
                return
            shortcutUri = f"steam://rungameid/{int(shortcutId)}"
            self.saveSettings()
            if not QDesktopServices.openUrl(QUrl(shortcutUri)):
                steamCommand = shutil.which("steam")
                launchResult = QProcess.startDetached(steamCommand, [shortcutUri]) if steamCommand else False
                started = launchResult[0] if isinstance(launchResult, tuple) else launchResult
                if not started:
                    QMessageBox.warning(self, "Could not open Steam entry", f"Steam could not open this entry:\n{shortcutUri}")
                    return
            self.log.appendPlainText(f"Requested Steam to launch {installation['name']} with -insecure.")

        def closeEvent(self, event):
            self.saveSettings()
            self.settings.setValue("window_geometry", self.saveGeometry())
            self.settings.setValue("selected_tab", self.tabs.currentIndex())
            if self.process.state() != QProcess.ProcessState.NotRunning:
                answer = QMessageBox.question(self, "Process running", "SteamCMD is still running. Close the launcher anyway?",
                                             QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
                if answer != QMessageBox.StandardButton.Yes:
                    event.ignore()
                    return
            event.accept()

def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Wheat Launcher")
    applyFleasionStyle(app)
    window = Launcher()
    window.show()
    return app.exec()
