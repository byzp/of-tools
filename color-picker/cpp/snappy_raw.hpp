// Raw-format Snappy decompressor — equivalent to python-snappy's
// `snappy.uncompress` (the un-framed format: varint preamble + tag stream).
// Replaces the python-snappy dependency. Returns false on any malformed input
// (the caller treats failure like the Python `except: continue`).
#pragma once
#include <cstdint>
#include <vector>

namespace snap {

inline bool uncompress(const uint8_t* in, size_t inlen, std::vector<uint8_t>& out) {
    size_t pos = 0;

    // Preamble: uncompressed length as a varint.
    uint64_t ulen = 0;
    int shift = 0;
    while (true) {
        if (pos >= inlen) return false;
        uint8_t b = in[pos++];
        ulen |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 63) return false;
    }

    out.clear();
    out.reserve((size_t)ulen);

    while (pos < inlen) {
        uint8_t tag = in[pos++];
        int type = tag & 0x03;
        if (type == 0) {  // literal
            uint32_t len = (uint32_t)(tag >> 2);
            if (len < 60) {
                len += 1;
            } else {
                int nbytes = (int)len - 59;  // 1..4 trailing length bytes
                if (pos + (size_t)nbytes > inlen) return false;
                uint32_t l = 0;
                for (int i = 0; i < nbytes; ++i)
                    l |= (uint32_t)in[pos + i] << (8 * i);
                pos += nbytes;
                len = l + 1;
            }
            if (pos + (size_t)len > inlen) return false;
            out.insert(out.end(), in + pos, in + pos + len);
            pos += len;
        } else {  // copy
            uint32_t len, offset;
            if (type == 1) {  // 1-byte offset, length 4..11
                len = ((tag >> 2) & 0x07) + 4;
                if (pos >= inlen) return false;
                offset = ((uint32_t)(tag >> 5) << 8) | in[pos];
                pos += 1;
            } else if (type == 2) {  // 2-byte offset
                len = (uint32_t)(tag >> 2) + 1;
                if (pos + 2 > inlen) return false;
                offset = (uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8);
                pos += 2;
            } else {  // type == 3, 4-byte offset
                len = (uint32_t)(tag >> 2) + 1;
                if (pos + 4 > inlen) return false;
                offset = (uint32_t)in[pos] | ((uint32_t)in[pos + 1] << 8) |
                         ((uint32_t)in[pos + 2] << 16) | ((uint32_t)in[pos + 3] << 24);
                pos += 4;
            }
            if (offset == 0 || offset > out.size()) return false;
            size_t start = out.size() - offset;
            // Byte-by-byte to allow overlapping copies (RLE). We reserved ulen
            // up front so no reallocation invalidates the source bytes, but copy
            // through a local to be safe regardless.
            for (uint32_t i = 0; i < len; ++i) {
                uint8_t b = out[start + i];
                out.push_back(b);
            }
        }
    }

    return out.size() == (size_t)ulen;
}

}  // namespace snap
