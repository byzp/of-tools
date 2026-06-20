#include "sniffer.hpp"

#include "mini_pcap.hpp"
// windows.h must come after winsock2.h (pulled in by mini_pcap.hpp)
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>

#include "pb.hpp"
#include "snappy_raw.hpp"

#pragma comment(lib, "ws2_32.lib")

// ── dynamic wpcap loader ────────────────────────────────────────────────────
bool load_pcap_api(PcapApi& api) {
    HMODULE m = LoadLibraryA("wpcap.dll");
    if (!m) return false;
    api.findalldevs = (pcap_findalldevs_t)GetProcAddress(m, "pcap_findalldevs");
    api.freealldevs = (pcap_freealldevs_t)GetProcAddress(m, "pcap_freealldevs");
    api.open_live = (pcap_open_live_t)GetProcAddress(m, "pcap_open_live");
    api.compile = (pcap_compile_t)GetProcAddress(m, "pcap_compile");
    api.setfilter = (pcap_setfilter_t)GetProcAddress(m, "pcap_setfilter");
    api.next_ex = (pcap_next_ex_t)GetProcAddress(m, "pcap_next_ex");
    api.datalink = (pcap_datalink_t)GetProcAddress(m, "pcap_datalink");
    api.freecode = (pcap_freecode_t)GetProcAddress(m, "pcap_freecode");
    api.close = (pcap_close_t)GetProcAddress(m, "pcap_close");
    return api.ok();
}

// ── framer (main.py _process_buf) ───────────────────────────────────────────
// Uses a read cursor and trims the consumed prefix once per call, so resyncing a
// mid-stream connection is O(n) instead of O(n^2) (erasing 2 bytes at a time off
// the front of a vector was the cause of ~1s-per-packet stalls).
void frame_and_dispatch(std::vector<uint8_t>& buf, const DyeCallback& cb) {
    // Safety cap: if the buffer grows huge without aligning (pathological garbage
    // stream), drop all but the last 64 KB and resync — bounds memory.
    const size_t kCap = 2 * 1024 * 1024;
    if (buf.size() > kCap)
        buf.erase(buf.begin(), buf.end() - 64 * 1024);

    const size_t n = buf.size();
    size_t pos = 0;
    while (true) {
        if (n - pos < 2) break;
        uint32_t hl = ((uint32_t)buf[pos] << 8) | buf[pos + 1];
        if (hl > 20 * 1024) {
            pos += 2;  // resync: advance cursor (no erase)
            continue;
        }
        if (n - pos < 2 + (size_t)hl) break;
        uint32_t msg_id, flag, body_len;
        if (!pb::parse_packet_head(&buf[pos + 2], hl, msg_id, flag, body_len)) {
            pos += 2;
            continue;
        }
        size_t need = 2 + (size_t)hl + body_len;
        if (n - pos < need) break;
        // consume this frame
        const uint8_t* body_ptr = &buf[pos + 2 + hl];
        size_t blen = body_len;
        pos += need;
        const uint8_t* bp = body_ptr;
        size_t bl = blen;
        std::vector<uint8_t> dec;
        if (flag == 1) {
            if (!snap::uncompress(body_ptr, blen, dec)) continue;
            bp = dec.data();
            bl = dec.size();
        }
        if (msg_id != 1652) continue;
        uint32_t picture_id;
        std::vector<double> params;
        if (!pb::parse_colorant_rsp(bp, bl, picture_id, params)) continue;
        cb(picture_id, params);
        break;  // mirror Python: stop after handling one 1652 this pass
    }
    if (pos > 0) buf.erase(buf.begin(), buf.begin() + pos);  // single O(n) trim
}

// ── packet parsing + per-flow reassembly ────────────────────────────────────
namespace {

struct FlowKey {
    uint32_t src, dst;
    uint16_t sp, dp;
    bool operator==(const FlowKey& o) const {
        return src == o.src && dst == o.dst && sp == o.sp && dp == o.dp;
    }
};
struct FlowKeyHash {
    size_t operator()(const FlowKey& k) const {
        uint64_t a = ((uint64_t)k.src << 32) ^ k.dst;
        uint64_t b = ((uint64_t)k.sp << 16) | k.dp;
        return std::hash<uint64_t>()(a) ^ (std::hash<uint64_t>()(b) << 1);
    }
};
using FlowMap = std::unordered_map<FlowKey, std::vector<uint8_t>, FlowKeyHash>;

inline uint16_t be16(const u_char* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

void handle_packet(const u_char* data, uint32_t caplen, int dlt, FlowMap& flows,
                   const DyeCallback& cb) {
    size_t off = 0;
    if (dlt == DLT_EN10MB || dlt < 0 /* unknown: try ethernet */) {
        if (caplen < 14) return;
        uint16_t eth = be16(data + 12);
        off = 14;
        if (eth == 0x8100) {  // 802.1Q VLAN tag
            if (caplen < 18) return;
            eth = be16(data + 16);
            off = 18;
        }
        if (eth != 0x0800) return;  // not IPv4
    } else if (dlt == DLT_NULL) {
        if (caplen < 4) return;
        off = 4;  // loopback 4-byte address family
    } else if (dlt == DLT_RAW) {
        off = 0;
    } else {
        if (caplen < 14) return;
        if (be16(data + 12) != 0x0800) return;
        off = 14;
    }

    if (off + 20 > caplen) return;
    const u_char* ip = data + off;
    if ((ip[0] >> 4) != 4) return;  // IPv4 only
    int ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || off + (size_t)ihl > caplen) return;
    if (ip[9] != 6) return;  // TCP
    uint16_t total_len = be16(ip + 2);
    uint32_t src, dst;
    memcpy(&src, ip + 12, 4);
    memcpy(&dst, ip + 16, 4);

    const u_char* tcp = ip + ihl;
    if ((size_t)(tcp - data) + 20 > caplen) return;
    uint16_t sport = be16(tcp);
    uint16_t dport = be16(tcp + 2);
    if (!(sport >= 11001 && sport <= 11003)) return;  // server->client only
    int dataoff = (tcp[12] >> 4) * 4;
    if (dataoff < 20) return;

    const u_char* payload = tcp + dataoff;
    long pstart = (long)(payload - data);
    long ip_end = (long)off + total_len;  // prefer IP length (skip eth padding)
    long plen = ip_end - pstart;
    long avail = (long)caplen - pstart;
    if (plen > avail) plen = avail;
    if (plen <= 0) return;

    FlowKey key{src, dst, sport, dport};
    std::vector<uint8_t>& buf = flows[key];
    buf.insert(buf.end(), payload, payload + plen);
    frame_and_dispatch(buf, cb);
}

}  // namespace

// ── active interface detection ──────────────────────────────────────────────
namespace {

uint32_t local_ipv4() {  // returns IPv4 in network byte order, 0 on failure
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    uint32_t ip = 0;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0) ip = local.sin_addr.s_addr;
    }
    closesocket(s);
    return ip;
}

std::string find_device(PcapApi& api, uint32_t local_ip) {
    pcap_if_t* alldevs = nullptr;
    char err[PCAP_ERRBUF_SIZE] = {0};
    if (api.findalldevs(&alldevs, err) != 0 || !alldevs) return "";
    std::string found;
    for (pcap_if_t* d = alldevs; d; d = d->next) {
        for (pcap_addr_t* a = d->addresses; a; a = a->next) {
            if (a->addr && a->addr->sa_family == AF_INET) {
                uint32_t ip = ((sockaddr_in*)a->addr)->sin_addr.s_addr;
                if (ip == local_ip) {
                    found = d->name ? d->name : "";
                    break;
                }
            }
        }
        if (!found.empty()) break;
    }
    api.freealldevs(alldevs);
    return found;
}

}  // namespace

bool start_sniffer(const DyeCallback& cb) {
    PcapApi api;
    if (!load_pcap_api(api)) {
        fprintf(stderr, "error: failed to load wpcap.dll (is npcap installed?)\n");
        return false;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    uint32_t local = local_ipv4();
    if (local == 0) {
        fprintf(stderr, "error: can't detect network interface\n");
        return false;
    }
    std::string dev = find_device(api, local);
    if (dev.empty()) {
        fprintf(stderr, "error: can't detect network interface\n");
        return false;
    }

    char err[PCAP_ERRBUF_SIZE] = {0};
    pcap_t* h = api.open_live(dev.c_str(), 65536, 1, 1000, err);
    if (!h) {
        fprintf(stderr, "error: pcap_open_live failed: %s\n", err);
        return false;
    }
    int dlt = api.datalink(h);

    struct bpf_program fp;
    if (api.compile(h, &fp, "tcp and portrange 11001-11003", 1,
                    PCAP_NETMASK_UNKNOWN) != 0) {
        fprintf(stderr, "error: pcap_compile failed\n");
        api.close(h);
        return false;
    }
    if (api.setfilter(h, &fp) != 0) {
        fprintf(stderr, "error: pcap_setfilter failed\n");
        api.freecode(&fp);
        api.close(h);
        return false;
    }
    api.freecode(&fp);

    std::thread([api, h, dlt, cb]() {
        FlowMap flows;
        struct pcap_pkthdr* hdr = nullptr;
        const u_char* data = nullptr;
        while (true) {
            int r = api.next_ex(h, &hdr, &data);
            if (r == 1)
                handle_packet(data, hdr->caplen, dlt, flows, cb);
            else if (r == 0)
                continue;  // timeout
            else
                break;  // error / capture closed
        }
    }).detach();
    return true;
}
