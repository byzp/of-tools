"""
Color picker floating UI — uvy indicator bar, horizontal guide line,
five dye-color squares, target-color picker, and match-result text.
"""

from PyQt5.QtWidgets import (
    QApplication,
    QWidget,
    QPushButton,
    QTextEdit,
    QVBoxLayout,
    QHBoxLayout,
    QColorDialog,
    QSizePolicy,
)
from PyQt5.QtCore import Qt, pyqtSignal, QObject, QRect, QPoint
from PyQt5.QtGui import (
    QPainter,
    QColor,
    QPen,
    QBrush,
    QPolygon,
    QFont,
    QFontMetrics,
)
import sys

# ── global state ──────────────────────────────────────────────────
_app = None
_win = None
_on_target_changed = None  # callback(hex_str: str)

_resize_margin = 8
_min_width = 400
_min_height = 360
_bar_width = 50
_bar_pad_top = 6
_bar_pad_bottom = 20


# ── signal bridge (thread-safe) ───────────────────────────────────
class _DataSignal(QObject):
    result_sig = pyqtSignal(object)  # dict or None


_signal = _DataSignal()


# ── gray vertical bar (left edge) ─────────────────────────────────
class _BarArea(QWidget):
    """Dark vertical bar with '0'/'1' labels and a uvy-position marker."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._uvy = 0.5

    def set_uvy(self, uvy: float):
        self._uvy = max(0.0, min(1.0, float(uvy)))
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()

        # dark background
        p.fillRect(0, 0, w, h, QColor(42, 42, 48))
        # subtle right-edge line
        p.fillRect(w - 1, 0, 1, h, QColor(65, 65, 70))

        pad_top = _bar_pad_top
        pad_bottom = _bar_pad_bottom
        # labels
        font = QFont("Consolas", 8)
        p.setFont(font)
        p.setPen(QColor(140, 140, 140))
        fm = QFontMetrics(font)
        label_1 = "1"
        p.drawText((w - fm.width(label_1)) // 2, pad_top + fm.ascent(), label_1)
        label_0 = "0"
        p.drawText((w - fm.width(label_0)) // 2, h - pad_bottom, label_0)

        # current uvy position marker (1=top, 0=bottom)
        usable = h - pad_top - pad_bottom
        y = int(pad_top + (1.0 - self._uvy) * usable)
        p.setPen(QPen(QColor(220, 220, 220, 200), 2))
        p.drawLine(4, y, w - 4, y)


# ── single dye-color square ───────────────────────────────────────
class _ColorSquare(QWidget):
    """Filled square; optionally marked with a downward triangle."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._rgb = (60, 60, 60)
        self._marked = False
        self.setMinimumSize(54, 120)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Minimum)

    def set_color(self, rgb: tuple):
        self._rgb = tuple(int(c) for c in rgb[:3])
        self.update()

    def set_marked(self, marked: bool):
        self._marked = marked
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        r = self.rect()

        # color fill area (extend down, leave room at bottom for triangle)
        fill_rect = r.adjusted(0, 0, 0, -18)
        p.fillRect(fill_rect, QColor(*self._rgb))
        # subtle border
        p.setPen(QPen(QColor(255, 255, 255, 55)))
        p.drawRect(fill_rect.adjusted(0, 0, -1, -1))

        # upward-pointing triangle at bottom, pointing at the square above
        if self._marked:
            cx = r.center().x()
            tri_w = 21
            tri_h = 12
            tri_y = fill_rect.bottom() + 3
            tri = QPolygon(
                [
                    QPoint(cx, tri_y),  # tip (points up toward square)
                    QPoint(cx - tri_w // 2, tri_y + tri_h),  # bottom-left
                    QPoint(cx + tri_w // 2, tri_y + tri_h),  # bottom-right
                ]
            )
            p.setPen(Qt.NoPen)
            p.setBrush(QBrush(QColor(255, 200, 50)))
            p.drawPolygon(tri)


# ── transparent overlay that draws the horizontal guide line ──────
class _LineOverlay(QWidget):
    """Sits on top of content; draws a short solid line + left-pointing triangle + uvy number."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._uvy = 0.5
        self._bar_w = _bar_width
        self._pad_top = _bar_pad_top
        self._pad_bottom = _bar_pad_bottom
        self._spacer = 20
        self.setAttribute(Qt.WA_TransparentForMouseEvents)

    def configure(self, uvy: float, bar_w: int):
        self._uvy = max(0.0, min(1.0, float(uvy)))
        self._bar_w = bar_w
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        # y in content coordinates: bar top=0, bar bottom = h - spacer
        # bar-internal uvy range = [pad_top, (h-spacer) - pad_bottom]
        usable = h - self._spacer - self._pad_top - self._pad_bottom
        y = int(self._pad_top + (1.0 - self._uvy) * usable)
        bar_end = self._bar_w
        # very short line stub (triangle points at it, no extension)
        line_end = bar_end + 14
        if w <= bar_end + 12:
            return

        # solid horizontal stub
        pen = QPen(QColor(210, 210, 210, 180), 2)
        p.setPen(pen)
        p.drawLine(bar_end + 4, y, line_end, y)

        # left-pointing triangle (tip left, at right end of line)
        tri = QPolygon(
            [
                QPoint(line_end - 8, y),  # tip (left, pointing at line)
                QPoint(line_end, y - 5),  # top-right
                QPoint(line_end, y + 5),  # bottom-right
            ]
        )
        p.setPen(Qt.NoPen)
        p.setBrush(QBrush(QColor(210, 210, 210, 200)))
        p.drawPolygon(tri)

        # uvy number to the right of the triangle
        font = QFont("Consolas", 9)
        p.setFont(font)
        p.setPen(QColor(220, 220, 220))
        fm = QFontMetrics(font)
        label = f"{self._uvy:.3f}"
        p.drawText(line_end + 4, y + fm.ascent() // 2 - 1, label)


# ── main floating window ──────────────────────────────────────────
class PickerWindow(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.setAttribute(Qt.WA_TranslucentBackground)

        # drag state
        self._drag_offset = None
        # resize state
        self._resizing = False
        self._resize_dir = ()
        self._press_pos = None
        self._press_geom = None
        # data
        self._target_hex = "#ffffff"
        # collapse state
        self._collapsed = False
        self._saved_size = None

        self.setMouseTracking(True)
        self._init_ui()
        _signal.result_sig.connect(self._on_result)

    # ── layout ─────────────────────────────────────────────────
    def _init_ui(self):
        self.resize(500, 360)
        self.setMinimumSize(_min_width, _min_height)

        # ── drag bar (top) ──
        self.drag_bar = QWidget(self)
        self.drag_bar.setFixedHeight(24)
        self.drag_bar.setCursor(Qt.SizeAllCursor)
        self.drag_bar.mousePressEvent = self._drag_mouse_press
        self.drag_bar.mouseMoveEvent = self._drag_mouse_move
        self.drag_bar.mouseReleaseEvent = self._drag_mouse_release
        self.drag_bar.setStyleSheet(
            "background: rgba(0,0,0,140);"
            "border-top-left-radius: 8px; border-top-right-radius: 8px;"
        )

        self._btn_collapse = QPushButton("\u2212")  # minus sign
        self._btn_collapse.setFixedSize(22, 18)
        self._btn_collapse.setFlat(True)
        self._btn_collapse.setFocusPolicy(Qt.NoFocus)
        self._btn_collapse.setCursor(Qt.PointingHandCursor)
        self._btn_collapse.setStyleSheet(
            "QPushButton{background: transparent; color: #aaa; border: none;"
            "font-size: 14px; font-weight: bold;}"
            "QPushButton:hover{background: rgba(255,255,255,25); border-radius: 4px; color: #fff;}"
        )
        self._btn_collapse.clicked.connect(self._toggle_collapse)

        drag_layout = QHBoxLayout(self.drag_bar)
        drag_layout.setContentsMargins(6, 0, 4, 0)
        drag_layout.addStretch()
        drag_layout.addWidget(self._btn_collapse)

        # ── content area ──
        content = QWidget(self)
        content.setStyleSheet(
            "background: rgba(0,0,0,140);"
            "border-bottom-left-radius: 8px; border-bottom-right-radius: 8px;"
        )
        content.setMouseTracking(True)
        content.installEventFilter(self)
        self._content = content

        # ── color strip + picker button (above squares, same width) ──
        self._color_strip = QWidget()
        self._color_strip.setFixedHeight(32)
        self._color_strip.setCursor(Qt.PointingHandCursor)
        self._color_strip.mousePressEvent = lambda e: self._pick_color()
        self._update_color_strip_style()

        # info text (append output, like translator)
        self._label_info = QTextEdit()
        self._label_info.setReadOnly(True)
        self._label_info.setAcceptRichText(False)
        self._label_info.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self._label_info.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self._label_info.setStyleSheet(
            "color: #bbb; font-family: Consolas, monospace;"
            "font-size: 13px; padding: 4px 6px; background: transparent;"
            "border: none;"
        )
        self._label_info.setMinimumHeight(60)
        self._label_info.setMaximumHeight(140)
        self._label_info.setPlainText("select a target color")

        # ── five color squares ──
        squares_row = QHBoxLayout()
        squares_row.setContentsMargins(8, 4, 8, 10)
        squares_row.setSpacing(6)
        self._squares = []
        for _ in range(5):
            sq = _ColorSquare()
            self._squares.append(sq)
            squares_row.addWidget(sq)

        # ── right-side layout ──
        right_layout = QVBoxLayout()
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(0)
        right_layout.addWidget(self._label_info)
        right_layout.addStretch()
        # color strip (same margins as squares, directly above them)
        strip_wrapper = QHBoxLayout()
        strip_wrapper.setContentsMargins(8, 2, 8, 0)
        strip_wrapper.addWidget(self._color_strip)
        right_layout.addLayout(strip_wrapper)
        right_layout.addLayout(squares_row)

        # ── gray bar (left, with bottom gap) ──
        self._bar = _BarArea()
        self._bar.setFixedWidth(_bar_width)
        bar_wrapper = QVBoxLayout()
        bar_wrapper.setContentsMargins(0, 0, 0, 0)
        bar_wrapper.addWidget(self._bar)
        bar_wrapper.addSpacing(20)

        # ── content assembly ──
        content_layout = QHBoxLayout(content)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(70)
        content_layout.addLayout(bar_wrapper)
        content_layout.addLayout(right_layout)

        # ── line overlay (painted on top of content) ──
        self._line_overlay = _LineOverlay(content)
        self._line_overlay.raise_()

        # ── window layout ──
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)
        main_layout.addWidget(self.drag_bar)
        main_layout.addWidget(content)

    def _update_color_strip_style(self):
        self._color_strip.setStyleSheet(
            f"background: {self._target_hex};"
            "border: 1px solid rgba(255,255,255,40); border-radius: 4px;"
        )

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, "_line_overlay") and hasattr(self, "_content"):
            self._line_overlay.setGeometry(self._content.rect())

    # ── color picker ───────────────────────────────────────────
    def _pick_color(self):
        c = QColorDialog.getColor(QColor(self._target_hex), self, "Pick Target Color")
        if c.isValid():
            self._target_hex = c.name()
            self._update_color_strip_style()
            if _on_target_changed:
                _on_target_changed(self._target_hex)

    # ── result handler (called in main thread via signal) ──────
    def _on_result(self, r):
        if r is None:
            return

        uvy = r.get("uvy", 0.5)
        self._bar.set_uvy(uvy)
        self._line_overlay.configure(uvy, _bar_width)

        sim_pct = r.get("sim", 0) * 100
        target = r.get("target_hex", "???")
        matched = r.get("hex", "???")
        info = (
            f"{target} \u2192 {matched}"
            f"  sim={sim_pct:.1f}%"
            f"  uvy={uvy:.3f}"
            f"  slot={r.get('slot', 0)}"
        )
        if self._label_info.toPlainText() == "select a target color":
            self._label_info.setPlainText(info)
        else:
            self._label_info.append(info)
        # scroll to bottom
        c = self._label_info.textCursor()
        c.movePosition(c.End)
        self._label_info.setTextCursor(c)

        colors = r.get("colors", [])
        best_slot = r.get("slot", 0) - 1  # 0-based
        for i, sq in enumerate(self._squares):
            if i < len(colors):
                sq.set_color(colors[i][:3])
                sq.set_marked(i == best_slot)
            else:
                sq.set_color((55, 55, 55))
                sq.set_marked(False)

    # ── drag ───────────────────────────────────────────────────
    def _drag_mouse_press(self, e):
        if e.button() == Qt.LeftButton:
            self._drag_offset = e.globalPos() - self.frameGeometry().topLeft()

    def _drag_mouse_move(self, e):
        if self._drag_offset and e.buttons() & Qt.LeftButton:
            self.move(e.globalPos() - self._drag_offset)

    def _drag_mouse_release(self, e):
        self._drag_offset = None

    # ── collapse / restore ─────────────────────────────────────
    def _toggle_collapse(self):
        if self._collapsed:
            # restore — expand back, keep button screen position
            self._content.show()
            self._btn_collapse.setText("\u2212")  # minus
            if self._saved_size:
                new_w = self._saved_size.width()
                old_w = self.width()
                self.resize(self._saved_size)
                self.move(self.x() + old_w - new_w, self.y())
            self.setMinimumSize(_min_width, _min_height)
            self._collapsed = False
        else:
            # collapse — shrink to narrow bar, keep button screen position
            self._saved_size = self.size()
            old_w = self._saved_size.width()
            self._content.hide()
            self._btn_collapse.setText("+")
            new_w = 52
            self.setMinimumSize(new_w, 24)
            self.resize(new_w, 24)
            # shift right so the + button stays at its original screen position
            self.move(self.x() + old_w - new_w, self.y())
            self._collapsed = True

    # ── resize helpers ─────────────────────────────────────────
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

    # ── event filter (resize + activity detection) ─────────────
    def eventFilter(self, obj, event):
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


# ── public API ────────────────────────────────────────────────────
def create_picker_window(on_target_changed=None):
    """Create and show the floating picker window.  Blocks in the Qt event loop.

    *on_target_changed(hex_str: str)* — called when the user picks a target color.
    """
    global _app, _win, _on_target_changed
    _on_target_changed = on_target_changed

    if _app is None:
        _app = QApplication.instance() or QApplication(sys.argv)
    if _win is None:
        _win = PickerWindow()
        _win.show()
    _app.exec_()


def send_result(result: dict | None):
    """Push a search result to the UI (thread-safe).

    *result* must be a dict with keys:
        target_hex, hex, sim, uvy, slot, colors
    where *colors* is a list of 5 (r,g,b,a) tuples.
    Pass None to clear.
    """
    _signal.result_sig.emit(result)
