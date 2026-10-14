from .common import *

class PathRow(QWidget):
    def __init__(self, label: str, placeholder: str, directory: bool, parent=None):
        super().__init__(parent)
        self.directory = directory
        self.edit = QLineEdit()
        self.edit.setPlaceholderText(placeholder)
        self.button = QPushButton("Browse")
        self.button.setObjectName("secondaryButton")
        self.button.clicked.connect(self.browse)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(10)
        row.addWidget(self.edit, 1)
        row.addWidget(self.button)
        self.label = label

    def browse(self):
        if self.directory:
            path = QFileDialog.getExistingDirectory(self, f"Select {self.label}", self.edit.text() or str(Path.home()))
        else:
            path, _ = QFileDialog.getOpenFileName(self, f"Select {self.label}", self.edit.text() or str(Path.home()))
        if path:
            self.edit.setText(path)

    def path(self) -> Path:
        return Path(self.edit.text()).expanduser()


class NoWheelComboBox(QComboBox):

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMaxVisibleItems(12)
        self.view().setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)

    def wheelEvent(self, event):
        if self.view().isVisible():
            delta = event.angleDelta().y() or event.pixelDelta().y()
            bar = self.view().verticalScrollBar()
            bar.setValue(bar.value() - delta)
            event.accept()
        else:
            event.ignore()


class WorkerSignals(QObject):
    finished = pyqtSignal(object)


class FunctionWorker(QRunnable):
    def __init__(self, function):
        super().__init__()
        self.function = function
        self.signals = WorkerSignals()

    def run(self):
        try:
            result = {"result": self.function()}
        except Exception as exc:
            result = {"error": str(exc)}
        self.signals.finished.emit(result)


