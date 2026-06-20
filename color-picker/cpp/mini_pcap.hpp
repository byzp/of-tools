// Minimal libpcap / npcap ABI declarations — just enough to dynamically load
// wpcap.dll (present in System32 on any npcap install) without the npcap SDK.
// Struct layouts match the stable libpcap ABI; pcap_pkthdr uses winsock's
// timeval (two 32-bit longs on Win64), giving the canonical 16-byte header.
#pragma once
#include <winsock2.h>  // struct timeval, sockaddr
#include <ws2tcpip.h>

#include <cstdint>

extern "C" {

typedef struct pcap pcap_t;
typedef unsigned int bpf_u_int32;
typedef unsigned char u_char;
typedef unsigned short u_short;

struct bpf_insn {
    u_short code;
    u_char jt;
    u_char jf;
    bpf_u_int32 k;
};
struct bpf_program {
    bpf_u_int32 bf_len;
    struct bpf_insn* bf_insns;
};

struct pcap_pkthdr {
    struct timeval ts;
    bpf_u_int32 caplen;  // bytes captured
    bpf_u_int32 len;     // original length
};

struct pcap_addr {
    struct pcap_addr* next;
    struct sockaddr* addr;
    struct sockaddr* netmask;
    struct sockaddr* broadaddr;
    struct sockaddr* dstaddr;
};
typedef struct pcap_addr pcap_addr_t;

struct pcap_if {
    struct pcap_if* next;
    char* name;
    char* description;
    struct pcap_addr* addresses;
    bpf_u_int32 flags;
};
typedef struct pcap_if pcap_if_t;

}  // extern "C"

#define PCAP_ERRBUF_SIZE 256
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#define DLT_NULL 0
#define DLT_EN10MB 1
#define DLT_RAW 12

// Function pointer types
typedef int (*pcap_findalldevs_t)(pcap_if_t**, char*);
typedef void (*pcap_freealldevs_t)(pcap_if_t*);
typedef pcap_t* (*pcap_open_live_t)(const char*, int, int, int, char*);
typedef int (*pcap_compile_t)(pcap_t*, struct bpf_program*, const char*, int,
                              bpf_u_int32);
typedef int (*pcap_setfilter_t)(pcap_t*, struct bpf_program*);
typedef int (*pcap_next_ex_t)(pcap_t*, struct pcap_pkthdr**, const u_char**);
typedef int (*pcap_datalink_t)(pcap_t*);
typedef void (*pcap_freecode_t)(struct bpf_program*);
typedef void (*pcap_close_t)(pcap_t*);

// Loaded entry points (defined in sniffer.cpp).
struct PcapApi {
    pcap_findalldevs_t findalldevs = nullptr;
    pcap_freealldevs_t freealldevs = nullptr;
    pcap_open_live_t open_live = nullptr;
    pcap_compile_t compile = nullptr;
    pcap_setfilter_t setfilter = nullptr;
    pcap_next_ex_t next_ex = nullptr;
    pcap_datalink_t datalink = nullptr;
    pcap_freecode_t freecode = nullptr;
    pcap_close_t close = nullptr;
    bool ok() const {
        return findalldevs && freealldevs && open_live && compile &&
               setfilter && next_ex && datalink && close;
    }
};

bool load_pcap_api(PcapApi& api);  // LoadLibrary("wpcap.dll") + GetProcAddress
