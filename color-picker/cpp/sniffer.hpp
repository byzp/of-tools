// Packet sniffer: replaces the scapy-based capture in main.py. Dynamically loads
// wpcap.dll, finds the active interface, captures TCP on ports 11001-11003,
// reassembles the custom framed protocol, and dispatches OutfitDyeParam payloads
// (msg_id == 1652) to a callback.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

// Called when an OutfitColorantSelectRsp (msg_id 1652) is decoded.
using DyeCallback =
    std::function<void(uint32_t picture_id, const std::vector<double>& params)>;

// Run the protocol framer over a single reassembled byte buffer, dispatching any
// complete msg_id==1652 message. Mirrors main.py _process_buf (including the
// 20KB header guard and the "stop after first match" return). Exposed for tests.
void frame_and_dispatch(std::vector<uint8_t>& buf, const DyeCallback& cb);

// Start capturing on a background thread. Performs wpcap load + interface
// detection + filter setup synchronously (so failures are reported here, like
// main.py's start_sniffer) and returns false on any setup failure.
bool start_sniffer(const DyeCallback& cb);

