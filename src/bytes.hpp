// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounds-checked little-endian access to a byte range. Every read that would
// leave the range throws ParseError, so malformed files fail cleanly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace tia {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Span {
public:
    Span() = default;
    Span(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const uint8_t* data() const { return p_; }

    bool has(size_t off, size_t len) const { return off <= n_ && len <= n_ - off; }

    void need(size_t off, size_t len) const {
        if (!has(off, len)) throw ParseError("read past end of data");
    }

    uint8_t u8(size_t off) const {
        need(off, 1);
        return p_[off];
    }
    uint16_t u16(size_t off) const {
        need(off, 2);
        return static_cast<uint16_t>(p_[off] | (p_[off + 1] << 8));
    }
    uint32_t u32(size_t off) const {
        need(off, 4);
        return static_cast<uint32_t>(p_[off]) | (static_cast<uint32_t>(p_[off + 1]) << 8) |
               (static_cast<uint32_t>(p_[off + 2]) << 16) | (static_cast<uint32_t>(p_[off + 3]) << 24);
    }
    uint64_t u64(size_t off) const {
        return static_cast<uint64_t>(u32(off)) | (static_cast<uint64_t>(u32(off + 4)) << 32);
    }
    // little-endian unsigned integer of 1..8 bytes
    uint64_t uint(size_t off, size_t len) const {
        need(off, len);
        uint64_t v = 0;
        for (size_t i = 0; i < len && i < 8; ++i) v |= static_cast<uint64_t>(p_[off + i]) << (8 * i);
        return v;
    }

    Span sub(size_t off, size_t len) const {
        need(off, len);
        return Span(p_ + off, len);
    }
    Span from(size_t off) const {
        need(off, 0);
        return Span(p_ + off, n_ - off);
    }

    std::string str(size_t off, size_t len) const {
        need(off, len);
        return std::string(reinterpret_cast<const char*>(p_ + off), len);
    }

    // LEB128-style unsigned varint; advances off.
    uint64_t varint(size_t& off) const {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            uint8_t c = u8(off++);
            v |= static_cast<uint64_t>(c & 0x7f) << shift;
            if (!(c & 0x80)) return v;
        }
        throw ParseError("varint too long");
    }

    // String whose length prefix counts the prefix itself (object attribute data).
    std::string pstrInclusive(size_t off) const {
        size_t p = off;
        uint64_t n = varint(p);
        size_t prefix = p - off;
        if (n < prefix) throw ParseError("bad string length");
        return str(p, static_cast<size_t>(n) - prefix);
    }

    // String whose length prefix does not count itself (system tables); advances off.
    std::string pstrExclusive(size_t& off) const {
        uint64_t n = varint(off);
        if (n > n_) throw ParseError("bad string length");
        std::string s = str(off, static_cast<size_t>(n));
        off += static_cast<size_t>(n);
        return s;
    }

private:
    const uint8_t* p_ = nullptr;
    size_t n_ = 0;
};

}  // namespace tia
