from .common import *


class SteamCmdMixin:
        def readProcessOutput(self):
            data = bytes(self.process.readAllStandardOutput()).decode(errors="replace")
            if data:
                if self.Downloading:
                    self.SteamcmdOutput = (self.SteamcmdOutput + data)[-2_000_000:]
                self.displayProcessData(data)

        def readProcessError(self):
            data = bytes(self.process.readAllStandardError()).decode(errors="replace")
            if data:
                if self.Downloading:
                    self.SteamcmdOutput = (self.SteamcmdOutput + data)[-2_000_000:]
                self.displayProcessData(data)

        def displayProcessData(self, data: str):
            clean = re.sub(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))", "", data)
            clean = clean.replace("\r\n", "\n").replace("\r", "\n")
            if clean:
                for view in (self.log, self.installLog):
                    view.insertPlainText(clean)
                    view.ensureCursorVisible()
                lastLine = clean.rstrip().split("\n")[-1] if clean.strip() else ""
                if lastLine:
                    self.statusLabel.setText(lastLine[-500:])
                    self.installDetail.setText(lastLine[-500:])
            progress = re.search(r"progress\s*:\s*(\d+(?:\.\d+)?)", clean, re.IGNORECASE)
            percent = re.search(r"(\d{1,3}(?:\.\d+)?)\s*%", clean)
            value = None
            if progress:
                number = float(progress.group(1))
                value = round(number * 100) if number <= 1 else round(number)
            elif percent:
                value = round(float(percent.group(1)))
            if value is not None and 0 <= value <= 100:
                self.installProgress.setRange(0, 100)
                self.installProgress.setValue(value)
                self.installProgress.setFormat(f"Downloading… {value}%")

        def sendProcessInput(self):
            if self.process.state() == QProcess.ProcessState.NotRunning:
                return
            self.process.write((self.stdinEdit.text() + "\n").encode())
            self.stdinEdit.clear()

        def toggleInstallPause(self):
            if (self.process.state() == QProcess.ProcessState.NotRunning or self.CancelRequested
                    or not sys.platform.startswith("linux")):
                return
            pid = self.SteamcmdPgid or int(self.process.processId())
            if not pid:
                return
            if self.ProcessPaused:
                self.signalInstallProcess(signal.SIGCONT, pid)
                self.PausedProcessIds = []
            else:
                self.PausedProcessIds = self.linuxProcessTree(pid)
                self.signalInstallProcess(signal.SIGSTOP, pid)
            self.ProcessPaused = not self.ProcessPaused
            if self.ProcessPaused:
                self.installDetail.setText("Paused. Click Resume to continue the download.")
                self.installProgress.setFormat("Paused")
            else:
                self.installDetail.setText("Resumed. Waiting for SteamCMD output…")
                self.installProgress.setFormat("Downloading…")
            self.updateActionStates()

        @staticmethod
        def linuxProcessTree(rootPid: int) -> list[int]:
            result: list[int] = []
            pending = [rootPid]
            seen: set[int] = set()
            while pending:
                pid = pending.pop()
                if pid in seen:
                    continue
                seen.add(pid)
                result.append(pid)
                try:
                    childText = Path(f"/proc/{pid}/task/{pid}/children").read_text(encoding="ascii")
                except OSError:
                    continue
                pending.extend(int(child) for child in childText.split() if child.isdigit())
            return result

        @staticmethod
        def signalProcesses(processIds: list[int], requestedSignal) -> None:
            for pid in processIds:
                try:
                    os.kill(pid, requestedSignal)
                except (ProcessLookupError, PermissionError):
                    continue

        def signalInstallProcess(self, requestedSignal, fallbackPid: int | None = None):
            pgid = self.SteamcmdPgid
            if pgid and sys.platform.startswith("linux"):
                try:
                    os.killpg(pgid, requestedSignal)
                    return
                except (ProcessLookupError, PermissionError, OSError):
                    pass
            pid = fallbackPid or int(self.process.processId())
            if pid:
                tree = self.PausedProcessIds or self.linuxProcessTree(pid)
                self.signalProcesses(list(reversed(tree)), requestedSignal)

        def processStarted(self):
            pid = int(self.process.processId())
            self.SteamcmdPgid = pid if sys.platform.startswith("linux") and shutil.which("setsid") else None
            self.updateActionStates()

        def toggleInstallLogs(self):
            visible = not self.installLog.isVisible()
            self.installLog.setVisible(visible)
            self.toggleInstallLogButton.setText("Hide Logs" if visible else "Show Logs")
            self.installDialog.resize(760, 520) if visible else self.installDialog.resize(560, 230)

        def cancelInstallation(self):
            if self.process.state() == QProcess.ProcessState.NotRunning or self.CancelRequested:
                return
            self.CancelRequested = True
            processIds = self.linuxProcessTree(int(self.process.processId())) if sys.platform.startswith("linux") else []
            self.CancelProcessIds = processIds
            if self.ProcessPaused:
                self.signalInstallProcess(signal.SIGCONT, int(self.process.processId()))
                self.ProcessPaused = False
                self.PausedProcessIds = []
            if self.SteamcmdPgid:
                try:
                    os.killpg(self.SteamcmdPgid, signal.SIGTERM)
                except (ProcessLookupError, PermissionError, OSError):
                    pass
            elif processIds:
                self.signalProcesses(list(reversed(processIds)), signal.SIGTERM)
            self.installTitle.setText("Cancelling installation…")
            self.installDetail.setText("Cancelling…")
            self.installProgress.setRange(0, 0)
            self.installProgress.setFormat("Cancelling…")
            self.process.terminate()
            QTimer.singleShot(3000, self.killProcessIfCancelling)
            self.updateActionStates()

        def killProcessIfCancelling(self):
            if not self.CancelRequested:
                return
            if self.SteamcmdPgid:
                try:
                    os.killpg(self.SteamcmdPgid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError, OSError):
                    pass
            elif self.CancelProcessIds:
                self.signalProcesses(list(reversed(self.CancelProcessIds)), signal.SIGKILL)
            if self.process.state() != QProcess.ProcessState.NotRunning:
                self.process.kill()

        def processError(self, error):
            self.log.appendPlainText(f"SteamCMD process error: {error}")
            self.installLog.appendPlainText(f"\nSteamCMD process error: {error}")
            if error == QProcess.ProcessError.FailedToStart:
                if self.Downloading:
                    installation = self.installationById(self.ActiveInstallationId)
                    if installation:
                        installation["status"] = "Failed"
                        self.installTitle.setText(f"Could not start SteamCMD · {installation.get('name')}")
                        self.installProgress.setRange(0, 1)
                        self.installProgress.setValue(0)
                        self.installProgress.setFormat("Could not start SteamCMD")
                self.Downloading = False
                self.refreshInstallations()

        def processFinished(self, exitCode: int, ExitStatus):
            self.readProcessOutput()
            self.readProcessError()
            if self.CancelRequested and self.SteamcmdPgid:
                try:
                    os.killpg(self.SteamcmdPgid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError, OSError):
                    pass
            elif self.CancelRequested and self.CancelProcessIds:
                self.signalProcesses(list(reversed(self.CancelProcessIds)), signal.SIGKILL)
            self.SteamcmdPgid = None
            if not self.Downloading:
                self.updateActionStates()
                return
            installation = self.installationById(self.ActiveInstallationId)
            self.Downloading = False
            if self.CancelRequested:
                self.CancelRequested = False
                self.CancelProcessIds = []
                self.ProcessPaused = False
                if installation:
                    installation["status"] = "Cancelled"
                    self.saveSettings()
                self.installTitle.setText(f"Installation cancelled · {installation.get('name') if installation else ''}")
                self.installDetail.clear()
                self.installProgress.setRange(0, 1)
                self.installProgress.setValue(0)
                self.installProgress.setFormat("Cancelled")
                self.log.appendPlainText("SteamCMD installation cancelled; partial files were left in place.")
                self.refreshInstallations()
                return
            if not installation:
                self.refreshInstallations()
                return
            exe = gameExecutable(Path(installation["path"]), installation.get("cs2_platform", "windows"))
            confirmed = re.search(
                r"success!\s*app\s*['\"]730['\"]\s*(?:fully installed|already up to date)",
                self.SteamcmdOutput,
                re.IGNORECASE,
            )
            if exitCode != 0 or not confirmed or not exe.is_file():
                installation["status"] = "Failed"
                reason = "SteamCMD did not confirm that app 730 finished installing."
                if exitCode != 0:
                    reason = f"SteamCMD exited with code {exitCode}."
                self.log.appendPlainText(f"{reason} Wheat was not installed; the partial files were left in place for inspection.")
                self.installLog.appendPlainText(f"\n{reason} Partial files were left in place for inspection.")
                self.installTitle.setText(f"Installation failed · {installation.get('name')}")
                self.installProgress.setRange(0, 1)
                self.installProgress.setValue(0)
                self.installProgress.setFormat("Installation failed")
                self.statusLabel.setText(f"Installation failed · {installation.get('name')} · see Activity for details")
                self.saveSettings()
                self.refreshInstallations()
                return
            try:
                self.completeInstallation(installation)
                self.installTitle.setText(f"Installation complete · {installation.get('name')}")
                self.installProgress.setRange(0, 100)
                self.installProgress.setValue(100)
                self.installProgress.setFormat("Complete")
            except (OSError, ValueError) as exc:
                installation["status"] = "Failed"
                self.log.appendPlainText(f"Installation assembly failed: {exc}")
                self.installLog.appendPlainText(f"\nInstallation assembly failed: {exc}")
                self.installTitle.setText(f"Assembly failed · {installation.get('name')}")
                self.installProgress.setRange(0, 1)
                self.installProgress.setValue(0)
                self.installProgress.setFormat("Installation failed")
                QMessageBox.critical(self, "Installation failed", str(exc))
                self.saveSettings()
                self.refreshInstallations()
            self.refreshMods()
            self.refreshRuntimeLog()
