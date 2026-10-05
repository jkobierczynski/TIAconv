// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "container.hpp"

#include <cstring>

#include "sha256.hpp"

namespace tia {

const char* layoutName(Layout l) { return l == Layout::V11 ? "v11" : "v14"; }

namespace {

constexpr size_t kHeaderV11 = 46;
constexpr size_t kHeaderV14 = 98;
constexpr size_t kBlockHeaderV11 = 28;
constexpr size_t kBlockHeaderV14 = 44;
constexpr size_t kHashLen = 32;
constexpr size_t kMarkerLen = 20;  // 0x0A, 10 characters, 8-byte timestamp, 0xFF

const char kCommit[] = "\x0a$$COMMIT$$";
const char kClose[] = "\x0a##CLOSE###";

}  // namespace

Container Container::parse(std::vector<uint8_t> data, const ContainerOptions& opt) {
    Container c;
    c.data_ = std::move(data);
    Span d = c.data();

    if (d.size() < kHeaderV11 + kBlockHeaderV11 || d.u16(0) != 0x40)
        throw ParseError("not a PLF file (unexpected file header)");

    // The 16-bit field after the leading 0x0040 is 1 in the older layout and 0
    // in the newer one; both headers end in 0xFF.
    uint16_t variant = d.u16(2);
    size_t off;
    size_t headerLen;
    if (variant == 1 && d.u8(kHeaderV11 - 1) == 0xff) {
        c.layout_ = Layout::V11;
        off = kHeaderV11;
        headerLen = kBlockHeaderV11;
    } else if (variant == 0 && d.size() >= kHeaderV14 && d.u8(kHeaderV14 - 1) == 0xff) {
        c.layout_ = Layout::V14;
        off = kHeaderV14;
        headerLen = kBlockHeaderV14;
    } else {
        throw ParseError("unsupported PLF layout (file header variant " + std::to_string(variant) + ")");
    }

    const bool hashed = c.layout_ == Layout::V14;
    c.hashesVerified_ = hashed && opt.verifyHashes;
    if (c.hashesVerified_) {
        auto h = sha256(d.data(), 65);
        if (std::memcmp(h.data(), d.data() + 65, kHashLen) != 0) ++c.hashErrors_;
    }

    while (off < d.size()) {
        if (!hashed && d.has(off, kMarkerLen)) {
            const char* p = reinterpret_cast<const char*>(d.data() + off);
            bool commit = std::memcmp(p, kCommit, 11) == 0;
            bool close = !commit && std::memcmp(p, kClose, 11) == 0;
            if (commit || close) {
                SaveMarker m;
                m.offset = off;
                m.kind = commit ? "commit" : "close";
                m.ticks = d.u64(off + 11);
                c.markers_.push_back(m);
                off += kMarkerLen;
                continue;
            }
        }
        if (!d.has(off, headerLen)) {
            c.complete_ = false;
            c.stopReason_ = "truncated block header at offset " + std::to_string(off);
            break;
        }
        Block b;
        b.offset = off;
        b.size = d.u32(off);
        b.type = d.u32(off + 4);
        b.id = d.u64(off + 8);
        b.flags = d.u16(off + 24);
        b.slotCount = d.u8(off + 26);
        b.headerLen = static_cast<uint32_t>(headerLen);
        size_t total = static_cast<size_t>(b.size) + (hashed ? kHashLen : 0);
        if (b.size < headerLen || !d.has(off, total)) {
            c.complete_ = false;
            c.stopReason_ = "invalid block size at offset " + std::to_string(off);
            break;
        }
        if (c.hashesVerified_) {
            auto h = sha256(d.data() + off, b.size);
            if (std::memcmp(h.data(), d.data() + off + b.size, kHashLen) != 0) ++c.hashErrors_;
        }
        c.latest_[{b.type, b.id}] = c.blocks_.size();
        c.blocks_.push_back(b);
        off += total;
    }
    return c;
}

bool Container::isSystem(const Block& b) const {
    if (layout_ == Layout::V11) return b.type == 0;
    return b.type >= 0x70000 && b.type < 0x70100;
}

Span Container::systemPayload(const Block& b) const {
    Span body = blockData(b).from(b.headerLen);
    uint32_t len = body.u32(0);
    return body.sub(4, len);
}

}  // namespace tia
