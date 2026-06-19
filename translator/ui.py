from PyQt5.QtWidgets import (
    QApplication,
    QWidget,
    QTextEdit,
    QVBoxLayout,
    QHBoxLayout,
    QPushButton,
    QToolTip,
)
from PyQt5.QtCore import Qt, QTimer, pyqtSignal, QObject, QRect
import sys
import threading

_app = None
_win = None
_pending_texts = []
_pending_clear = False

_initial_opacity = 0.95
_min_opacity = 0.25
_fade_step = 0.05
_fade_interval_ms = 300
_idle_delay_ms = 3000

_resize_margin = 8
_min_width = 200
_min_height = 80


class _TextSignal(QObject):
    sig = pyqtSignal(str)
    clear_sig = pyqtSignal()


_signal = _TextSignal()


class ClearButton(QPushButton):
    def __init__(self, parent=None):
        super().__init__("↺", parent)
        self._tip = "Clear"
        self.setToolTip("")
        self.setMinimumWidth(48)

        self.setAttribute(Qt.WA_Hover, True)

    def enterEvent(self, e):
        QToolTip.showText(self.mapToGlobal(self.rect().bottomRight()), self._tip)
        super().enterEvent(e)

    def leaveEvent(self, e):
        QToolTip.hideText()
        super().leaveEvent(e)


class FloatingWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.setAttribute(Qt.WA_TranslucentBackground)

        self._current_opacity = _initial_opacity
        self.setWindowOpacity(self._current_opacity)

        self._drag_offset = None
        self._resizing = False
        self._resize_dir = ()
        self._press_pos = None
        self._press_geom = None

        self._fade_timer = QTimer(self)
        self._fade_timer.setInterval(_fade_interval_ms)
        self._fade_timer.timeout.connect(self._fade_step)

        self._idle_timer = QTimer(self)
        self._idle_timer.setSingleShot(True)
        self._idle_timer.timeout.connect(self._start_fade)
        self._idle_timer.start(_idle_delay_ms)

        self.setMouseTracking(True)
        self._init_ui()

        _signal.sig.connect(self.receive_text)
        _signal.clear_sig.connect(self.clear)

    def _init_ui(self):
        self.resize(420, 200)
        self.setMinimumSize(_min_width, _min_height)

        self.drag_bar = QWidget(self)
        self.drag_bar.setFixedHeight(24)
        self.drag_bar.setCursor(Qt.SizeAllCursor)
        self.drag_bar.mousePressEvent = self._drag_mouse_press
        self.drag_bar.mouseMoveEvent = self._drag_mouse_move
        self.drag_bar.mouseReleaseEvent = self._drag_mouse_release

        self.text = QTextEdit(self)
        self.text.setReadOnly(True)
        self.text.setAcceptRichText(False)
        self.text.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.text.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.text.installEventFilter(self)

        self.drag_bar.setStyleSheet(
            "background: rgba(0,0,0,120); border-top-left-radius:8px; border-top-right-radius:8px;"
        )
        self.text.setStyleSheet(
            "background: rgba(0,0,0,120); color: white; border-bottom-left-radius:8px; border-bottom-right-radius:8px; padding:6px;"
        )

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        drag_layout = QHBoxLayout(self.drag_bar)
        drag_layout.setContentsMargins(6, 0, 6, 0)
        drag_layout.addStretch()

        self.clear_button = ClearButton(self.drag_bar)
        self.clear_button.setFixedHeight(20)
        self.clear_button.setFlat(True)
        self.clear_button.setFocusPolicy(Qt.NoFocus)
        self.clear_button.clicked.connect(self.clear)
        self.clear_button.setStyleSheet(
            "QPushButton{background: transparent; color: white; border: none; font-weight: bold;}"
            "QPushButton:hover{background: rgba(255,255,255,30); border-radius:8px;}"
        )

        drag_layout.addWidget(self.clear_button)
        main_layout.addWidget(self.drag_bar)
        main_layout.addWidget(self.text)

        self.text.setMouseTracking(True)
        self.drag_bar.setMouseTracking(True)

    def eventFilter(self, obj, event):
        if event.type() in (
            event.MouseButtonPress,
            event.MouseButtonRelease,
            event.MouseMove,
            event.Wheel,
            event.KeyPress,
        ):
            self._reset_opacity_and_timer()

        if event.type() in (
            event.MouseButtonPress,
            event.MouseButtonRelease,
            event.MouseMove,
        ):
            local_pos = event.pos()
            if obj is not self:
                local_pos = obj.mapTo(self, local_pos)

            if (
                event.type() == event.MouseButtonPress
                and event.button() == Qt.LeftButton
            ):
                self._window_mouse_press(local_pos, event.globalPos())
            elif (
                event.type() == event.MouseButtonRelease
                and event.button() == Qt.LeftButton
            ):
                self._window_mouse_release()
            elif event.type() == event.MouseMove:
                self._window_mouse_move(local_pos, event.globalPos(), event.buttons())

        return super().eventFilter(obj, event)

    def _get_edges(self, p):
        x, y, w, h = p.x(), p.y(), self.width(), self.height()
        dirs = ()
        if x <= _resize_margin:
            dirs += ("left",)
        if x >= w - _resize_margin:
            dirs += ("right",)
        if y <= _resize_margin:
            dirs += ("top",)
        if y >= h - _resize_margin:
            dirs += ("bottom",)
        return dirs

    def _update_cursor(self, p):
        d = self._get_edges(p)
        if ("left" in d and "top" in d) or ("right" in d and "bottom" in d):
            self.setCursor(Qt.SizeFDiagCursor)
        elif ("right" in d and "top" in d) or ("left" in d and "bottom" in d):
            self.setCursor(Qt.SizeBDiagCursor)
        elif "left" in d or "right" in d:
            self.setCursor(Qt.SizeHorCursor)
        elif "top" in d or "bottom" in d:
            self.setCursor(Qt.SizeVerCursor)
        else:
            self.setCursor(Qt.ArrowCursor)

    def _window_mouse_press(self, local_pos, global_pos):
        dirs = self._get_edges(local_pos)
        if dirs:
            self._resizing = True
            self._resize_dir = dirs
            self._press_pos = global_pos
            self._press_geom = self.geometry()

    def _window_mouse_move(self, local_pos, global_pos, buttons):
        if self._resizing:
            dx = global_pos.x() - self._press_pos.x()
            dy = global_pos.y() - self._press_pos.y()
            g = QRect(self._press_geom)

            if "left" in self._resize_dir:
                g.setLeft(min(g.right() - self.minimumWidth() + 1, g.left() + dx))
            if "right" in self._resize_dir:
                g.setRight(max(g.left() + self.minimumWidth() - 1, g.right() + dx))
            if "top" in self._resize_dir:
                g.setTop(min(g.bottom() - self.minimumHeight() + 1, g.top() + dy))
            if "bottom" in self._resize_dir:
                g.setBottom(max(g.top() + self.minimumHeight() - 1, g.bottom() + dy))

            self.setGeometry(g)
        else:
            self._update_cursor(local_pos)

    def _window_mouse_release(self):
        self._resizing = False
        self._resize_dir = ()
        self._press_pos = None
        self._press_geom = None
        self.setCursor(Qt.ArrowCursor)

    def _drag_mouse_press(self, e):
        if e.button() == Qt.LeftButton:
            self._drag_offset = e.globalPos() - self.frameGeometry().topLeft()

    def _drag_mouse_move(self, e):
        if self._drag_offset and e.buttons() & Qt.LeftButton:
            self.move(e.globalPos() - self._drag_offset)

    def _drag_mouse_release(self, e):
        self._drag_offset = None

    def receive_text(self, s):
        self._reset_opacity_and_timer()
        if self.text.toPlainText():
            self.text.append(s)
        else:
            self.text.setPlainText(s)
        c = self.text.textCursor()
        c.movePosition(c.End)
        self.text.setTextCursor(c)

    def clear(self):
        self._reset_opacity_and_timer()
        self.text.clear()

    def _reset_opacity_and_timer(self):
        self._fade_timer.stop()
        self._current_opacity = _initial_opacity
        self.setWindowOpacity(self._current_opacity)
        self._idle_timer.start(_idle_delay_ms)

    def _start_fade(self):
        if self._current_opacity > _min_opacity:
            self._fade_timer.start()

    def _fade_step(self):
        self._current_opacity = max(_min_opacity, self._current_opacity - _fade_step)
        self.setWindowOpacity(self._current_opacity)
        if self._current_opacity <= _min_opacity:
            self._fade_timer.stop()


def _ensure_app_and_window():
    global _app, _win, _pending_clear
    if _app is None:
        _app = QApplication.instance() or QApplication(sys.argv)
    if _win is None:
        _win = FloatingWindow()
        _win.show()
        for t in _pending_texts:
            _signal.sig.emit(t)
        _pending_texts.clear()
        if _pending_clear:
            _signal.clear_sig.emit()
            _pending_clear = False


def create_floating_window():
    _ensure_app_and_window()
    _app.exec_()


def send_text(s):
    if _win is None:
        _pending_texts.append(s)
        _ensure_app_and_window()
        return
    _signal.sig.emit(s)


def clear_text():
    global _pending_clear
    if _win is None:
        _pending_clear = True
        _ensure_app_and_window()
        return
    _signal.clear_sig.emit()


if __name__ == "__main__":
    create_floating_window()
