// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "project.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "miniz.h"

namespace tia {

bool Value::asUInt(uint64_t& out) const {
    switch (type) {
        case Type::Bool: out = b ? 1 : 0; return true;
        case Type::Int: out = static_cast<uint64_t>(i); return true;
        case Type::UInt: out = u; return true;
        default: return false;
    }
}

const Value* Value::field(const std::string& name) const {
    if (type != Type::Record) return nullptr;
    for (size_t i = 0; i < names.size() && i < elements.size(); ++i)
        if (names[i] == name) return &elements[i];
    return nullptr;
}

bool Value::truthy() const {
    uint64_t v = 0;
    return asUInt(v) && v != 0;
}

const Value* Object::attr(const std::string& setShortName, const std::string& name) const {
    for (const auto& s : sets) {
        if (s.first != setShortName) continue;
        for (const auto& kv : s.second)
            if (kv.first == name) return &kv.second;
    }
    return nullptr;
}

const Value* Object::expandoValue(const std::string& name) const {
    for (const auto& kv : expando)
        if (kv.first == name) return &kv.second;
    return nullptr;
}

std::string Object::attrString(const std::string& setShortName, const std::string& name) const {
    const Value* v = attr(setShortName, name);
    return (v && (v->type == Value::Type::String || v->type == Value::Type::DateTime)) ? v->s : std::string();
}

bool Object::relationTarget(uint32_t relationId, std::pair<uint32_t, uint64_t>& out) const {
    if (relationId == 0) return false;
    for (const auto& r : relations)
        if (r.relation == relationId) {
            out = {r.targetType, r.targetId};
            return true;
        }
    return false;
}

std::string formatTicks(uint64_t raw) {
    const uint64_t ticks = raw & 0x3fffffffffffffffULL;
    const unsigned kind = static_cast<unsigned>(raw >> 62);  // 1 = UTC
    if (ticks == 0) return std::string();
    const uint64_t ticksPerDay = 864000000000ULL;
    uint64_t days = ticks / ticksPerDay;
    uint64_t rem = ticks % ticksPerDay;
    if (days > 3652058) return std::string();  // beyond year 9999
    // civil date from days since 0001-01-01 (proleptic Gregorian)
    int64_t z = static_cast<int64_t>(days) + 306;  // days since 0000-03-01
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    uint64_t secs = rem / 10000000ULL;
    uint64_t frac = rem % 10000000ULL;
    char buf[48];
    int n = std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02llu:%02llu:%02llu", static_cast<long long>(y),
                          static_cast<long long>(m), static_cast<long long>(d),
                          static_cast<unsigned long long>(secs / 3600),
                          static_cast<unsigned long long>((secs / 60) % 60),
                          static_cast<unsigned long long>(secs % 60));
    std::string out(buf, static_cast<size_t>(n));
    if (frac) {
        n = std::snprintf(buf, sizeof buf, ".%03llu", static_cast<unsigned long long>(frac / 10000));
        out.append(buf, static_cast<size_t>(n));
    }
    if (kind == 1) out += 'Z';
    return out;
}

namespace {

bool inflate(Span in, std::string& out) {
    // The embedded documents are zlib streams of up to a few megabytes.
    mz_ulong cap = static_cast<mz_ulong>(in.size()) * 16 + 4096;
    for (int attempt = 0; attempt < 6; ++attempt) {
        out.resize(cap);
        mz_ulong len = cap;
        int rc = mz_uncompress(reinterpret_cast<unsigned char*>(&out[0]), &len, in.data(),
                               static_cast<mz_ulong>(in.size()));
        if (rc == MZ_OK) {
            out.resize(len);
            return true;
        }
        if (rc != MZ_BUF_ERROR) return false;
        cap *= 4;
    }
    return false;
}

}  // namespace

// Blob layout after the outer length prefix:
//   u8 kind
//   kind 0 / 1: u64 total size, u32 (unknown), u16 page size, u32 page count,
//               varint n, n x u32 bitmap of pages that are present, then the
//               pages: raw (kind 0) or varint length + zlib stream (kind 1).
//               Absent pages are all zero.
//   kind 4:     varint length, bytes.
bool decodeBlob(const std::string& raw, std::string& out) {
    out.clear();
    if (raw.empty()) return true;
    // Absent pages cost no input, so the size has to be bounded here. The
    // documents this is used for are a few hundred kilobytes at most.
    constexpr uint64_t kMaxBlob = 64ull * 1024 * 1024;
    try {
        Span s(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        const uint8_t kind = s.u8(0);
        if (kind == 4) {
            size_t off = 1;
            uint64_t len = s.varint(off);
            out = s.str(off, static_cast<size_t>(len));
            return true;
        }
        if (kind != 0 && kind != 1) return false;
        uint64_t total = s.u64(1);
        uint16_t pageSize = s.u16(13);
        uint32_t count = s.u32(15);
        size_t off = 19;
        uint64_t words = s.varint(off);
        if (total > kMaxBlob || words > raw.size() / 4 || (count && pageSize == 0)) return false;
        if (static_cast<uint64_t>(count) * pageSize < total || count > total / (pageSize ? pageSize : 1) + 1) return false;
        const size_t bitmap = off;
        off += static_cast<size_t>(words) * 4;
        s.need(bitmap, static_cast<size_t>(words) * 4);
        out.reserve(static_cast<size_t>(total));
        uint64_t left = total;
        std::string page;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t want = static_cast<size_t>(left < pageSize ? left : pageSize);
            const bool present = i / 32 < words && ((s.u32(bitmap + 4 * (i / 32)) >> (i % 32)) & 1);
            if (!present) {
                out.append(want, '\0');
            } else if (kind == 0) {
                out += s.str(off, want);
                off += want;
            } else {
                uint64_t len = s.varint(off);
                Span z = s.sub(off, static_cast<size_t>(len));
                off += static_cast<size_t>(len);
                page.resize(pageSize);
                mz_ulong got = pageSize;
                if (mz_uncompress(reinterpret_cast<unsigned char*>(&page[0]), &got, z.data(),
                                  static_cast<mz_ulong>(z.size())) != MZ_OK || got < want)
                    return false;
                out.append(page, 0, want);
            }
            left -= want;
        }
        return left == 0;
    } catch (const ParseError&) {
        return false;
    }
}

namespace {

// True when the root element is <MetaInfo>. Older projects also carry a
// StorageMetaInfoXML document, which describes the same types after
// resolution and is not needed here.
bool isMetaInfoDocument(const std::string& xml) {
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        if (pos + 1 >= xml.size()) return false;
        const char c = xml[pos + 1];
        if (c == '?' || c == '!') {
            ++pos;
            continue;
        }
        size_t end = xml.find_first_of(" \t\r\n/>", pos + 1);
        if (end == std::string::npos) return false;
        std::string name = xml.substr(pos + 1, end - pos - 1);
        const size_t colon = name.find(':');
        if (colon != std::string::npos) name.erase(0, colon + 1);
        return name == "MetaInfo";
    }
    return false;
}

}  // namespace

Project::Project(Container container) : container_(std::move(container)) { loadSystemObjects(); }

const Block* Project::live(uint32_t type, uint64_t id) const {
    auto it = container_.latest().find({type, id});
    if (it == container_.latest().end()) return nullptr;
    const Block& b = container_.blocks()[it->second];
    return b.deleted() ? nullptr : &b;
}

void Project::loadSystemObjects() {
    for (const auto& kv : container_.latest()) {
        const Block& b = container_.blocks()[kv.second];
        if (!container_.isSystem(b) || b.deleted()) continue;
        Span p;
        try {
            p = container_.systemPayload(b);
        } catch (const ParseError&) {
            continue;
        }
        if (p.size() >= 2 && p.u8(0) == 0x78) {
            std::string xml;
            if (!inflate(p, xml)) continue;
            if (xml.compare(0, 3, "\xef\xbb\xbf") == 0) xml.erase(0, 3);
            if (!isMetaInfoDocument(xml)) continue;
            try {
                meta_.load(xml);
                metaXml_.push_back(std::move(xml));
            } catch (const ParseError&) {
            }
            continue;
        }
        if (container_.layout() == Layout::V14 && b.type != 0x70011) continue;
        loadExpandoTable(p);
    }
}

// Key table for the expando attributes of one object type:
//   u32 length (of the whole payload), u32 object type id, u32 count, u32 count,
//   then per key: u32 key, string name, u32 value type id.
// In the older layout system objects carry no type, so a table is recognised
// by parsing cleanly to the last byte.
bool Project::loadExpandoTable(Span p) {
    try {
        if (p.size() < 16) return false;
        uint32_t len = p.u32(0), objType = p.u32(4), count = p.u32(8);
        if (len != p.size() || count != p.u32(12) || count == 0 || count > 100000) return false;
        std::map<uint32_t, ExpandoKey> keys;
        size_t off = 16;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t key = p.u32(off);
            off += 4;
            ExpandoKey k;
            k.name = p.pstrExclusive(off);
            k.typeId = p.u32(off);
            off += 4;
            keys[key] = std::move(k);
        }
        if (off != p.size()) return false;
        auto& dst = expando_[objType];
        for (auto& kv : keys) dst[kv.first] = std::move(kv.second);
        return true;
    } catch (const ParseError&) {
        return false;
    }
}

// A stored structure, at `offset` in a segment:
//   size (u32; u16 in the older layout), counting itself
//   one fixed-size field per element, in the order of the type model
//   the strings and blobs those fields point to, offsets from the size field
// Read only when every element is a plain value and the fields and their
// strings cover the structure exactly; anything else stays unread.
bool Project::readRecord(const TypeDef& type, Span seg, size_t offset, Value& out) const {
    const size_t head = container_.layout() == Layout::V11 ? 2 : 4;
    if (!seg.has(offset, head)) return false;
    const size_t size = head == 2 ? seg.u16(offset) : seg.u32(offset);
    if (size < head || !seg.has(offset, size)) return false;
    const Span rec = seg.sub(offset, size);
    Value v;
    v.type = Value::Type::Record;
    std::vector<std::pair<size_t, size_t>> spans;
    size_t pos = head;
    for (const auto& e : type.elements) {
        const TypeDef* et = meta_.find(e.type);
        const Storage st = meta_.storage(e.type);
        switch (st.kind) {
            case ValueKind::Bool:
            case ValueKind::UInt:
            case ValueKind::Int:
            case ValueKind::Float:
            case ValueKind::DateTime:
            case ValueKind::Guid:
            case ValueKind::Enum:
                break;
            case ValueKind::String:
            case ValueKind::Blob: {
                if (pos + 4 > size) return false;
                const size_t at = rec.u32(pos);
                if (at == 0) break;
                if (at >= size) return false;
                size_t p = at;
                const uint64_t n = rec.varint(p);
                if (n < p - at || n > size - at) return false;
                spans.emplace_back(at, at + static_cast<size_t>(n));
                break;
            }
            default:
                return false;  // nested structures, texts, references, unknown types
        }
        if (st.size == 0 || pos + st.size > size) return false;
        v.names.push_back(e.name);
        v.elements.push_back(readValue(st, st.kind == ValueKind::Enum ? et : nullptr, rec, pos));
        pos += st.size;
    }
    std::sort(spans.begin(), spans.end());
    size_t end = pos;
    for (const auto& sp : spans) {
        if (sp.first != end) return false;
        end = sp.second;
    }
    if (end != size) return false;
    out = std::move(v);
    return true;
}

// A stored array of structures: u32 size, u32 count, count x u32 offset from
// the size field, then the structures.
bool Project::readList(const TypeDef& type, Span seg, size_t offset, Value& out) const {
    const TypeDef* element = meta_.find(type.elementType);
    if (!element || element->kind != TypeKind::Structure) return false;
    if (!seg.has(offset, 8)) return false;
    const size_t size = seg.u32(offset);
    const size_t count = seg.u32(offset + 4);
    if (size < 8 || !seg.has(offset, size) || count > 100000 || 8 + 4 * count > size) return false;
    const Span arr = seg.sub(offset, size);
    Value v;
    v.type = Value::Type::List;
    const size_t head = container_.layout() == Layout::V11 ? 2 : 4;
    std::vector<std::pair<size_t, size_t>> spans;
    for (size_t i = 0; i < count; ++i) {
        const size_t at = arr.u32(8 + 4 * i);
        if (at < 8 + 4 * count || at >= size) return false;
        Value record;
        if (!readRecord(*element, arr, at, record)) return false;
        spans.emplace_back(at, at + (head == 2 ? arr.u16(at) : arr.u32(at)));
        v.elements.push_back(std::move(record));
    }
    // the structures follow the offsets without a gap, to the end
    std::sort(spans.begin(), spans.end());
    size_t end = 8 + 4 * count;
    for (const auto& sp : spans) {
        if (sp.first != end) return false;
        end = sp.second;
    }
    if (end != size) return false;
    out = std::move(v);
    return true;
}

Value Project::readValue(const Storage& st, const TypeDef* enumType, Span seg, size_t pos) const {
    Value v;
    switch (st.kind) {
        case ValueKind::ObjectRef:
            return v;
        case ValueKind::Bool:
            v.type = Value::Type::Bool;
            v.b = seg.u8(pos) != 0;
            return v;
        case ValueKind::UInt:
        case ValueKind::Unknown:
            v.type = Value::Type::UInt;
            v.u = seg.uint(pos, st.size);
            return v;
        case ValueKind::Enum: {
            v.type = Value::Type::UInt;
            v.u = seg.uint(pos, st.size);
            if (enumType) {
                auto it = enumType->constants.find(static_cast<int64_t>(v.u));
                if (it != enumType->constants.end()) {
                    v.type = Value::Type::String;
                    v.s = it->second;
                }
            }
            return v;
        }
        case ValueKind::Int: {
            uint64_t raw = seg.uint(pos, st.size);
            if (st.size < 8 && (raw >> (8 * st.size - 1)) & 1) raw |= ~0ULL << (8 * st.size);
            v.type = Value::Type::Int;
            v.i = static_cast<int64_t>(raw);
            return v;
        }
        case ValueKind::Float: {
            v.type = Value::Type::Float;
            if (st.size == 4) {
                uint32_t raw = seg.u32(pos);
                float f;
                std::memcpy(&f, &raw, 4);
                v.f = f;
            } else {
                uint64_t raw = seg.u64(pos);
                std::memcpy(&v.f, &raw, 8);
            }
            return v;
        }
        case ValueKind::DateTime: {
            std::string s = formatTicks(seg.u64(pos));
            if (!s.empty()) {
                v.type = Value::Type::DateTime;
                v.s = std::move(s);
            }
            return v;
        }
        case ValueKind::Guid: {
            static const char* hex = "0123456789abcdef";
            Span g = seg.sub(pos, 16);
            v.type = Value::Type::String;
            for (size_t k = 0; k < 16; ++k) {
                v.s += hex[g.u8(k) >> 4];
                v.s += hex[g.u8(k) & 15];
            }
            return v;
        }
        case ValueKind::String:
        case ValueKind::Blob:
        case ValueKind::Text:
        case ValueKind::Relative: {
            uint32_t off = seg.u32(pos);
            if (off == 0 || off >= seg.size()) return v;
            if (st.kind == ValueKind::Text) return readText(seg, off);
            if (st.kind == ValueKind::String) {
                v.type = Value::Type::String;
                v.s = seg.pstrInclusive(off);
            } else if (st.kind == ValueKind::Blob) {
                size_t p = off;
                uint64_t n = seg.varint(p);
                size_t prefix = p - off;
                if (n < prefix) throw ParseError("bad blob length");
                v.type = Value::Type::Bytes;
                v.s = seg.str(p, static_cast<size_t>(n) - prefix);
            } else {
                v.type = Value::Type::Opaque;
                // enumType is the structure or array type here
                try {
                    Value read;
                    if (enumType && enumType->kind == TypeKind::Structure && readRecord(*enumType, seg, off, read)) return read;
                    if (enumType && enumType->kind == TypeKind::Array && readList(*enumType, seg, off, read)) return read;
                } catch (const ParseError&) {
                }
            }
            return v;
        }
    }
    return v;
}

// A text in several languages (pe:CoreTextAttributeT), at `offset` in a segment:
//   varint length (counting itself)
//   u32 total size, u32 used size, u32 0xFFFFFFFF, u32 count
//   count x u16 language id        0xFFFF = no language
//   count x u32 offset             from the total-size field
//   at each offset: u32 length, UTF-8 bytes
// Anything that does not fit this is reported as opaque.
Value Project::readText(Span seg, size_t offset) {
    Value v;
    v.type = Value::Type::Opaque;
    try {
        size_t p = offset;
        const uint64_t n = seg.varint(p);
        if (n < p - offset) return v;
        Span t = seg.sub(p, static_cast<size_t>(n) - (p - offset));
        const uint32_t used = t.u32(4), count = t.u32(12);
        if (t.u32(8) != 0xffffffffu || used > t.size() || count > t.size() / 6) return v;
        std::vector<std::pair<uint16_t, std::string>> texts;
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t language = t.u16(16 + 2 * static_cast<size_t>(i));
            const uint32_t at = t.u32(16 + 2 * static_cast<size_t>(count) + 4 * static_cast<size_t>(i));
            if (at > used || used - at < 4) return v;
            const uint32_t len = t.u32(at);
            if (len > used - at - 4) return v;
            texts.emplace_back(language, t.str(at + 4, len));
        }
        v.type = Value::Type::Text;
        v.texts = std::move(texts);
        for (const auto& e : v.texts)
            if (e.first == 0xffff && !e.second.empty()) v.s = e.second;
        for (const auto& e : v.texts)
            if (v.s.empty()) v.s = e.second;
    } catch (const ParseError&) {
    }
    return v;
}

namespace {

// UTF-16LE up to a zero terminator, as UTF-8.
std::string utf16z(Span s, size_t pos) {
    std::string out;
    for (;; pos += 2) {
        uint32_t c = s.u16(pos);
        if (c == 0) break;
        if (c >= 0xd800 && c < 0xdc00) {
            const uint32_t low = s.u16(pos + 2);
            if (low >= 0xdc00 && low < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                pos += 2;
            }
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return out;
}

}  // namespace

// Expando segment:
//   u32 used length, u32 capacity, u24 count + u8 flags
//   older layout: u32 object type id
//   newer layout: u8 kind, u8 unknown, u32 offset of the value slots
//   kind 2 (and the older layout): `count` u32 keys, looked up in the key
//     table of the object type
//   kind 1: `count` u32 value type ids, then `count` u32 offsets of the
//     attribute names, which are stored right here (UTF-16, zero-terminated)
//   `count` u32 value slots, then variable-size data. A slot holds the value
//   itself for fixed types of up to 4 bytes, otherwise an offset into the
//   variable data (0xFFFFFFFF = no value).
void Project::decodeExpando(uint32_t objectType, Span seg, Object& out) const {
    auto table = expando_.find(objectType);
    const size_t head = container_.layout() == Layout::V11 ? 16 : 18;
    if (seg.size() < head) return;
    // The top byte of the count field is a flag (seen as 0x01 on segments with
    // reserved space before the variable data), not part of the count.
    uint32_t count = seg.u32(8) & 0x00ffffffu;
    if (count > seg.size() / 8) throw ParseError("bad expando count");
    const bool named = container_.layout() == Layout::V14 && seg.u8(12) == 1;
    const size_t keys = head;
    size_t slots = head + 4 * static_cast<size_t>(count);
    if (named) {
        slots = seg.u32(14);
        if (slots < head + 8 * static_cast<size_t>(count) || slots > seg.size()) throw ParseError("bad expando layout");
    }
    const size_t var = slots + 4 * static_cast<size_t>(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key = seg.u32(keys + 4 * i);
        ExpandoKey inlineKey;
        const ExpandoKey* ek = nullptr;
        if (named) {
            inlineKey.typeId = key;
            inlineKey.name = utf16z(seg, seg.u32(keys + 4 * (static_cast<size_t>(count) + i)));
            ek = &inlineKey;
        } else if (table != expando_.end()) {
            auto it = table->second.find(key);
            if (it != table->second.end()) ek = &it->second;
        }
        if (!ek) {
            out.expando.emplace_back("#" + std::to_string(key), Value());
            continue;
        }
        Storage st;
        const TypeDef* et = nullptr;
        if (const char* basic = MetaModel::basicTypeName(ek->typeId)) {
            st = meta_.storage(basic);
        } else if (const TypeDef* t = meta_.findById(ek->typeId)) {
            st = meta_.storage(t->name);
            if (t->kind == TypeKind::Enumeration) et = t;
        } else {
            out.expando.emplace_back(ek->name, Value());
            continue;
        }
        const size_t slot = slots + 4 * i;
        Value v;
        const bool indirect = st.size > 4 || st.kind == ValueKind::String || st.kind == ValueKind::Blob ||
                              st.kind == ValueKind::Relative || st.kind == ValueKind::Text;
        if (!indirect) {
            v = readValue(st, et, seg, slot);
        } else {
            uint32_t off = seg.u32(slot);
            if (off != 0xffffffffu) {
                size_t p = var + off;
                if (st.kind == ValueKind::String) {
                    v.type = Value::Type::String;
                    v.s = seg.pstrInclusive(p);
                } else if (st.kind == ValueKind::Blob) {
                    size_t q = p;
                    uint64_t n = seg.varint(q);
                    if (n < q - p) throw ParseError("bad blob length");
                    v.type = Value::Type::Bytes;
                    v.s = seg.str(q, static_cast<size_t>(n) - (q - p));
                } else if (st.kind == ValueKind::Text) {
                    v = readText(seg, p);
                } else if (st.kind == ValueKind::Relative) {
                    v.type = Value::Type::Opaque;
                } else {
                    v = readValue(st, et, seg, p);
                }
            }
        }
        out.expando.emplace_back(ek->name, std::move(v));
    }
}

// Slots after the attribute sets hold relations. The first two are keyed
// lists (u16 size, u16 count, then {relation id, target type, target id});
// further slots are per-relation lists marked by 0x7FFFFFFF that only carry
// {target type, target id}.
void Project::decodeRelations(Span block, const Block& b, size_t firstSlot, Object& out) const {
    for (size_t si = firstSlot; si < b.slotCount; ++si) {
        uint32_t so = block.u32(b.headerLen + 4 * si);
        if (so == 0) continue;
        try {
            if (block.has(so, 12) && block.u32(so + 4) == 0x7fffffffu) {
                uint32_t count = block.u32(so + 8);
                if (!block.has(so + 12, static_cast<size_t>(count) * 12)) throw ParseError("relation list overruns block");
                for (uint32_t i = 0; i < count; ++i) {
                    RelationEntry e;
                    e.targetType = block.u32(so + 12 + 12 * i);
                    e.targetId = block.u64(so + 16 + 12 * i);
                    e.slot = static_cast<uint32_t>(si);
                    out.relations.push_back(e);
                }
            } else if (si < firstSlot + 2) {
                uint16_t size = block.u16(so), count = block.u16(so + 2);
                if (4 + 16 * static_cast<size_t>(count) > size) continue;
                for (uint16_t i = 0; i < count; ++i) {
                    RelationEntry e;
                    e.relation = block.u32(so + 4 + 16 * i);
                    e.targetType = block.u32(so + 8 + 16 * i);
                    e.targetId = block.u64(so + 12 + 16 * i);
                    e.slot = static_cast<uint32_t>(si);
                    out.relations.push_back(e);
                }
            }
        } catch (const ParseError& e) {
            out.problems.push_back(std::string("relation slot ") + std::to_string(si) + ": " + e.what());
        }
    }
}

bool Project::decode(const Block& b, Object& out) const {
    if (container_.isSystem(b)) return false;
    const TypeDef* t = meta_.findById(b.type);
    if (!t || t->kind != TypeKind::ObjectType) return false;
    out = Object();
    out.type = b.type;
    out.id = b.id;
    out.def = t;
    if (b.deleted()) return true;

    Span block = container_.blockData(b);
    const auto& layout = meta_.layout(*t);
    if (b.slotCount < layout.size() || !block.has(b.headerLen, 4 * static_cast<size_t>(b.slotCount))) {
        out.problems.push_back("slot table does not match the type model (" + std::to_string(b.slotCount) +
                               " slots, model expects at least " + std::to_string(layout.size()) + ")");
        return true;
    }
    for (size_t i = 0; i < layout.size(); ++i) {
        const LayoutSet& ls = layout[i];
        uint32_t so = block.u32(b.headerLen + 4 * i);
        if (so == 0 || !ls.set) continue;
        const std::string shortName = ls.set->shortName();
        try {
            uint32_t len = block.u32(so);
            Span seg = block.sub(so, len);
            if (ls.set->expando) {
                decodeExpando(b.type, seg, out);
                continue;
            }
            NamedValues vals;
            size_t pos = 4;
            for (const auto& la : ls.attributes) {
                if (!la.persisted) continue;
                const TypeDef* et = la.storage.kind == ValueKind::Enum || la.storage.kind == ValueKind::Relative
                                        ? meta_.find(la.def->type)
                                        : nullptr;
                if (la.storage.size) vals.emplace_back(la.def->name, readValue(la.storage, et, seg, pos));
                pos += la.storage.size;
            }
            out.sets.emplace_back(shortName, std::move(vals));
        } catch (const ParseError& e) {
            out.problems.push_back(shortName + ": " + e.what());
        }
    }
    decodeRelations(block, b, layout.size(), out);
    return true;
}

}  // namespace tia
