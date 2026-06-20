"""
Overfield outfit color picker — capture OutfitColorantSelectRsp, then
scan uvy 0.000-1.000 for the closest match to a target hex color.
"""

import math
import os
import socket
import struct
import sys
import threading
from collections import defaultdict

import numpy as np
import psutil
import snappy
from PIL import Image
from scapy.all import Raw, sniff

from algo import SwirlNoiseGenHelper

from net_pb2 import OutfitColorantSelectRsp, PacketHead

# texture resource path
if getattr(sys, "frozen", False):
    _TEXTURE_DIR = os.path.join(sys._MEIPASS, "resources", "swirlnoisetexture")
else:
    _TEXTURE_DIR = os.path.join(
        os.path.dirname(__file__), "resources", "swirlnoisetexture"
    )

PORT_MIN, PORT_MAX = 11001, 11003

_captured_params = None
_current_target = None


def hex_to_rgb(h: str) -> tuple[int, int, int]:
    h = h.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def rgb_to_hex(rgb: tuple) -> str:
    return f"#{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}"


def _texture_path(picture_id: int) -> str:
    return os.path.join(_TEXTURE_DIR, f"{max(1, min(picture_id, 4))}.png")


def _linearize(c: float) -> float:
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _cie76(a: tuple, b: tuple) -> float:
    lr_a, lg_a, lb_a = map(_linearize, a)
    lr_b, lg_b, lb_b = map(_linearize, b)

    def to_xyz(lr, lg, lb):
        return (
            lr * 0.4124564 + lg * 0.3575761 + lb * 0.1804375,
            lr * 0.2126729 + lg * 0.7151522 + lb * 0.0721750,
            lr * 0.0193339 + lg * 0.1191920 + lb * 0.9503041,
        )

    def lab(x, y, z):
        xn, yn, zn = 0.95047, 1.0, 1.08883

        def f(t):
            d = 6.0 / 29.0
            return t ** (1.0 / 3.0) if t > d**3 else t / (3.0 * d * d) + 4.0 / 29.0

        return (
            116.0 * f(y / yn) - 16.0,
            500.0 * (f(x / xn) - f(y / yn)),
            200.0 * (f(y / yn) - f(z / zn)),
        )

    L1, a1, b1 = lab(*to_xyz(lr_a, lg_a, lb_a))
    L2, a2, b2 = lab(*to_xyz(lr_b, lg_b, lb_b))
    return max(
        0.0, 1.0 - math.sqrt((L1 - L2) ** 2 + (a1 - a2) ** 2 + (b1 - b2) ** 2) / 100.0
    )


_flow_bufs = defaultdict(bytearray)


def _process_buf(key):
    global _captured_params
    buf = _flow_bufs[key]
    while True:
        if len(buf) < 2:
            return
        hl = struct.unpack(">H", buf[0:2])[0]
        if hl > 20 * 1024:
            del buf[:2]
            continue
        if len(buf) < 2 + hl:
            return
        head = PacketHead()
        try:
            head.ParseFromString(bytes(buf[2 : 2 + hl]))
        except Exception:
            del buf[:2]
            continue
        need = 2 + hl + head.body_len
        if len(buf) < need:
            return
        body = bytes(buf[2 + hl : 2 + hl + head.body_len])
        del buf[:need]
        if head.flag == 1:
            try:
                body = snappy.uncompress(body)
            except Exception:
                continue
        if head.msg_id != 1652:
            continue
        rsp = OutfitColorantSelectRsp()
        try:
            rsp.ParseFromString(body)
        except Exception:
            continue
        _captured_params = {
            "picture_id": rsp.param.picture_id,
            "swirl_params": list(rsp.param.params),
        }
        if _current_target is not None:
            _search_bg(_current_target, _captured_params)
        return


def _pkt_cb(pkt):
    if not pkt.haslayer(Raw):
        return
    ip = pkt.getlayer("IP")
    if ip is None:
        return
    sp = getattr(pkt.payload, "sport", None)
    if sp is None or not (PORT_MIN <= sp <= PORT_MAX):
        return
    key = (ip.src, ip.dst, sp, getattr(pkt.payload, "dport", None))
    _flow_bufs[key].extend(bytes(pkt[Raw].load))
    try:
        _process_buf(key)
    except Exception:
        pass


def _active_iface():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    finally:
        s.close()
    for name, addrs in psutil.net_if_addrs().items():
        for a in addrs:
            if a.family == socket.AF_INET and a.address == ip:
                return name
    return None


def start_sniffer():
    iface = _active_iface()
    if iface is None:
        print("error: can't detect network interface")
        sys.exit(1)
    bpf = f"tcp and portrange {PORT_MIN}-{PORT_MAX}"
    t = threading.Thread(
        target=lambda: sniff(iface=iface, filter=bpf, prn=_pkt_cb, store=0), daemon=True
    )
    t.start()


_STEP = 0.001
_COLOR_COUNT = 5


def _search(target_hex: str, params: dict) -> dict | None:
    target_rgb = hex_to_rgb(target_hex)
    pid = params.get("picture_id", 1)
    tex = Image.open(_texture_path(pid)).convert("RGBA")
    arr = np.array(params["swirl_params"], dtype=np.float64).reshape(16, 4)
    h = SwirlNoiseGenHelper()
    h.set_swirl_params(arr, tex)

    best, best_sim = None, -1.0
    for uvy in np.linspace(0.0, 1.0, int(round(1.0 / _STEP)) + 1, dtype=np.float32):
        for idx, (r, g, b, a) in enumerate(h.get_color_array(float(uvy), _COLOR_COUNT)):
            sim = _cie76(target_rgb, (r, g, b))
            if sim > best_sim:
                best_sim = sim
                best = {
                    "hex": rgb_to_hex((r, g, b)),
                    "sim": round(sim, 4),
                    "uvy": round(float(uvy), 4),
                    "slot": idx + 1,
                }
    return best


def _search_bg(target_hex: str, params: dict):
    threading.Thread(target=lambda: _do_search(target_hex, params), daemon=True).start()


def _do_search(target_hex: str, params: dict):
    r = _search(target_hex, params)
    if r:
        print(
            f"  {target_hex} -> {r['hex']}  sim={r['sim']*100}%  uvy={r['uvy']}  slot={r['slot']}"
        )


def main():
    if not os.path.isdir(_TEXTURE_DIR):
        print(f"error: texture dir not found {_TEXTURE_DIR}")
        sys.exit(1)

    start_sniffer()
    while True:
        try:
            raw = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not raw:
            continue
        raw = raw.lstrip("#")
        if len(raw) != 6 or not all(c in "0123456789abcdefABCDEF" for c in raw):
            continue
        global _current_target
        _current_target = raw
        if _captured_params is not None:
            _search_bg(_current_target, _captured_params)


if __name__ == "__main__":
    main()
