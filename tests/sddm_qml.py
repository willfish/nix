"""Execute rendered themes in Qt, without authenticating or starting a desktop.

The model mirrors the pinned SDDM roles, including its empty DisplayRole.
These tests cover QML behaviour, not PAM or a real compositor session.
"""
import itertools
import json
from pathlib import Path
import sys
import unittest

from PySide6.QtCore import (
    QAbstractListModel, QModelIndex, QObject, Qt, QUrl, Signal, Slot,
)
from PySide6.QtGui import QGuiApplication
from PySide6.QtQuick import QQuickView
from PySide6.QtTest import QTest

THEMES = json.loads(Path(sys.argv.pop(1)).read_text())
SOURCE = Path(sys.argv.pop(1))
APP = QGuiApplication([])


class Sessions(QAbstractListModel):
    FILE = int(Qt.UserRole) + 2
    NAME = int(Qt.UserRole) + 4

    def __init__(self, files):
        super().__init__()
        self.files = files

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.files)

    def roleNames(self):
        return {self.FILE: b"file", self.NAME: b"name"}

    def data(self, index, role=Qt.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.files):
            return None
        if role == self.FILE:
            return self.files[index.row()]
        if role == self.NAME:
            return "A translated display name"
        return None


class Login(QObject):
    loginFailed = Signal()
    loginSucceeded = Signal()

    def __init__(self):
        super().__init__()
        self.calls = []

    @Slot(str, str, int)
    def login(self, user, password, session):
        self.calls.append((user, password, session))


class SddmQmlTests(unittest.TestCase):
    def load(self, theme, files):
        self.model = Sessions(files)
        self.login = Login()
        self.view = QQuickView()
        self.view.rootContext().setContextProperty("sessionModel", self.model)
        self.view.rootContext().setContextProperty("sddm", self.login)
        self.view.setSource(QUrl.fromLocalFile(str(Path(theme) / "Main.qml")))
        self.assertEqual(
            self.view.status(), QQuickView.Ready,
            "\n".join(error.toString() for error in self.view.errors()),
        )
        self.view.show()
        self.view.requestActivate()
        APP.processEvents()
        self.root = self.view.rootObject()
        inputs = [obj for obj in self.root.findChildren(QObject)
                  if obj.inherits("QQuickTextInput")]
        self.assertEqual(len(inputs), 1)
        self.password = inputs[0]

    def type_text(self, text):
        # QTest.keyClicks is QWidget-only; QQuickView uses QWindow events.
        for char in text:
            QTest.keyClick(self.view, Qt.Key(ord(char.upper())))

    def close(self):
        self.view.close()
        self.view.setSource(QUrl())
        self.view.deleteLater()
        APP.processEvents()

    def test_model_contract_matches_pinned_sddm(self):
        header = (SOURCE / "src/greeter/SessionModel.h").read_text()
        self.assertRegex(
            header, r"DirectoryRole\s*=\s*Qt::UserRole\s*\+\s*1,"
            r"\s*FileRole,\s*TypeRole,\s*NameRole",
        )
        source = (SOURCE / "src/greeter/SessionModel.cpp").read_text()
        self.assertRegex(
            source, r"case FileRole:\s*return session->fileName\(\);",
        )
        self.assertNotIn("case Qt::DisplayRole:", source)
        model = Sessions(["hyprland.desktop"])
        self.assertIsNone(model.data(model.index(0, 0), Qt.DisplayRole))

    def test_every_theme_selects_plain_hyprland_regardless_of_order(self):
        for name, theme in THEMES.items():
            for files in itertools.permutations([
                "hyprland-uwsm.desktop", "hyprland.desktop", "other.desktop",
            ]):
                with self.subTest(theme=name, files=files):
                    self.load(theme, list(files))
                    try:
                        self.assertEqual(
                            self.root.property("sessionIndex"),
                            files.index("hyprland.desktop"),
                        )
                        self.assertEqual(
                            self.root.property("currentUser"), "william",
                        )
                    finally:
                        self.close()

    def test_missing_session_never_submits_login(self):
        for name, theme in THEMES.items():
            for files in ([], ["hyprland-uwsm.desktop"], ["other.desktop"]):
                with self.subTest(theme=name, files=files):
                    self.load(theme, files)
                    try:
                        self.assertEqual(self.root.property("sessionIndex"), -1)
                        self.type_text("dummy-password")
                        QTest.keyClick(self.view, Qt.Key_Return)
                        QTest.keyClick(self.view, Qt.Key_Enter)
                        self.assertEqual(self.login.calls, [])
                    finally:
                        self.close()

    def test_password_submit_failure_clear_and_retry(self):
        for name, theme in THEMES.items():
            with self.subTest(theme=name):
                self.load(theme, ["hyprland-uwsm.desktop", "hyprland.desktop"])
                try:
                    self.assertTrue(self.password.property("activeFocus"))
                    self.type_text("wrong-dummy")
                    QTest.keyClick(self.view, Qt.Key_Return)
                    self.assertEqual(
                        self.login.calls, [("william", "wrong-dummy", 1)],
                    )
                    self.login.loginFailed.emit()
                    APP.processEvents()
                    self.assertEqual(self.password.property("text"), "")
                    self.assertTrue(self.root.property("loginFailed"))
                    self.assertTrue(self.password.property("activeFocus"))
                    self.type_text("retry-dummy")
                    QTest.keyClick(self.view, Qt.Key_Enter)
                    self.assertEqual(
                        self.login.calls[-1], ("william", "retry-dummy", 1),
                    )
                    self.assertEqual(len(self.login.calls), 2)
                    self.login.loginSucceeded.emit()
                    self.assertFalse(self.root.property("loginFailed"))
                finally:
                    self.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
