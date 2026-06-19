#!/usr/bin/env python3
import struct
import threading
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from typing import Optional, Tuple
import os
import sys

import psutil
import traceback
import socket
from scapy.all import sniff, Raw, conf
import snappy
import net_pb2 as OverField_pb2
from msg_id import MsgId
import logging
import yaml
from ui import create_floating_window, send_text
import translate

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

id_to_name = {
    v: k
    for k, v in vars(MsgId).items()
    if not k.startswith("__") and isinstance(v, int)
}

flow_buffers = defaultdict(bytearray)

executor = ThreadPoolExecutor(max_workers=8)
pending_lock = threading.Lock()
pending = {}
next_seq = 0
print_seq = 0


def schedule_translation(name: str, text: str):
    global next_seq
    with pending_lock:
        seq = next_seq
        next_seq += 1
        future = executor.submit(translate.translate_text, text)
        pending[seq] = (name, future)


def printer_loop(stop_event: threading.Event):
    global print_seq
    while True:
        with pending_lock:
            has_next = print_seq in pending
        if not has_next:
            if stop_event.is_set():
                with pending_lock:
                    if not pending:
                        break
                time.sleep(0.05)
                continue
            time.sleep(0.05)
            continue
        with pending_lock:
            name, future = pending[print_seq]
        try:
            res = future.result(timeout=translate.TRANSLATION_TIMEOUT)
            if res:
                text = f"{name}>>>{res}"
                try:
                    send_text(text)
                except Exception:
                    logger.exception("send_text failed")
        except TimeoutError:
            pass
        except Exception:
            pass
        with pending_lock:
            try:
                del pending[print_seq]
            except KeyError:
                pass
        print_seq += 1


def process_flow_buffer(flow_key):
    buf = flow_buffers[flow_key]
    while True:
        if len(buf) < 2:
            break
        try:
            header_len = struct.unpack(">H", buf[0:2])[0]
            if header_len > 20 * 1024:
                del buf[:2]
                continue
        except Exception:
            try:
                del buf[:2]
            except Exception:
                break
            continue
        if len(buf) < 2 + header_len:
            break
        header_data = bytes(buf[2 : 2 + header_len])
        packet_head = OverField_pb2.PacketHead()
        try:
            packet_head.ParseFromString(header_data)
        except Exception:
            try:
                del buf[:2]
            except Exception:
                break
            continue
        total_needed = 2 + header_len + getattr(packet_head, "body_len", 0)
        if len(buf) < total_needed:
            break
        body_data = bytes(buf[2 + header_len : 2 + header_len + packet_head.body_len])
        try:
            del buf[:total_needed]
        except Exception:
            break
        if getattr(packet_head, "flag", 0) == 1:
            try:
                body_data = snappy.uncompress(body_data)
            except Exception:
                try:
                    del buf[:2]
                except Exception:
                    pass
                continue
        msgid = getattr(packet_head, "msg_id", None)
        if msgid is None:
            continue
        proto_name = id_to_name.get(msgid)
        proto_cls = getattr(OverField_pb2, proto_name, None)
        if proto_cls is None:
            continue
        try:
            sy = proto_cls()
            sy.ParseFromString(body_data)
            txt = getattr(sy.msg, "text", "")
            name = getattr(sy.msg, "name", "")
            if txt:
                schedule_translation(name, txt)
        except Exception:
            continue


def pkt_callback(
    pkt,
    ip_filter: Optional[str],
    port_range: Optional[Tuple[int, int]],
    stop_event: Optional[threading.Event] = None,
):
    if stop_event is not None and stop_event.is_set():
        return False
    if not pkt.haslayer(Raw):
        return
    ip_layer = pkt.getlayer("IP")
    if ip_layer is None:
        return
    src_ip = ip_layer.src
    dst_ip = ip_layer.dst
    sport = getattr(pkt.payload, "sport", None)
    dport = getattr(pkt.payload, "dport", None)
    if ip_filter is not None:
        if not (src_ip == ip_filter or dst_ip == ip_filter):
            return
    if port_range is not None:
        pmin, pmax = port_range
        sport_ok = sport is not None and pmin <= sport <= pmax
        dport_ok = dport is not None and pmin <= dport <= pmax
        if not (sport_ok or dport_ok):
            return
    payload = bytes(pkt[Raw].load)
    if not payload:
        return
    flow_key = (src_ip, dst_ip, sport, dport)
    flow_buffers[flow_key].extend(payload)
    try:
        process_flow_buffer(flow_key)
    except Exception:
        pass


def start_sniffer(
    iface: str,
    ip: Optional[str],
    port_range: Optional[Tuple[int, int]],
    stop_event: threading.Event,
    bpf: Optional[str] = None,
    promisc: bool = False,
):
    if ip is None:
        if port_range is not None:
            pmin, pmax = port_range
            bpf_filter = f"tcp and portrange {pmin}-{pmax}"
        else:
            bpf_filter = "tcp"
    else:
        if port_range is not None:
            pmin, pmax = port_range
            bpf_filter = f"tcp and host {ip} and portrange {pmin}-{pmax}"
        else:
            bpf_filter = f"tcp and host {ip}"
    if bpf:
        bpf_filter = f"({bpf_filter}) and ({bpf})"
    conf.sniff_promisc = bool(promisc)

    def _stop_filter(pkt):
        return stop_event.is_set()

    def _prn_wrapper(pkt):
        return pkt_callback(
            pkt, ip_filter=ip, port_range=port_range, stop_event=stop_event
        )

    try:
        sniff(
            iface=iface,
            filter=bpf_filter,
            prn=_prn_wrapper,
            store=0,
            stop_filter=_stop_filter,
        )
    except Exception as e:
        traceback.print_exc()
        print(
            f"Sniff initialization failed, did you forget to install NPCAP? https://npcap.com/dist/npcap-1.87.exe"
        )


def get_active_interface():
    gws = psutil.net_if_addrs()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
    finally:
        s.close()
    for iface, addrs in gws.items():
        for addr in addrs:
            if addr.family == socket.AF_INET and addr.address == local_ip:
                return iface
    return None


def find_external_config(filename="config.json"):
    exe_dir = (
        os.path.dirname(sys.executable)
        if getattr(sys, "frozen", False)
        else os.path.dirname(os.path.abspath(__file__))
    )
    candidate = os.path.join(exe_dir, filename)
    if os.path.exists(candidate):
        return candidate
    cwd_candidate = os.path.join(os.getcwd(), filename)
    if os.path.exists(cwd_candidate):
        return cwd_candidate
    user_dir = os.path.expanduser("~")
    user_candidate = os.path.join(user_dir, "." + filename)
    if os.path.exists(user_candidate):
        return user_candidate
    return None


def main():
    try:
        config_path = find_external_config("config.yaml")
        with open(config_path, "r", encoding="utf-8") as f:
            cfg = yaml.load(f, Loader=yaml.SafeLoader)
    except Exception as e:
        traceback.print_exc()
        print(
            "config.json load failed! use default config. You can access https://github.com/byzp/of-translate/blob/main/config.yaml Download this file"
        )
        cfg = {}
    translate.configure(cfg)
    iface = get_active_interface()
    threading.Thread(target=create_floating_window, daemon=True).start()
    if iface is None:
        raise RuntimeError("No active network interface found")
    stop_evt = threading.Event()
    printer_thread = threading.Thread(target=printer_loop, args=(stop_evt,))
    printer_thread.start()
    sniff_thread = threading.Thread(
        target=start_sniffer,
        args=(iface, None, (11001, 11003), stop_evt),
        kwargs={"bpf": None, "promisc": False},
    )
    sniff_thread.start()
    time.sleep(1)
    send_text(f"Started, listening on adapter: {iface}")
    try:
        while sniff_thread.is_alive():
            time.sleep(0.2)
    except KeyboardInterrupt:
        stop_evt.set()
    sniff_thread.join(timeout=5)
    with pending_lock:
        pass
    stop_evt.set()
    printer_thread.join(timeout=5)
    executor.shutdown(wait=False)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        traceback.print_exc()
        print(
            "Startup failed, you can copy the output of this page and submit an issue. https://github.com/byzp/of-translate/issues"
        )
        input("press any key to exit...")
