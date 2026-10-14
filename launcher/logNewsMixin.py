from .common import *
from .widgets import FunctionWorker, NoWheelComboBox


class LogNewsMixin:
        def setLogFollowing(self, enabled: bool):
            if enabled:
                self.logTimer.start()
                self.refreshRuntimeLog()
            else:
                self.logTimer.stop()

        def tabChanged(self, index: int):
            if self.tabs.tabText(index) == "News":
                self.loadNews()

        @staticmethod
        def fetchReleaseNews() -> list[dict]:
            request = urllib.request.Request(WHEAT_RELEASES_API, headers={"User-Agent": "WheatModLoader", "Accept": "application/vnd.github+json"})
            with urllib.request.urlopen(request, timeout=15) as response:
                data = response.read(8 * 1024 * 1024 + 1)
            if len(data) > 8 * 1024 * 1024:
                raise ValueError("Release notes response exceeded 8 MiB.")
            releases = json.loads(data.decode("utf-8"))
            if not isinstance(releases, list):
                raise ValueError("GitHub returned an unexpected release list.")
            return [release for release in releases if isinstance(release, dict) and not release.get("draft")]

        def loadNews(self, force: bool = False):
            if self.NewsWorker:
                return
            if self.NewsLoaded and not force:
                return
            self.newsStatus.setText("Loading release notes…")
            self.refreshNewsButton.setEnabled(False)
            worker = FunctionWorker(self.fetchReleaseNews)
            worker.signals.finished.connect(self.displayReleaseNews)
            self.NewsWorker = worker
            QThreadPool.globalInstance().start(worker)

        def displayReleaseNews(self, outcome: dict):
            self.NewsWorker = None
            self.refreshNewsButton.setEnabled(True)
            if "error" in outcome:
                self.newsStatus.setText("Could not load release notes.")
                self.newsView.setPlainText(f"Could not load Wheat release notes.\n\n{outcome['error']}\n\nCheck your connection and use Refresh to try again.")
                return
            releases = outcome.get("result", [])
            if not releases:
                self.newsStatus.setText("No published releases found.")
                self.newsView.clear()
                self.NewsLoaded = True
                return
            sections = []
            for release in releases:
                title = str(release.get("name") or release.get("tag_name") or "Untitled release")
                tag = str(release.get("tag_name") or "")
                url = str(release.get("html_url") or "")
                if not url.startswith("https://github.com/"):
                    url = ""
                published = str(release.get("published_at") or "Date unavailable")
                label = f"{title} ({tag})" if tag and tag not in title else title
                if url:
                    heading = f"## [{label}]({url})"
                else:
                    heading = f"## {label}"
                if release.get("prerelease"):
                    published += " · Prerelease"
                body = str(release.get("body") or "No release notes were included.")
                sections.append(f"{heading}\n\nPublished: {published}\n\n{body}\n\n---")
            self.newsView.setMarkdown("\n\n".join(sections))
            self.newsStatus.setText(f"Showing {len(releases)} published release(s) from GitHub.")
            self.NewsLoaded = True

        def openInstallationLogs(self, installation: dict):
            installId = str(installation.get("id", ""))
            existing = self.LogWindows.get(installId)
            if existing and existing["dialog"].isVisible():
                existing["dialog"].showNormal()
                existing["dialog"].raise_()
                existing["dialog"].activateWindow()
                return
            dialog = QDialog(self)
            dialog.setWindowTitle(f"Logs · {installation.get('name', 'Wheat')}")
            dialog.resize(900, 600)
            dialog.setAttribute(Qt.WidgetAttribute.WADeleteOnClose, True)
            layout = QVBoxLayout(dialog)
            toolbar = QHBoxLayout()
            toolbar.addWidget(QLabel("Log file:"))
            source = NoWheelComboBox()
            source.addItem("loader.log", "loader.log")
            source.addItem("consoleReport.txt", "consoleReport.txt")
            toolbar.addWidget(source)
            pathLabel = QLabel()
            pathLabel.setObjectName("hint")
            toolbar.addWidget(pathLabel, 1)
            refreshButton = QPushButton("Refresh")
            toolbar.addWidget(refreshButton)
            layout.addLayout(toolbar)
            text = QPlainTextEdit()
            text.setReadOnly(True)
            text.setMaximumBlockCount(12000)
            layout.addWidget(text, 1)
            window = {"dialog": dialog, "source": source, "path_label": pathLabel, "text": text}
            self.LogWindows[installId] = window
            source.currentIndexChanged.connect(lambda *_: self.refreshRuntimeLog(installId))
            refreshButton.clicked.connect(lambda: self.refreshRuntimeLog(installId))
            dialog.finished.connect(lambda *_: self.LogWindows.pop(installId, None))
            dialog.show()
            self.refreshRuntimeLog(installId)

        def refreshRuntimeLog(self, installId: str | None = None, *_):
            windows = [(installId, self.LogWindows.get(installId))] if installId else list(self.LogWindows.items())
            for currentId, window in windows:
                if not window or not window["dialog"].isVisible():
                    continue
                installation = self.installationById(currentId)
                if not installation:
                    continue
                filename = window["source"].currentData()
                path = gameBinFor(Path(installation["path"]), installation.get("cs2_platform", "windows")) / "wheat" / filename
                window["path_label"].setText(str(path))
                try:
                    with path.open("rb") as handle:
                        handle.seek(0, os.SEEK_END)
                        size = handle.tell()
                        handle.seek(max(0, size - 1_500_000))
                        data = handle.read()
                    contents = data.decode("utf-8", errors="replace")
                    if size > len(data):
                        contents = "[Showing the most recent 1.5 MB]\n" + contents
                except OSError as exc:
                    contents = f"Log file is not available yet.\n\n{exc}"
                text = window["text"]
                if text.toPlainText() != contents:
                    text.setPlainText(contents)
                    cursor = text.textCursor()
                    cursor.movePosition(QTextCursor.MoveOperation.End)
                    text.setTextCursor(cursor)
