// Minimal protobuf wire-format parser (parse-only) for the three messages this
// tool reads off the wire:
//
//   PacketHead { uint32 packet_id=1; msg_id=2; flag=3; body_len=4; ... }
//   OutfitColorantSelectRsp { StatusCode status=1; OutfitDyeParam param=2 }
//   OutfitDyeParam { uint32 picture_id=1; repeated double params=2; float uvy=3; bool is_dye=4 }
//
// Replaces the generated `net_pb2` module. Host is assumed little-endian (x64),
// matching the IEEE-754 little-endian wire encoding for fixed64/fixed32.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace pb {

// Wire types
enum { WT_VARINT = 0, WT_FIXED64 = 1, WT_LEN = 2, WT_FIXED32 = 5 };

// Read a base-128 varint. Advances p. Returns false on truncation/overflow.
inline bool read_varint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            out = v;
            return true;
        }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

// Skip the value of a field with the given wire type. Advances p.
inline bool skip_value(const uint8_t*& p, const uint8_t* end, uint32_t wt) {
    switch (wt) {
        case WT_VARINT: {
            uint64_t tmp;
            return read_varint(p, end, tmp);
        }
        case WT_FIXED64:
            if (end - p < 8) return false;
            p += 8;
            return true;
        case WT_FIXED32:
            if (end - p < 4) return false;
            p += 4;
            return true;
        case WT_LEN: {
            uint64_t len;
            if (!read_varint(p, end, len)) return false;
            if ((uint64_t)(end - p) < len) return false;
            p += len;
            return true;
        }
        default:
            return false;  // groups (3/4) unsupported; not used here
    }
}

// Parse PacketHead, extracting the fields the framer needs.
inline bool parse_packet_head(const uint8_t* data, size_t len,
                              uint32_t& msg_id, uint32_t& flag,
                              uint32_t& body_len) {
    msg_id = 0;
    flag = 0;
    body_len = 0;
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(p, end, tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7);
        if (wt == WT_VARINT) {
            uint64_t v;
            if (!read_varint(p, end, v)) return false;
            if (field == 2) msg_id = (uint32_t)v;
            else if (field == 3) flag = (uint32_t)v;
            else if (field == 4) body_len = (uint32_t)v;
        } else {
            if (!skip_value(p, end, wt)) return false;
        }
    }
    return true;
}

// Parse OutfitDyeParam, extracting picture_id (field 1) and the repeated double
// params (field 2). Handles both packed (length-delimited) and the unlikely
// unpacked (one fixed64 per element) encodings.
inline bool parse_dye_param(const uint8_t* data, size_t len,
                            uint32_t& picture_id, std::vector<double>& params) {
    picture_id = 0;
    params.clear();
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(p, end, tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7);
        if (field == 1 && wt == WT_VARINT) {
            uint64_t v;
            if (!read_varint(p, end, v)) return false;
            picture_id = (uint32_t)v;
        } else if (field == 2 && wt == WT_LEN) {  // packed doubles
            uint64_t blen;
            if (!read_varint(p, end, blen)) return false;
            if ((uint64_t)(end - p) < blen) return false;
            if (blen % 8 != 0) return false;
            size_t count = (size_t)(blen / 8);
            params.reserve(params.size() + count);
            for (size_t i = 0; i < count; ++i) {
                double d;
                std::memcpy(&d, p, 8);
                p += 8;
                params.push_back(d);
            }
        } else if (field == 2 && wt == WT_FIXED64) {  // unpacked double
            double d;
            std::memcpy(&d, p, 8);
            p += 8;
            params.push_back(d);
        } else {
            if (!skip_value(p, end, wt)) return false;
        }
    }
    return true;
}

// Parse OutfitColorantSelectRsp: find field 2 (param, OutfitDyeParam) and parse
// it. Returns false if the param submessage is absent or malformed.
inline bool parse_colorant_rsp(const uint8_t* data, size_t len,
                               uint32_t& picture_id,
                               std::vector<double>& params) {
    const uint8_t* p = data;
    const uint8_t* end = data + len;
    bool found = false;
    while (p < end) {
        uint64_t tag;
        if (!read_varint(p, end, tag)) return false;
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7);
        if (field == 2 && wt == WT_LEN) {
            uint64_t blen;
            if (!read_varint(p, end, blen)) return false;
            if ((uint64_t)(end - p) < blen) return false;
            if (!parse_dye_param(p, (size_t)blen, picture_id, params)) return false;
            p += blen;
            found = true;
        } else {
            if (!skip_value(p, end, wt)) return false;
        }
    }
    return found;
}

}  // namespace pb
