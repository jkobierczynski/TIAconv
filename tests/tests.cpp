// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-contained tests. The container test builds a small synthetic project
// in memory, so no Siemens project file is needed (or redistributed).
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "container.hpp"
#include "history.hpp"
#include "inventory.hpp"
#include "meta.hpp"
#include "miniz.h"
#include "output.hpp"
#include "program.hpp"
#include "project.hpp"
#include "sha256.hpp"
#include "xml.hpp"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        ++checks;                                                                \
        bool threw = false;                                                      \
        try {                                                                    \
            (void)(expr);                                                        \
        } catch (const tia::ParseError&) {                                       \
            threw = true;                                                        \
        }                                                                        \
        if (!threw) {                                                            \
            ++failures;                                                          \
            std::printf("FAIL %s:%d  expected ParseError: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void set32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void append(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }
void appendHash(Bytes& file, size_t from) {
    auto h = tia::sha256(file.data() + from, file.size() - from);
    file.insert(file.end(), h.begin(), h.end());
}

std::string hex(const std::array<uint8_t, 32>& h) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : h) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

void testBytes() {
    const uint8_t raw[] = {0x34, 0x12, 0x78, 0x56, 0xac, 0x02, 0x04, 'a', 'b', 'c', 0x02, 'x', 'y'};
    tia::Span s(raw, sizeof raw);
    CHECK(s.u16(0) == 0x1234);
    CHECK(s.u32(0) == 0x56781234u);
    size_t off = 4;
    CHECK(s.varint(off) == 300 && off == 6);       // 0xac 0x02
    CHECK(s.pstrInclusive(6) == "abc");            // length 4 counts the prefix
    off = 10;
    CHECK(s.pstrExclusive(off) == "xy" && off == 13);
    CHECK_THROWS(s.u32(11));
    CHECK_THROWS(s.sub(10, 4));
    CHECK(s.uint(0, 3) == 0x781234u);
}

void testSha256() {
    CHECK(hex(tia::sha256(nullptr, 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* abc = "abc";
    CHECK(hex(tia::sha256(reinterpret_cast<const uint8_t*>(abc), 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the padding spills into a second block
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(hex(tia::sha256(reinterpret_cast<const uint8_t*>(two), 56)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void testXml() {
    auto root = tia::parseXml("<?xml version=\"1.0\"?><!-- c --><ns0:A x=\"1 &amp; 2\" y='&#x41;&lt;'>text"
                              "<B k=\"v>w\"/><C><D/></C><![CDATA[<junk>]]></ns0:A>");
    CHECK(root->name == "A");
    CHECK(root->attrOr("x", "") == "1 & 2");
    CHECK(root->attrOr("y", "") == "A<");
    CHECK(root->children.size() == 2);
    CHECK(root->children[0]->name == "B" && root->children[0]->attrIs("k", "v>w"));
    CHECK(root->children[1]->children.size() == 1);
    CHECK_THROWS(tia::parseXml("<A><B></A"));
    CHECK_THROWS(tia::parseXml("   "));
}

void testTicks() {
    CHECK(tia::formatTicks(0).empty());
    // 2014-05-12T10:24:02.621 UTC, taken from a real save marker (kind bits = UTC)
    CHECK(tia::formatTicks(0x08d13be36e11ab05ULL | (1ULL << 62)).substr(0, 10) == "2014-05-12");
    CHECK(tia::formatTicks(621355968000000000ULL) == "1970-01-01T00:00:00");
    CHECK(tia::formatTicks(630822816000000000ULL | (1ULL << 62)) == "2000-01-01T00:00:00Z");
    CHECK(tia::formatTicks(631139040005000000ULL) == "2001-01-01T00:00:00.500");
}

const char kMeta[] =
    "<MetaInfo xmlns=\"http://www.siemens.com/Automation/2004/04/ObjectFrame/Meta\">"
    "<Package name=\"P\" id=\"0x00000001\"><Namespace name=\"T\">"
    "<Enumeration name=\"Colour\" id=\"0x00005001\" base=\"xs:unsignedByte\">"
    "<Constant name=\"Red\" value=\"1\"/><Constant name=\"Blue\" value=\"2\"/></Enumeration>"
    "<AttributeSet name=\"IB\" id=\"0x00003001\" persistent=\"true\">"
    "<Attribute name=\"Name\" id=\"0\" type=\"xs:string\" constant=\"true\" virtual=\"true\"/>"
    "<Attribute name=\"Owner\" id=\"1\" type=\"Thing\"/>"
    "<Attribute name=\"Count\" id=\"2\" type=\"xs:int\"/>"
    "<Attribute name=\"Paint\" id=\"3\" type=\"Colour\"/>"
    "<Attribute name=\"Busy\" id=\"4\" type=\"xs:boolean\" intrinsic=\"true\"/>"
    "</AttributeSet>"
    "<AttributeSet name=\"IX\" id=\"0x00003002\" persistent=\"true\" expando=\"true\"/>"
    "<AttributeSet name=\"IA\" id=\"0x00003003\" persistent=\"true\">"
    "<Attribute name=\"Stamp\" id=\"0\" type=\"xs:dateTime\"/></AttributeSet>"
    "<ObjectType name=\"Base\" id=\"0x00001000\">"
    "<Implements ref=\"IB\"><Attribute name=\"Name\" constant=\"false\"/></Implements>"
    "<Implements ref=\"T.IX\"/>"
    "<Relation name=\"Parent\" id=\"0x00002001\" cardinality=\"1\" behaviourType=\"x.parent\"><Target ref=\"T.Base\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"Thing\" id=\"0x00001001\"><Base ref=\"T.Base\" primary=\"true\"/>"
    "<Implements ref=\"IB\"><Attribute name=\"Name\"/></Implements><Implements ref=\"IA\"/>"
    // a relation that declares its other direction in place, and one that only names it
    "<Relation name=\"Labels\" id=\"0x00002002\" cardinality=\"*\" behaviourType=\"x.weak\"><Target ref=\"T.Label\"/>"
    "<Inverse name=\"LabelOf\" id=\"0x00002003\" cardinality=\"1\"/></Relation>"
    "<Relation name=\"Spare\" id=\"0x00002004\" cardinality=\"1\" behaviourType=\"x.weak\"><Target ref=\"T.Label\"/>"
    "<Inverse ref=\"Parent\"/></Relation>"
    "<Relation name=\"Twin\" id=\"0x00002005\" cardinality=\"1\" behaviourType=\"x.weak\"><Target ref=\"T.Label\"/>"
    "<Inverse name=\"Own\" id=\"0x00002007\" cardinality=\"1\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"Label\" id=\"0x00001002\"><Base ref=\"T.Base\" primary=\"true\"/>"
    "<Relation name=\"Own\" id=\"0x00002006\" cardinality=\"1\" behaviourType=\"x.weak\"><Target ref=\"T.Thing\"/></Relation>"
    "</ObjectType>"
    "</Namespace></Package></MetaInfo>";

void testMeta() {
    tia::MetaModel m;
    m.load(kMeta);
    const tia::TypeDef* thing = m.findById(0x1001);
    CHECK(thing && thing->name == "T.Thing");
    CHECK(m.derivesFrom("T.Thing", "T.Base") && !m.derivesFrom("T.Base", "T.Thing"));
    CHECK(m.relationId("Base", "Parent") == 0x2001);
    CHECK(m.relation(0x2001) && m.relation(0x2001)->behaviour == "parent");
    // The other direction of Thing.Labels is a relation of Label with an id
    // of its own. An <Inverse ref=...> declares nothing new, and a relation
    // the type declares itself wins over an inverse of the same name.
    CHECK(m.relationId("Thing", "Labels") == 0x2002 && m.relationId("Label", "LabelOf") == 0x2003);
    CHECK(m.relation(0x2003) && m.relation(0x2003)->inverse && m.relation(0x2003)->owner == "T.Label");
    CHECK(m.relation(0x2002) && !m.relation(0x2002)->inverse);
    CHECK(m.relationId("Label", "Parent") == 0 && m.relationId("Thing", "LabelOf") == 0);
    CHECK(m.relationId("Label", "Own") == 0x2006 && m.relation(0x2007) && m.relation(0x2007)->inverse);
    CHECK(m.storage("T.Colour").size == 1 && m.storage("T.Colour").kind == tia::ValueKind::Enum);
    CHECK(m.storage("T.Thing").size == 0);
    const auto& lay = m.layout(*thing);
    // slot order is by qualified attribute-set name
    CHECK(lay.size() == 3 && lay[0].name == "T.IA" && lay[1].name == "T.IB" && lay[2].name == "T.IX");
    // Name: constant in the set, not restated by Thing, switched on by the primary base
    CHECK(lay[1].attributes.size() == 5);
    CHECK(lay[1].attributes[0].persisted);   // Name
    CHECK(lay[1].attributes[2].persisted);   // Count
    CHECK(!lay[1].attributes[4].persisted);  // Busy is intrinsic
}

Bytes block(uint32_t type, uint64_t id, uint16_t flags, uint8_t slots, const Bytes& body) {
    Bytes b;
    put32(b, static_cast<uint32_t>(44 + body.size()));
    put32(b, type);
    put64(b, id);
    put64(b, 0);
    put16(b, flags);
    b.push_back(slots);
    b.push_back(5);
    for (int i = 0; i < 16; ++i) b.push_back(static_cast<uint8_t>(i));
    append(b, body);
    return b;
}

Bytes systemBody(const Bytes& payload) {
    Bytes b;
    put32(b, static_cast<uint32_t>(payload.size()));
    append(b, payload);
    b.push_back(0xff);
    return b;
}

Bytes syntheticProject() {
    Bytes f(98, 0);
    f[0] = 0x40;
    f[4] = 1;
    auto hh = tia::sha256(f.data(), 65);
    std::memcpy(f.data() + 65, hh.data(), 32);
    f[97] = 0xff;

    auto add = [&](const Bytes& blk) {
        size_t from = f.size();
        append(f, blk);
        appendHash(f, from);
    };

    // type model
    Bytes z(mz_compressBound(sizeof kMeta));
    mz_ulong zl = static_cast<mz_ulong>(z.size());
    mz_compress(z.data(), &zl, reinterpret_cast<const unsigned char*>(kMeta), sizeof kMeta - 1);
    z.resize(zl);
    add(block(0x70000, 1, 0, 0, systemBody(z)));

    // expando key table for T.Thing: 1 -> "Speed" (xs:int), 2 -> "Label" (xs:string), 3 -> "Addr" (xs:long)
    Bytes t;
    put32(t, 0);
    put32(t, 0x1001);
    put32(t, 3);
    put32(t, 3);
    const struct { uint32_t key; const char* name; uint32_t type; } keys[] = {
        {1, "Speed", 0x80000005}, {2, "Label", 0x8000000b}, {3, "Addr", 0x80000006}};
    for (const auto& k : keys) {
        put32(t, k.key);
        t.push_back(static_cast<uint8_t>(std::strlen(k.name)));
        for (const char* p = k.name; *p; ++p) t.push_back(static_cast<uint8_t>(*p));
        put32(t, k.type);
    }
    set32(t, 0, static_cast<uint32_t>(t.size()));
    add(block(0x70011, 1024, 0, 0, systemBody(t)));

    // object T.Thing #7: slots IA, IB, IX, relations(single), relations(multi)
    Bytes body(5 * 4, 0);
    auto slot = [&](int i) { set32(body, 4 * static_cast<size_t>(i), static_cast<uint32_t>(44 + body.size())); };

    slot(0);  // IA: Stamp
    put32(body, 12);
    put64(body, 630822816000000000ULL | (1ULL << 62));

    slot(1);  // IB: Name (string), Owner (object reference, no bytes), Count, Paint
    put32(body, 4 + 4 + 4 + 1 + 6);
    put32(body, 13);                       // Name -> offset 13 in the segment
    put32(body, static_cast<uint32_t>(-5));  // Count
    body.push_back(2);                     // Paint = Blue
    body.push_back(6);                     // length prefix counts itself
    for (char ch : std::string("Motor")) body.push_back(static_cast<uint8_t>(ch));

    slot(2);  // IX: expando
    {
        Bytes x;
        put32(x, 0);  // used length, patched below
        put32(x, 0);  // capacity
        put32(x, 3);  // count
        for (int i = 0; i < 6; ++i) x.push_back(0);
        put32(x, 1); put32(x, 2); put32(x, 3);           // keys
        put32(x, 1500); put32(x, 0); put32(x, 4);        // Speed inline, Label @0, Addr @4
        x.push_back(4); x.push_back('P'); x.push_back('L'); x.push_back('C');
        put64(x, 0xc0a80102);                            // 192.168.1.2
        set32(x, 0, static_cast<uint32_t>(x.size()));
        set32(x, 4, static_cast<uint32_t>(x.size()));
        append(body, x);
    }

    slot(3);  // keyed relation list
    put16(body, 4 + 16);
    put16(body, 1);
    put32(body, 0x2001);
    put32(body, 0x1001);
    put64(body, 3);

    add(block(0x1001, 7, 0, 5, body));

    // object #8 exists, then is deleted by a later tombstone
    add(block(0x1001, 8, 0, 5, body));
    add(block(0x1001, 8, 4, 0, Bytes{0xff}));
    return f;
}

void testProject() {
    Bytes file = syntheticProject();
    tia::ContainerOptions opt;
    opt.verifyHashes = true;
    tia::Project p(tia::Container::parse(file, opt));
    const tia::Container& c = p.container();
    CHECK(c.layout() == tia::Layout::V14);
    CHECK(c.blocks().size() == 5 && c.complete());
    CHECK(c.hashesVerified() && c.hashErrors() == 0);
    CHECK(p.metaDocuments() == 1 && p.expandoTables() == 1);

    const tia::Block* b = p.live(0x1001, 7);
    CHECK(b != nullptr);
    CHECK(p.live(0x1001, 8) == nullptr);  // deleted
    if (b) {
        tia::Object o;
        CHECK(p.decode(*b, o));
        CHECK(o.problems.empty());
        CHECK(o.attrString("IB", "Name") == "Motor");
        const tia::Value* count = o.attr("IB", "Count");
        CHECK(count && count->type == tia::Value::Type::Int && count->i == -5);
        CHECK(o.attrString("IB", "Paint") == "Blue");
        CHECK(o.attr("IB", "Owner") == nullptr);
        CHECK(o.attrString("IA", "Stamp") == "2000-01-01T00:00:00Z");
        const tia::Value* speed = o.expandoValue("Speed");
        CHECK(speed && speed->i == 1500);
        const tia::Value* label = o.expandoValue("Label");
        CHECK(label && label->s == "PLC");
        const tia::Value* addr = o.expandoValue("Addr");
        CHECK(addr && addr->i == 0xc0a80102);
        std::pair<uint32_t, uint64_t> target;
        CHECK(o.relationTarget(0x2001, target) && target.first == 0x1001 && target.second == 3);
    }

    // a flipped byte is caught by the hash check and must not crash the reader
    Bytes bad = file;
    bad[bad.size() - 60] ^= 0x55;
    tia::Container damaged = tia::Container::parse(bad, opt);
    CHECK(damaged.hashErrors() >= 1);

    // truncation ends the block list cleanly
    Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() - 40));
    tia::Container shortFile = tia::Container::parse(cut, opt);
    CHECK(!shortFile.complete() && shortFile.blocks().size() == 4);

    CHECK_THROWS(tia::Container::parse(Bytes(200, 0x11), opt));
}

// Every prefix and a spread of single-byte corruptions of the synthetic
// project must be handled without crashing.
void testRobustness() {
    Bytes file = syntheticProject();
    size_t survived = 0;
    auto tryOne = [&](const Bytes& data) {
        try {
            tia::Project p(tia::Container::parse(data, {}));
            for (const auto& kv : p.container().latest()) {
                tia::Object o;
                try {
                    p.decode(p.container().blocks()[kv.second], o);
                } catch (const tia::ParseError&) {
                }
            }
        } catch (const tia::ParseError&) {
        }
        ++survived;
    };
    for (size_t n = 0; n < file.size(); n += 7) tryOne(Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n)));
    for (size_t i = 98; i < file.size(); i += 3) {
        Bytes m = file;
        m[i] = static_cast<uint8_t>(m[i] ^ (0x01u << (i % 8)));
        tryOne(m);
        m[i] = 0xff;
        tryOne(m);
    }
    CHECK(survived > 100);
}


// The order in which a type hierarchy is searched for the statement that
// decides whether an attribute is stored: depth first, but a type reached
// more than once counts at its last position, so a base comes after every
// type derived from it.
//
//        A      A says X is stored, C says it is constant, B says nothing.
//       / \     D derives from B, then C. Searching D, B, A, C would find A
//      B   C    first; the order that matches real projects is D, B, C, A.
//       \ /
//        D
const char kDiamond[] =
    "<MetaInfo><Package name=\"P\" id=\"0x1\"><Namespace name=\"T\">"
    "<AttributeSet name=\"IS\" id=\"0x3001\" persistent=\"true\">"
    "<Attribute name=\"X\" id=\"0\" type=\"xs:int\"/><Attribute name=\"Y\" id=\"1\" type=\"xs:int\" constant=\"true\"/>"
    "<Attribute name=\"Z\" id=\"2\" type=\"xs:int\"/></AttributeSet>"
    "<ObjectType name=\"A\" id=\"0x1000\"><Implements ref=\"IS\">"
    "<Attribute name=\"X\" constant=\"false\"/><Attribute name=\"Y\" constant=\"false\"/></Implements></ObjectType>"
    "<ObjectType name=\"B\" id=\"0x1001\"><Base ref=\"T.A\" primary=\"true\"/>"
    "<Implements ref=\"IS\"><Attribute name=\"X\"/></Implements></ObjectType>"
    "<ObjectType name=\"C\" id=\"0x1002\"><Base ref=\"T.A\" primary=\"true\"/>"
    "<Implements ref=\"IS\"><Attribute name=\"X\" constant=\"true\"/></Implements></ObjectType>"
    "<ObjectType name=\"D\" id=\"0x1003\"><Base ref=\"T.B\" primary=\"true\"/><Base ref=\"T.C\"/></ObjectType>"
    "<ObjectType name=\"E\" id=\"0x1004\"><Base ref=\"T.B\" primary=\"true\"/></ObjectType>"
    "</Namespace></Package></MetaInfo>";

void testStorageRule() {
    tia::MetaModel m;
    m.load(kDiamond);
    auto persisted = [&](uint32_t type, size_t attribute) {
        const auto& lay = m.layout(*m.findById(type));
        return lay.size() == 1 && lay[0].attributes.size() == 3 && lay[0].attributes[attribute].persisted;
    };
    CHECK(!persisted(0x1003, 0));  // D: C's "constant" is found before A's "stored"
    CHECK(persisted(0x1004, 0));   // E: only B and A, B does not decide
    CHECK(persisted(0x1003, 1));   // Y: constant in the set, switched on by A
    CHECK(persisted(0x1003, 2));   // Z: never mentioned, the set's default applies
    CHECK(m.derivesFromShort("T.D", "A") && m.derivesFromShort("T.D", "C") && !m.derivesFromShort("T.E", "C"));
}

Bytes varint(uint64_t v) {
    Bytes b;
    while (v >= 0x80) {
        b.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
    return b;
}

std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }

Bytes deflated(const std::string& in) {
    Bytes z(mz_compressBound(static_cast<mz_ulong>(in.size())));
    mz_ulong zl = static_cast<mz_ulong>(z.size());
    mz_compress(z.data(), &zl, reinterpret_cast<const unsigned char*>(in.data()), static_cast<mz_ulong>(in.size()));
    z.resize(zl);
    return z;
}

// Stored form of a large value: 4 a plain run of bytes, 0 pages, 1 compressed pages.
Bytes blobPlain(const std::string& data) {
    Bytes b{4};
    append(b, varint(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

Bytes blobPaged(const std::string& data, uint16_t pageSize, bool compressed) {
    const uint32_t pages = static_cast<uint32_t>((data.size() + pageSize - 1) / pageSize);
    Bytes b{static_cast<uint8_t>(compressed ? 1 : 0)};
    put64(b, data.size());
    put32(b, 0);
    put16(b, pageSize);
    put32(b, pages);
    const uint32_t words = (pages + 31) / 32;
    append(b, varint(words));
    std::vector<uint32_t> bitmap(words, 0);
    Bytes body;
    for (uint32_t i = 0; i < pages; ++i) {
        std::string page = data.substr(static_cast<size_t>(i) * pageSize, pageSize);
        if (page.find_first_not_of('\0') == std::string::npos) continue;  // all zero: left out
        bitmap[i / 32] |= 1u << (i % 32);
        if (compressed) {
            page.resize(pageSize, '\0');
            Bytes z = deflated(page);
            append(body, varint(z.size()));
            append(body, z);
        } else {
            body.insert(body.end(), page.begin(), page.end());
        }
    }
    for (uint32_t w : bitmap) put32(b, w);
    append(b, body);
    return b;
}

void testBlob() {
    std::string out;
    CHECK(tia::decodeBlob(std::string(), out) && out.empty());
    CHECK(tia::decodeBlob(str(blobPlain("<Root/>")), out) && out == "<Root/>");
    std::string big(300, 'x');  // length needs a two-byte prefix
    CHECK(tia::decodeBlob(str(blobPlain(big)), out) && out == big);

    std::string data;
    for (int i = 0; i < 100; ++i) data += static_cast<char>('a' + i % 26);
    data += std::string(64, '\0');  // one page that is not stored at all
    data += "tail";
    CHECK(tia::decodeBlob(str(blobPaged(data, 32, false)), out) && out == data);
    CHECK(tia::decodeBlob(str(blobPaged(data, 32, true)), out) && out == data);
    CHECK(tia::decodeBlob(str(blobPaged(data, 4096, true)), out) && out == data);

    CHECK(!tia::decodeBlob(std::string("\x07zzz", 4), out));  // unknown kind
    Bytes cut = blobPaged(data, 32, true);
    cut.resize(cut.size() - 5);
    CHECK(!tia::decodeBlob(str(cut), out));
    // a claimed size far beyond what is there must be refused, not allocated
    Bytes huge = blobPaged(data, 32, false);
    huge[8] = 0x7f;
    CHECK(!tia::decodeBlob(str(huge), out));
    for (size_t n = 0; n < cut.size(); ++n) tia::decodeBlob(str(Bytes(cut.begin(), cut.begin() + static_cast<std::ptrdiff_t>(n))), out);
}

// ---- tags and data blocks -------------------------------------------------

const char kProgramMeta[] =
    "<MetaInfo><Package name=\"P\" id=\"0x1\"><Namespace name=\"M\">"
    "<AttributeSet name=\"ICoreAttributes\" id=\"0x3001\" persistent=\"true\">"
    "<Attribute name=\"Name\" id=\"0\" type=\"xs:string\"/>"
    "<Attribute name=\"Comment\" id=\"1\" type=\"pe:CoreTextAttributeT\"/></AttributeSet>"
    "<AttributeSet name=\"ICommentsExpandoAttributeSet\" id=\"0x3008\" persistent=\"true\" expando=\"true\"/>"
    "<AttributeSet name=\"IRootExpando\" id=\"0x3009\" persistent=\"true\" expando=\"true\"/>"
    "<AttributeSet name=\"IDataTypeContent\" id=\"0x3002\" persistent=\"true\">"
    "<Attribute name=\"DataTypeID\" id=\"0\" type=\"xs:int\"/><Attribute name=\"Scope\" id=\"1\" type=\"xs:string\"/></AttributeSet>"
    "<AttributeSet name=\"IGeneralBlockSourceData\" id=\"0x3003\" persistent=\"true\">"
    "<Attribute name=\"Number\" id=\"0\" type=\"xs:int\"/><Attribute name=\"OnlySymbolicAccess\" id=\"1\" type=\"xs:boolean\"/></AttributeSet>"
    "<AttributeSet name=\"IGeneralDataBlockSourceData\" id=\"0x3004\" persistent=\"true\">"
    "<Attribute name=\"Type\" id=\"0\" type=\"xs:string\"/><Attribute name=\"OfName\" id=\"1\" type=\"xs:string\"/></AttributeSet>"
    "<AttributeSet name=\"IInterfacePartData\" id=\"0x3005\" persistent=\"true\">"
    "<Attribute name=\"Payload\" id=\"0\" type=\"pe:BlobT\"/></AttributeSet>"
    "<AttributeSet name=\"IStructureItem\" id=\"0x3006\" persistent=\"true\">"
    "<Attribute name=\"DisplayTypeName\" id=\"0\" type=\"xs:string\"/></AttributeSet>"
    "<AttributeSet name=\"ITagAddress\" id=\"0x3007\" persistent=\"true\">"
    "<Attribute name=\"LogicalAddress\" id=\"0\" type=\"xs:string\"/></AttributeSet>"
    "<ObjectType name=\"CoreObject\" id=\"0x1000\"><Implements ref=\"ICoreAttributes\"/>"
    "<Relation name=\"Target\" id=\"0x2101\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "<Relation name=\"Environment\" id=\"0x2102\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"ProjectData\" id=\"0x1001\"><Base ref=\"M.CoreObject\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"PlcData\" id=\"0x1002\"><Base ref=\"M.CoreObject\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"EAMTZTagTableData\" id=\"0x1003\"><Base ref=\"M.CoreObject\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"TagTableContentData\" id=\"0x1004\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Relation name=\"TagTable\" id=\"0x2103\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"EAMTZTagData\" id=\"0x1005\"><Base ref=\"M.TagTableContentData\" primary=\"true\"/>"
    "<Implements ref=\"ITagAddress\"/><Implements ref=\"IStructureItem\"/></ObjectType>"
    "<ObjectType name=\"BlockInterfaceBaseData\" id=\"0x1006\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Relation name=\"CurrentInterface\" id=\"0x2104\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "<Relation name=\"InterfaceComments\" id=\"0x2106\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"CodeBlockData\" id=\"0x100b\"><Base ref=\"M.BlockInterfaceBaseData\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"InterfaceCommentsPartData\" id=\"0x100c\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"ICommentsExpandoAttributeSet\"/></ObjectType>"
    "<ObjectType name=\"DataBlockData\" id=\"0x1007\"><Base ref=\"M.BlockInterfaceBaseData\" primary=\"true\"/>"
    "<Implements ref=\"IGeneralBlockSourceData\"/><Implements ref=\"IGeneralDataBlockSourceData\"/></ObjectType>"
    "<ObjectType name=\"InterfaceVersionRootData\" id=\"0x1008\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"IInterfacePartData\"/><Implements ref=\"IRootExpando\"/>"
    "<Relation name=\"UsedParts\" id=\"0x2105\" cardinality=\"n\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"InterfacePartData\" id=\"0x1009\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"IInterfacePartData\"/></ObjectType>"
    "<ObjectType name=\"DataTypeItemData\" id=\"0x100a\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"IDataTypeContent\"/></ObjectType>"
    // constants, and the device items a hardware identifier stands for
    "<AttributeSet name=\"IDefaultStrategyData\" id=\"0x300a\" persistent=\"true\">"
    "<Attribute name=\"DefaultValue\" id=\"0\" type=\"xs:string\"/></AttributeSet>"
    "<AttributeSet name=\"IStructureRoot\" id=\"0x300b\" persistent=\"true\">"
    "<Attribute name=\"IsSystemDefined\" id=\"0\" type=\"xs:boolean\"/></AttributeSet>"
    "<ObjectType name=\"ConstantTagData\" id=\"0x100d\"><Base ref=\"M.TagTableContentData\" primary=\"true\"/>"
    "<Implements ref=\"IDefaultStrategyData\"/><Implements ref=\"IStructureItem\"/></ObjectType>"
    "<ObjectType name=\"SimaticConstantTagData\" id=\"0x100e\"><Base ref=\"M.ConstantTagData\" primary=\"true\"/>"
    "<Implements ref=\"IStructureRoot\"/></ObjectType>"
    "<ObjectType name=\"BaseDeviceData\" id=\"0x100f\"><Base ref=\"M.CoreObject\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"BaseDeviceItemData\" id=\"0x1010\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Relation name=\"Parent\" id=\"0x2107\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "<Relation name=\"ConstantTags\" id=\"0x2108\" cardinality=\"*\" behaviourType=\"x.ref\"><Target ref=\"M.ConstantTagData\"/>"
    "<Inverse name=\"DeviceItem\" id=\"0x2109\" cardinality=\"1\"/></Relation>"
    "</ObjectType>"
        // HMI tags and the list that links them to PLC tags
    "<Enumeration name=\"AcquisitionModes\" id=\"0x5010\" base=\"xs:int\"><Constant name=\"OnDemand\" value=\"3\"/>"
    "<Constant name=\"Visible\" value=\"4\"/></Enumeration>"
    "<Structure name=\"LinkHandle\" id=\"0x4101\"><Element name=\"Index\" type=\"xs:int\"/></Structure>"
    "<Array name=\"CoreArrayString\" id=\"0x4102\" type=\"xs:string\"/>"
    "<Structure name=\"LinkInformation\" id=\"0x4103\"><Element name=\"Coid\" type=\"pe:BlobT\"/>"
    "<Element name=\"ConsumerIndex\" type=\"xs:int\"/><Element name=\"Handle\" type=\"LinkHandle\"/>"
    "<Element name=\"IsConnected\" type=\"xs:boolean\"/><Element name=\"NamePath\" type=\"CoreArrayString\"/>"
    "<Element name=\"ProviderIndex\" type=\"xs:int\"/><Element name=\"QuotedNamePath\" type=\"xs:string\"/></Structure>"
    "<Array name=\"LinkArray\" id=\"0x4104\" type=\"LinkInformation\"/>"
    "<AttributeSet name=\"IScopedLinkMaintainerData\" id=\"0x3010\" persistent=\"true\">"
    "<Attribute name=\"Links\" id=\"0\" type=\"LinkArray\"/><Attribute name=\"Version\" id=\"1\" type=\"xs:uint\"/></AttributeSet>"
    "<AttributeSet name=\"IHmiTagAttributes\" id=\"0x3011\" persistent=\"true\">"
    "<Attribute name=\"AcquisitionTriggerMode\" id=\"0\" type=\"AcquisitionModes\"/></AttributeSet>"
    "<AttributeSet name=\"IHmiTagStructureAttributes\" id=\"0x3012\" persistent=\"true\">"
    "<Attribute name=\"StartValue\" id=\"0\" type=\"xs:string\"/></AttributeSet>"
    "<AttributeSet name=\"ISoftlinkAttributes\" id=\"0x3013\" persistent=\"true\" expando=\"true\"/>"
    "<ObjectType name=\"ScopedLinkMaintainerData\" id=\"0x1020\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"IScopedLinkMaintainerData\"/>"
    "<Relation name=\"Consumer\" id=\"0x2201\" cardinality=\"*\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/>"
    "<Inverse name=\"InverseConsumer\" id=\"0x2202\" cardinality=\"*\"/></Relation></ObjectType>"
    "<ObjectType name=\"HmiConnectionData\" id=\"0x1021\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Relation name=\"ConnectionPoint\" id=\"0x2203\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"HmiCycleData\" id=\"0x1022\"><Base ref=\"M.CoreObject\" primary=\"true\"/></ObjectType>"
    "<ObjectType name=\"HmiTagData\" id=\"0x1023\"><Base ref=\"M.TagTableContentData\" primary=\"true\"/>"
    "<Implements ref=\"IStructureItem\"/><Implements ref=\"ITagAddress\"/><Implements ref=\"IHmiTagAttributes\"/>"
    "<Implements ref=\"IHmiTagStructureAttributes\"/><Implements ref=\"ISoftlinkAttributes\"/>"
    "<Relation name=\"IHmiTagAttributes_Connection\" id=\"0x2204\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.HmiConnectionData\"/></Relation>"
    "<Relation name=\"IHmiTagAttributes_AcquisitionCycle\" id=\"0x2205\" cardinality=\"1\" behaviourType=\"x.ref\"><Target ref=\"M.HmiCycleData\"/></Relation>"
    "<Relation name=\"Members\" id=\"0x2206\" cardinality=\"*\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "<ObjectType name=\"HmiStructureMemberTagData\" id=\"0x1024\"><Base ref=\"M.CoreObject\" primary=\"true\"/>"
    "<Implements ref=\"IStructureItem\"/>"
    "<Relation name=\"Members\" id=\"0x2207\" cardinality=\"*\" behaviourType=\"x.ref\"><Target ref=\"M.CoreObject\"/></Relation>"
    "</ObjectType>"
    "</Namespace></Package></MetaInfo>";

struct Field {
    char kind;  // s string, b blob, r structure or array, i 32-bit number, o boolean
    std::string text;
    uint32_t number;
};
Field fs(const std::string& v) { return {'s', v, 0}; }
Field fb(const Bytes& v) { return {'b', str(v), 0}; }
Field fr(const Bytes& v) { return {'r', str(v), 0}; }
Field fi(uint32_t v) { return {'i', std::string(), v}; }
Field fo(bool v) { return {'o', std::string(), v ? 1u : 0u}; }

// One attribute-set segment: length, fixed part, then the strings and blobs
// the fixed part points to. Both length prefixes count themselves.
Bytes segment(const std::vector<Field>& fields) {
    Bytes fixed, var;
    size_t fixedSize = 4;
    for (const auto& f : fields) fixedSize += f.kind == 'o' ? 1 : 4;
    for (const auto& f : fields) {
        if (f.kind == 'i') {
            put32(fixed, f.number);
        } else if (f.kind == 'o') {
            fixed.push_back(static_cast<uint8_t>(f.number));
        } else {
            put32(fixed, static_cast<uint32_t>(fixedSize + var.size()));
            if (f.kind == 'r') {  // a structure or an array brings its own size
                var.insert(var.end(), f.text.begin(), f.text.end());
                continue;
            }
            size_t prefix = 1;
            while (varint(f.text.size() + prefix).size() != prefix) ++prefix;
            append(var, varint(f.text.size() + prefix));
            var.insert(var.end(), f.text.begin(), f.text.end());
        }
    }
    Bytes seg;
    put32(seg, static_cast<uint32_t>(fixedSize + var.size()));
    append(seg, fixed);
    append(seg, var);
    return seg;
}

// A text in several languages, as stored for comments.
Bytes coreText(const std::vector<std::pair<uint16_t, std::string>>& texts) {
    Bytes b;
    put32(b, 0);  // total size, patched below
    put32(b, 0);  // used size, patched below
    put32(b, 0xffffffffu);
    put32(b, static_cast<uint32_t>(texts.size()));
    for (const auto& t : texts) put16(b, t.first);
    const size_t offsets = b.size();
    for (size_t i = 0; i < texts.size(); ++i) put32(b, 0);
    for (size_t i = 0; i < texts.size(); ++i) {
        set32(b, offsets + 4 * i, static_cast<uint32_t>(b.size()));
        put32(b, static_cast<uint32_t>(texts[i].second.size()));
        b.insert(b.end(), texts[i].second.begin(), texts[i].second.end());
    }
    set32(b, 4, static_cast<uint32_t>(b.size()));
    b.insert(b.end(), {0x29, 0x10, 0x22, 0x00});  // real files have leftovers and spare room here
    b.resize(b.size() + 12, 0);
    set32(b, 0, static_cast<uint32_t>(b.size()));
    return b;
}
Field ft(const std::vector<std::pair<uint16_t, std::string>>& texts) { return {'b', str(coreText(texts)), 0}; }
Field fnone() { return {'i', std::string(), 0}; }  // a string, blob or text that is not there

// Expando segment of the kind that carries its attribute names itself:
// member comments, named by the member's ID path.
Bytes commentSegment(const std::vector<std::pair<std::string, std::string>>& comments) {
    const size_t n = comments.size();
    Bytes b;
    put32(b, 0);  // used, patched below
    put32(b, 0);  // capacity, patched below
    put32(b, static_cast<uint32_t>(n));
    b.push_back(1);  // names inline
    b.push_back(0);
    put32(b, 0);  // offset of the value slots, patched below
    for (size_t i = 0; i < n; ++i) put32(b, 0x8000001fu);
    const size_t names = b.size();
    for (size_t i = 0; i < n; ++i) put32(b, 0);
    for (size_t i = 0; i < n; ++i) {
        set32(b, names + 4 * i, static_cast<uint32_t>(b.size()));
        for (char c : comments[i].first) put16(b, static_cast<uint8_t>(c));
        put16(b, 0);
    }
    const size_t slots = b.size();
    set32(b, 14, static_cast<uint32_t>(slots));
    for (size_t i = 0; i < n; ++i) put32(b, 0);
    const size_t var = b.size();
    for (size_t i = 0; i < n; ++i) {
        set32(b, slots + 4 * i, static_cast<uint32_t>(b.size() - var));
        Bytes t = coreText({{0x0409, comments[i].second}});
        size_t prefix = 1;
        while (varint(t.size() + prefix).size() != prefix) ++prefix;
        append(b, varint(t.size() + prefix));
        append(b, t);
    }
    set32(b, 0, static_cast<uint32_t>(b.size()));
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

// Expando segment whose keys are looked up in the key table of the object
// type; here with a single blob value.
Bytes keyedBlobSegment(uint32_t key, const Bytes& blob) {
    Bytes b;
    put32(b, 0);  // used, patched below
    put32(b, 0);  // capacity, patched below
    put32(b, 1);
    b.push_back(2);  // names in the key table
    b.push_back(0);
    put32(b, 0);
    put32(b, key);
    put32(b, 0);  // value at offset 0 of the variable data
    size_t prefix = 1;
    while (varint(blob.size() + prefix).size() != prefix) ++prefix;
    append(b, varint(blob.size() + prefix));
    append(b, blob);
    set32(b, 0, static_cast<uint32_t>(b.size()));
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

struct Rel {
    uint32_t relation;
    uint32_t type;
    uint64_t id;
};

Bytes object(uint32_t type, uint64_t id, const std::vector<Bytes>& sets, const std::vector<Rel>& singles,
             const std::vector<Rel>& multi) {
    const size_t slots = sets.size() + 2;
    Bytes body(slots * 4, 0);
    size_t n = 0;
    for (const auto& seg : sets) {
        if (!seg.empty()) set32(body, 4 * n, static_cast<uint32_t>(44 + body.size()));  // empty: no segment
        ++n;
        append(body, seg);
    }
    for (const auto* list : {&singles, &multi}) {
        set32(body, 4 * n++, static_cast<uint32_t>(44 + body.size()));
        put16(body, static_cast<uint16_t>(4 + 16 * list->size()));
        put16(body, static_cast<uint16_t>(list->size()));
        for (const auto& r : *list) {
            put32(body, r.relation);
            put32(body, r.type);
            put64(body, r.id);
        }
    }
    return block(type, id, 0, static_cast<uint8_t>(slots), body);
}

Bytes programProject() {
    Bytes f(98, 0);
    f[0] = 0x40;
    f[4] = 1;
    auto hh = tia::sha256(f.data(), 65);
    std::memcpy(f.data() + 65, hh.data(), 32);
    f[97] = 0xff;
    auto add = [&](const Bytes& blk) {
        size_t from = f.size();
        append(f, blk);
        appendHash(f, from);
    };
    add(block(0x70000, 1, 0, 0, systemBody(deflated(kProgramMeta))));
    {
        // key table of InterfaceVersionRootData: 7 -> "Values" (blob)
        Bytes t;
        put32(t, 0);
        put32(t, 0x1008);
        put32(t, 1);
        put32(t, 1);
        put32(t, 7);
        t.push_back(6);
        for (char c : std::string("Values")) t.push_back(static_cast<uint8_t>(c));
        put32(t, 0x8000000d);
        set32(t, 0, static_cast<uint32_t>(t.size()));
        add(block(0x70011, 1024, 0, 0, systemBody(t)));
    }

    enum : uint32_t { Project = 0x1001, Plc, Table, Content, TagT, IfBase, Db, Root, Part, DataType, Code, Comments };
    enum : uint32_t { Target = 0x2101, Environment, TagTable, Interface, UsedParts, InterfaceComments };
    auto name = [](const std::string& n) { return segment({fs(n), fnone()}); };
    auto commented = [](const std::string& n, const std::vector<std::pair<uint16_t, std::string>>& texts) {
        return segment({fs(n), ft(texts)});
    };
    const Rel inProject{Environment, Project, 1}, onPlc{Target, Plc, 2};

    add(object(Project, 1, {name("Demo")}, {}, {}));
    add(object(Plc, 2, {name("PLC_1")}, {inProject}, {}));
    add(object(Table, 3, {name("Default tag table")}, {inProject, onPlc}, {}));
    // data type numbers, as every project lists them
    add(object(DataType, 10, {name("Bool"), segment({fi(1), fs("S71500")})}, {}, {}));
    add(object(DataType, 11, {name("Int"), segment({fi(5), fs("S71500")})}, {}, {}));
    add(object(DataType, 12, {name("Byte"), segment({fi(2), fs("S71500")})}, {}, {}));

    // a tag, and the copy of a tag that belongs to a library object
    add(object(TagT, 20,
               {commented("Start", {{0x0409, "Start button"}, {0x0407, "Starttaste"}, {0xffff, "Start button"}}),
                segment({fs("Bool")}), segment({fs("%I0.0")})},
               {inProject, onPlc, {TagTable, Table, 3}}, {}));
    add(object(TagT, 21, {name("Copy"), segment({fs("Bool")}), segment({fs("%I0.1")})},
               {{Environment, Plc, 2}, onPlc, {TagTable, Table, 3}}, {}));

    // global block DB7 with standard access: elementary members, an anonymous
    // structure in a part of its own, a named type with defaults, an array
    const std::string global =
        "\xef\xbb\xbf<Root><Member ID=\"51\" Name=\"Run\" RID=\"0x02000001\" StdO=\"0\"/>"
        "<Member ID=\"52\" Name=\"Speed\" RID=\"0x02000005\" StdO=\"16\"/>"
        "<Member ID=\"53\" Name=\"Cfg\" RID=\"0x93010000\" SubPartIndex=\"1\" StdO=\"32\"/>"
        "<Member ID=\"54\" Name=\"Motor\" RID=\"0x02000300\" Type=\"&quot;MotorUDT&quot;\" SubPartIndex=\"2\" StdO=\"64\"/>"
        "<Member ID=\"55\" Name=\"Buf\" RID=\"0x02010002\" Type=\"Array[0..3] of Byte\" StdO=\"96\"/>"
        "<Member ID=\"56\" Name=\"Gone\" RID=\"0x02000300\" Type=\"&quot;Missing&quot;\" SubPartIndex=\"0\" StdO=\"128\"/>"
        "<Member ID=\"57\" Name=\"Odd\" RID=\"0x02000999\" StdO=\"160\"/>"
        "<Values><V p=\"52\" v=\"1500\"/><V p=\"53:51\" v=\"true\"/><V p=\"54:52\" v=\"9\"/></Values></Root>";
    const std::string cfg = "<Structure><Member ID=\"51\" Name=\"On\" RID=\"0x02000001\" StdO=\"0\"/>"
                            "<Member ID=\"52\" Name=\"Lim\" RID=\"0x02000005\" StdO=\"16\"/></Structure>";
    const std::string udt = "<Root><Member ID=\"51\" Name=\"Id\" RID=\"0x02000005\" StdO=\"0\"/>"
                            "<Member ID=\"52\" Name=\"Gain\" RID=\"0x02000005\" StdO=\"16\"/></Root>";
    // a PLC data type keeps its defaults in an attribute of the root object
    const std::string udtDefaults = "\xef\xbb\xbf<Values><V p=\"51\" h=\"1\" v=\"3\"/><V p=\"52\" v=\"4\"/></Values>";
    add(object(Db, 30,
               {commented("Data", {{0x0407, "nur deutsch"}}), segment({fi(7), fo(false)}),
                segment({fs("SharedDB"), fs("")})},
               {inProject, onPlc, {Interface, Root, 100}, {InterfaceComments, Comments, 120}}, {}));
    // comments of the block's members, of the data type's members (kept with
    // the type) and of the function block's
    add(object(Comments, 120, {commentSegment({{"52", "Speed in rpm"}, {"53:51", "enable"}, {"54:51", "not this one"}}), name("")}, {}, {}));
    add(object(Code, 40, {name("MotorUDT")}, {inProject, onPlc, {Interface, Root, 102}, {InterfaceComments, Comments, 121}}, {}));
    add(object(Comments, 121, {commentSegment({{"51", "ident"}}), name("")}, {}, {}));
    add(object(Code, 41, {name("FB1")}, {inProject, onPlc, {Interface, Root, 111}, {InterfaceComments, Comments, 122}}, {}));
    add(object(Comments, 122, {commentSegment({{"51", "Go!\nsecond line"}, {"52", ""}}), name("")}, {}, {}));
    add(object(Root, 100, {name(""), segment({fb(blobPlain(global))}), Bytes()}, {},
               {{UsedParts, 0, 0}, {UsedParts, Part, 101}, {UsedParts, Root, 102}}));
    add(object(Part, 101, {name(""), segment({fb(blobPaged(cfg, 64, false))})}, {}, {}));
    add(object(Root, 102, {name(""), segment({fb(blobPlain(udt))}), keyedBlobSegment(7, blobPlain(udtDefaults))}, {}, {}));

    // instance block DB8, optimized: its own interface object only has the
    // values, the members come from the function block's interface
    const std::string values = "<Values><V p=\"51\" v=\"true\"/></Values>";
    const std::string fb1 = "<Root><Member ID=\"2\" Name=\"Input\"><Member ID=\"51\" Name=\"Go\" RID=\"0x02000001\"/></Member>"
                            "<Member ID=\"3\" Name=\"Output\" RID=\"0xFFFF\" SubPartIndex=\"0\"/>"
                            "<Member ID=\"5\" Name=\"Static\" RID=\"0xFFFF\" SubPartIndex=\"1\"/>"
                            "<Member ID=\"6\" Name=\"Temp\"><Member ID=\"60\" Name=\"t\" RID=\"0x02000005\"/></Member></Root>";
    const std::string outputs = "<Section><Member ID=\"52\" Name=\"Done\" RID=\"0x02000001\"/></Section>";
    add(object(Db, 31, {name("Inst"), segment({fi(8), fo(true)}), segment({fs("IDBofFB"), fs("FB1")})},
               {inProject, onPlc, {Interface, Root, 110}}, {}));
    add(object(Root, 110, {name(""), segment({fb(blobPaged(values, 16, true))}), Bytes()}, {}, {{UsedParts, Root, 111}}));
    add(object(Root, 111, {name(""), segment({fb(blobPlain(fb1))}), Bytes()}, {}, {{UsedParts, Part, 112}, {UsedParts, 0, 0}}));
    add(object(Part, 112, {name(""), segment({fb(blobPlain(outputs))})}, {}, {}));

    // Constants. A station with a CPU and an interface below it; two hardware
    // identifiers, a process image partition, two user constants and a copy
    // that belongs to a library object.
    {
        enum : uint32_t { Constant = 0x100e, Device = 0x100f, Item = 0x1010 };
        enum : uint32_t { ItemParent = 0x2107, ConstantItem = 0x2109 };
        // sets in slot order: ICoreAttributes, IDefaultStrategyData, IStructureItem, IStructureRoot
        auto constant = [&](uint64_t id, const std::string& n, const std::string& type, const std::string& value,
                            bool system, const std::vector<Rel>& rels, const Bytes& core = Bytes()) {
            add(object(Constant, id,
                       {core.empty() ? name(n) : core, segment({fs(value)}), segment({fs(type)}), segment({fo(system)})},
                       rels, {}));
        };
        add(object(Device, 200, {name("Station_1")}, {inProject}, {}));
        add(object(Item, 201, {name("PLC_1")}, {inProject, {ItemParent, Device, 200}}, {}));
        add(object(Item, 202, {name("PROFINET interface_1")}, {inProject, {ItemParent, Item, 201}}, {}));
        const Rel inTable{TagTable, Table, 3};
        constant(210, "Local~PROFINET_interface_1", "Hw_Interface", "64", true,
                 {inProject, onPlc, inTable, {ConstantItem, Item, 202}});
        constant(211, "Local", "Hw_SubModule", "9", true, {inProject, onPlc, inTable, {ConstantItem, Item, 201}});
        constant(212, "PIP 1", "Pip", "1", true, {inProject, onPlc, inTable});
        constant(213, "LIMIT", "Int", "100", false, {inProject, onPlc, inTable}, commented("LIMIT", {{0x0409, "upper limit"}}));
        constant(214, "GREETING", "String", "'Hi'", false, {inProject, onPlc, inTable});
        constant(215, "Copy", "Int", "1", false, {{Environment, Plc, 2}, onPlc, inTable});
    }

    // a block whose interface object is missing
    add(object(Db, 32, {name("Bare"), segment({fi(9), fo(true)}), segment({fs("SharedDB"), fs("")})},
               {inProject, onPlc, {Interface, Root, 999}}, {}));
    return f;
}

void testProgram() {
    Bytes file = programProject();
    tia::Project p(tia::Container::parse(file, {}));
    tia::ProgramData d = tia::buildProgramData(p);

    CHECK(d.tags.size() == 1 && d.stats.tagsOutsideProject == 1);
    if (d.tags.size() == 1) {
        const tia::Tag& t = d.tags[0];
        CHECK(t.name == "Start" && t.address == "%I0.0" && t.dataType == "Bool");
        CHECK(t.plc == "PLC_1" && t.table == "Default tag table");
        CHECK(t.comment == "Start button");  // the entry without a language wins
    }
    {
        tia::Object o;
        const tia::Block* b = p.live(0x1005, 20);
        CHECK(b && p.decode(*b, o));
        const tia::Value* c = o.attr("ICoreAttributes", "Comment");
        CHECK(c && c->type == tia::Value::Type::Text && c->texts.size() == 3);
        if (c && c->texts.size() == 3) CHECK(c->texts[1].first == 0x0407 && c->texts[1].second == "Starttaste");
        // an object without a comment
        CHECK(p.live(0x1002, 2) && p.decode(*p.live(0x1002, 2), o) && o.attr("ICoreAttributes", "Comment") &&
              o.attr("ICoreAttributes", "Comment")->isNull());
        // names of member comments come from the segment itself
        CHECK(p.live(0x100c, 120) && p.decode(*p.live(0x100c, 120), o) && o.problems.empty());
        CHECK(o.expando.size() == 3 && o.expando[1].first == "53:51" && o.expando[1].second.s == "enable");
    }

    CHECK(d.blocks.size() == 3);
    if (d.blocks.size() != 3) return;
    const tia::DataBlock& g = d.blocks[0];
    CHECK(g.name == "Data" && g.hasNumber && g.number == 7 && g.kind == "global" && g.plc == "PLC_1");
    CHECK(g.hasAccess && !g.symbolicAccessOnly);
    CHECK(g.title == "nur deutsch" && g.comment.empty());  // only one language: that one
    CHECK(g.members.size() == 7 && g.memberCount == 11);
    if (g.members.size() == 7) {
        const auto& m = g.members;
        CHECK(m[0].name == "Run" && m[0].dataType == "Bool" && tia::formatOffset(m[0]) == "0.0" && !m[0].hasStartValue);
        CHECK(m[1].dataType == "Int" && tia::formatOffset(m[1]) == "2" && m[1].startValue == "1500");
        CHECK(m[0].comment.empty() && m[1].comment == "Speed in rpm");
        // anonymous structure: members from its own part, values by ID path
        CHECK(m[2].dataType == "Struct" && tia::formatOffset(m[2]) == "4" && m[2].members.size() == 2);
        if (m[2].members.size() == 2) {
            CHECK(m[2].members[0].name == "On" && tia::formatOffset(m[2].members[0]) == "4.0");
            CHECK(m[2].members[0].hasStartValue && m[2].members[0].startValue == "true");
            CHECK(m[2].members[0].comment == "enable" && m[2].members[1].comment.empty());
            CHECK(tia::formatOffset(m[2].members[1]) == "6" && !m[2].members[1].hasStartValue);
        }
        // named type: its own defaults, unless the block overrides them
        CHECK(m[3].dataType == "\"MotorUDT\"" && m[3].members.size() == 2);
        if (m[3].members.size() == 2) {
            CHECK(m[3].members[0].startValue == "3" && tia::formatOffset(m[3].members[0]) == "8");
            CHECK(m[3].members[1].startValue == "9" && tia::formatOffset(m[3].members[1]) == "10");
            // comments of a named type's members belong to the type
            CHECK(m[3].members[0].comment == "ident" && m[3].members[1].comment.empty());
        }
        CHECK(m[4].dataType == "Array[0..3] of Byte" && tia::formatOffset(m[4]) == "12");
        CHECK(m[5].unresolved && m[5].members.empty());
        CHECK(m[6].dataType == "type#2457");  // a number the project does not list
    }
    CHECK(g.notes.size() == 1);  // says that one nested type could not be followed

    const tia::DataBlock& i = d.blocks[1];
    CHECK(i.name == "Inst" && i.kind == "instance" && i.instanceOf == "FB1" && i.symbolicAccessOnly);
    CHECK(i.members.size() == 2 && i.notes.empty());
    if (i.members.size() == 2) {
        CHECK(i.members[0].name == "Go" && i.members[0].section == "Input" && i.members[0].startValue == "true");
        CHECK(i.members[0].comment == "Go!\nsecond line" && i.members[1].comment.empty());
        CHECK(i.members[1].name == "Done" && i.members[1].section == "Output" && !i.members[1].hasOffset);
    }

    const tia::DataBlock& bare = d.blocks[2];
    CHECK(bare.name == "Bare" && bare.members.empty() && !bare.notes.empty());
    CHECK(d.stats.blocksWithoutInterface == 1 && d.warnings.size() == 1);

    // Constants: hardware identifiers first, by number, with the item and the
    // station they stand for; then user constants in the order entered; the
    // rest after that.
    CHECK(d.constants.size() == 5 && d.stats.constantsOutsideProject == 1);
    if (d.constants.size() == 5) {
        const auto& c = d.constants;
        CHECK(c[0].name == "Local" && c[0].kind == "hardware" && c[0].value == "9" && c[0].system);
        CHECK(c[0].standsFor == "PLC_1" && c[0].standsForDevice == "Station_1" && c[0].plc == "PLC_1");
        CHECK(c[1].name == "Local~PROFINET_interface_1" && c[1].dataType == "Hw_Interface" && c[1].value == "64");
        CHECK(c[1].standsFor == "PROFINET interface_1" && c[1].standsForDevice == "Station_1");
        CHECK(c[2].name == "LIMIT" && c[2].kind == "user" && !c[2].system && c[2].dataType == "Int");
        CHECK(c[2].value == "100" && c[2].comment == "upper limit" && c[2].table == "Default tag table");
        CHECK(c[2].standsFor.empty() && c[3].name == "GREETING" && c[3].value == "'Hi'" && c[3].comment.empty());
        CHECK(c[4].name == "PIP 1" && c[4].kind == "pip" && c[4].standsFor.empty());
    }

    // The list of blocks: the three data blocks and the two objects with an
    // interface of their own, data blocks after code blocks, by number.
    CHECK(d.blockList.size() == 5 && d.stats.listedBlocksOutsideProject == 0);
    if (d.blockList.size() == 5) {
        const auto& l = d.blockList;
        CHECK(l[0].name == "Data" && l[0].hasNumber && l[0].number == 7 && l[0].kind == "global");
        CHECK(l[0].title == "nur deutsch" && l[0].plc == "PLC_1" && l[0].hasAccess && !l[0].symbolicAccessOnly);
        CHECK(l[1].name == "Inst" && l[1].number == 8 && l[1].kind == "instance" && l[1].instanceOf == "FB1");
        CHECK(l[2].name == "Bare" && l[2].number == 9);
        CHECK(!l[3].hasNumber && !l[4].hasNumber && l[3].kind.empty());
        // nothing is stored about protection, folders or downloads here
        CHECK(l[0].protection.empty() && l[0].folder.empty() && l[0].downloaded.empty() && l[0].downloads.empty());
        CHECK(!l[0].hasLoadMemory && !l[0].hasNetworks && !l[0].writeProtectedInDevice.stored);
    }
    // The download history of a block as stored in a public V19 project;
    // its first entry is the block's DownloadTime there.
    {
        const auto h = tia::parseDownloadHistory("FB3-638682897237619877;FB3-638682472174389229;FB?-0");
        CHECK(h.size() == 2);
        if (h.size() == 2) CHECK(h[0] == "2024-11-27T07:35:23.761Z" && h[1] == "2024-11-26T19:46:57.438Z");
        CHECK(tia::parseDownloadHistory("DB?-0").empty() && tia::parseDownloadHistory("").empty());
        // anything that is not a number after the last dash is passed over
        CHECK(tia::parseDownloadHistory(";;-;FB1-;FB1-12x;FB1-99999999999999999999999;x").empty());
        CHECK(tia::parseDownloadHistory("FB1-9999999999999999999;FB1-4000000000000000000").empty());  // not times
        std::string many;
        for (int i = 0; i < 200; ++i) many += "FB1-638682897237619877;";
        CHECK(tia::parseDownloadHistory(many).size() == 64);
    }
    CHECK(tia::languageName("LAD_CLASSIC") == "LAD" && tia::languageName("FBD_CLASSIC") == "FBD");
    CHECK(tia::languageName("SCL") == "SCL" && tia::languageName("LAD_IEC") == "LAD_IEC" && tia::languageName("").empty());

    // corrupted copies must not crash the member walk
    size_t survived = 0;
    for (size_t at = 98; at < file.size(); at += 5) {
        Bytes m = file;
        m[at] = static_cast<uint8_t>(m[at] ^ (0x01u << (at % 8)));
        try {
            tia::Project q(tia::Container::parse(m, {}));
            tia::buildProgramData(q);
        } catch (const tia::ParseError&) {
        }
        ++survived;
    }
    CHECK(survived > 800);
}

// The state of a project after an earlier save is still in the file: each
// save ends with a system object of type 0x7000C.
void testSaves() {
    Bytes f(98, 0);
    f[0] = 0x40;
    f[4] = 1;
    auto hh = tia::sha256(f.data(), 65);
    std::memcpy(f.data() + 65, hh.data(), 32);
    f[97] = 0xff;
    auto add = [&](const Bytes& blk) {
        size_t from = f.size();
        append(f, blk);
        appendHash(f, from);
    };
    auto saved = [&](uint64_t n) { add(block(0x7000c, n, 0, 0, systemBody(Bytes(8, 0)))); };
    add(block(0x70000, 1, 0, 0, systemBody(deflated(kProgramMeta))));
    auto plc = [](const std::string& n) { return object(0x1002, 2, {segment({fs(n), fnone()})}, {}, {}); };
    add(plc("first"));
    saved(1);
    add(plc("second"));
    add(object(0x1002, 3, {segment({fs("other"), fnone()})}, {}, {}));
    saved(2);
    add(plc("third"));
    add(block(0x1002, 3, 4, 0, Bytes{0xff}));  // deleted in the third save
    saved(3);

    auto nameAt = [&](size_t save) {
        tia::ContainerOptions opt;
        opt.throughSave = save;
        tia::Project p(tia::Container::parse(f, opt));
        CHECK(p.container().saveCount() == 3);
        tia::Object o;
        const tia::Block* b = p.live(0x1002, 2);
        return b && p.decode(*b, o) ? o.attrString("ICoreAttributes", "Name") : std::string("-");
    };
    CHECK(nameAt(1) == "first");
    CHECK(nameAt(2) == "second");
    CHECK(nameAt(3) == "third" && nameAt(0) == "third" && nameAt(99) == "third");
    tia::ContainerOptions two;
    two.throughSave = 2;
    tia::Project p2(tia::Container::parse(f, two));
    CHECK(p2.live(0x1002, 3) != nullptr);  // still there after the second save
    tia::Project p3(tia::Container::parse(f, {}));
    CHECK(p3.live(0x1002, 3) == nullptr);
}

// ---- structures and arrays of structures ------------------------------------

const char kStructMeta[] =
    "<MetaInfo><Package name=\"P\" id=\"0x1\"><Namespace name=\"M\">"
    "<Structure name=\"Event\" id=\"0x4001\"><Element name=\"Version\" type=\"xs:string\"/>"
    "<Element name=\"Date\" type=\"xs:dateTime\"/><Element name=\"Id\" type=\"xs:string\"/>"
    "<Element name=\"Count\" type=\"xs:int\"/><Element name=\"Flag\" type=\"xs:boolean\"/></Structure>"
    "<Array name=\"Events\" id=\"0x4002\" type=\"Event\"/>"
    "<AttributeSet name=\"IInfo\" id=\"0x3001\" persistent=\"true\">"
    "<Attribute name=\"History\" id=\"0\" type=\"Events\"/><Attribute name=\"One\" id=\"1\" type=\"Event\"/>"
    "<Attribute name=\"After\" id=\"2\" type=\"xs:int\"/></AttributeSet>"
    "<ObjectType name=\"Info\" id=\"0x1001\"><Implements ref=\"IInfo\"/></ObjectType>"
    "<AttributeSet name=\"IVault\" id=\"0x3002\" persistent=\"true\">"
    "<Attribute name=\"Settings\" id=\"0\" type=\"Events\"/><Attribute name=\"OldPasswords\" id=\"1\" type=\"Events\"/>"
    "</AttributeSet>"
    "<ObjectType name=\"Vault\" id=\"0x1002\"><Implements ref=\"IVault\"/></ObjectType>"
    "</Namespace></Package></MetaInfo>";

// A stored structure "Event": its size, one field per element, the strings.
Bytes eventRecord(const char* version, uint64_t ticks, const char* id, uint32_t count, bool flagOn) {
    const size_t fixed = 4 + 4 + 8 + 4 + 4 + 1;
    Bytes head, var;
    auto text = [&](const char* t) {
        if (!t) {
            put32(head, 0);  // not there
            return;
        }
        put32(head, static_cast<uint32_t>(fixed + var.size()));
        const std::string v(t);
        append(var, varint(v.size() + 1));
        var.insert(var.end(), v.begin(), v.end());
    };
    text(version);
    put64(head, ticks);
    text(id);
    put32(head, count);
    head.push_back(flagOn ? 1 : 0);
    Bytes r;
    put32(r, static_cast<uint32_t>(fixed + var.size()));
    append(r, head);
    append(r, var);
    return r;
}

Bytes eventList(const std::vector<Bytes>& records) {
    Bytes l;
    put32(l, 0);
    put32(l, static_cast<uint32_t>(records.size()));
    size_t at = 8 + 4 * records.size();
    for (const auto& r : records) {
        put32(l, static_cast<uint32_t>(at));
        at += r.size();
    }
    for (const auto& r : records) append(l, r);
    set32(l, 0, static_cast<uint32_t>(l.size()));
    return l;
}

void testStructures() {
    const uint64_t t1 = 630822816000000000ULL | (1ULL << 62);
    const Bytes list = eventList({eventRecord("V1", t1, "Created", 3, true), eventRecord(nullptr, t1, "", 0, false)});
    const Bytes one = eventRecord("V2", t1, nullptr, 0xfffffffeu, false);
    // segment: length, History -> list, One -> record, After
    auto build = [&](const Bytes& l, const Bytes& r) {
        Bytes seg;
        put32(seg, static_cast<uint32_t>(16 + l.size() + r.size()));
        put32(seg, 16);
        put32(seg, static_cast<uint32_t>(16 + l.size()));
        put32(seg, 7);
        append(seg, l);
        append(seg, r);
        Bytes f(98, 0);
        f[0] = 0x40;
        f[4] = 1;
        f[97] = 0xff;
        for (const Bytes& blk : {block(0x70000, 1, 0, 0, systemBody(deflated(kStructMeta))), object(0x1001, 5, {seg}, {}, {})}) {
            size_t from = f.size();
            append(f, blk);
            appendHash(f, from);
        }
        return f;
    };
    auto decode = [&](const Bytes& file, tia::Object& o) {
        tia::Project p(tia::Container::parse(file, {}));
        const tia::Block* b = p.live(0x1001, 5);
        return b && p.decode(*b, o);
    };
    using T = tia::Value::Type;
    {
        tia::Object o;
        CHECK(decode(build(list, one), o));
        const tia::Value* h = o.attr("IInfo", "History");
        CHECK(h && h->type == T::List && h->elements.size() == 2);
        if (h && h->type == T::List && h->elements.size() == 2) {
            const tia::Value& a = h->elements[0];
            CHECK(a.type == T::Record && a.names.size() == 5);
            CHECK(a.field("Version") && a.field("Version")->s == "V1");
            CHECK(a.field("Date") && a.field("Date")->s == "2000-01-01T00:00:00Z");
            CHECK(a.field("Id") && a.field("Id")->s == "Created");
            CHECK(a.field("Count") && a.field("Count")->i == 3);
            CHECK(a.field("Flag") && a.field("Flag")->b);
            CHECK(a.field("Nothing") == nullptr);
            const tia::Value& b = h->elements[1];
            CHECK(b.field("Version") && b.field("Version")->isNull());       // not there
            CHECK(b.field("Id") && b.field("Id")->type == T::String && b.field("Id")->s.empty());  // there, empty
        }
        const tia::Value* r = o.attr("IInfo", "One");
        CHECK(r && r->type == T::Record);
        if (r && r->type == T::Record) {
            CHECK(r->field("Version")->s == "V2" && r->field("Id")->isNull() && r->field("Count")->i == -2);
        }
        const tia::Value* after = o.attr("IInfo", "After");
        CHECK(after && after->i == 7);
    }
    // What does not follow the rule exactly stays an unread structure; the
    // rest of the object is read as before.
    auto opaque = [&](Bytes l, Bytes r, bool listBad) {
        tia::Object o;
        CHECK(decode(build(l, r), o));
        const tia::Value* h = o.attr("IInfo", "History");
        const tia::Value* one1 = o.attr("IInfo", "One");
        CHECK(h && one1);
        if (!h || !one1) return;
        CHECK((h->type == T::Opaque) == listBad);
        CHECK((one1->type == T::Opaque) == !listBad);
        const tia::Value* after = o.attr("IInfo", "After");
        CHECK(after && after->i == 7);
    };
    {
        Bytes r = one;  // a byte that belongs to nothing
        r.push_back(0);
        set32(r, 0, static_cast<uint32_t>(r.size()));
        opaque(list, r, false);
    }
    {
        Bytes r = one;  // a string that points outside the structure
        set32(r, 4, 0x1000);
        opaque(list, r, false);
    }
    {
        Bytes r = one;  // size beyond the segment
        set32(r, 0, 0x7fffffff);
        opaque(list, r, false);
    }
    {
        Bytes l = list;  // more elements than there is room for
        set32(l, 4, 0x00ffffff);
        opaque(l, one, true);
    }
    {
        Bytes l = list;  // an element that starts inside the offsets
        set32(l, 8, 4);
        opaque(l, one, true);
    }
    {
        Bytes l = list;  // both offsets name the same structure
        set32(l, 12, l[8]);
        opaque(l, one, true);
    }

    // The object dump shows what is read, and no password in it: neither a
    // value that a structure itself calls a password (the settings of an HMI
    // connection are name and value pairs), nor anything below an attribute
    // that is named like one.
    {
        const Bytes settings = eventList({eventRecord("NTP", t1, "TimeSyncMode", 1, true),
                                          eventRecord("hunter2", t1, "Password", 2, true),
                                          eventRecord("", t1, "Password", 3, true)});
        const Bytes old = eventList({eventRecord("s3cret", t1, "first", 4, false)});
        Bytes seg;
        put32(seg, static_cast<uint32_t>(12 + settings.size() + old.size()));
        put32(seg, 12);
        put32(seg, static_cast<uint32_t>(12 + settings.size()));
        append(seg, settings);
        append(seg, old);
        Bytes f(98, 0);
        f[0] = 0x40;
        f[4] = 1;
        f[97] = 0xff;
        for (const Bytes& blk : {block(0x70000, 1, 0, 0, systemBody(deflated(kStructMeta))), object(0x1002, 6, {seg}, {}, {})}) {
            size_t from = f.size();
            append(f, blk);
            appendHash(f, from);
        }
        tia::Project p(tia::Container::parse(f, {}));
        tia::Object o;
        const tia::Block* b = p.live(0x1002, 6);
        CHECK(b && p.decode(*b, o));
        const tia::Value* v = o.attr("IVault", "Settings");
        CHECK(v && v->type == T::List && v->elements.size() == 3 && v->elements[1].field("Version")->s == "hunter2");
        std::ostringstream dump;
        tia::writeObjects(dump, p);
        const std::string d = dump.str();
        CHECK(d.find("\"Settings\": [{\"Version\": \"NTP\", \"Date\": \"2000-01-01T00:00:00Z\", \"Id\": \"TimeSyncMode\", "
                     "\"Count\": 1, \"Flag\": true}, {\"Version\": {\"redacted\": true}, \"Date\": \"2000-01-01T00:00:00Z\", "
                     "\"Id\": \"Password\", \"Count\": 2, \"Flag\": true}, {\"Version\": \"\",") != std::string::npos);
        CHECK(d.find("\"OldPasswords\": [{\"Version\": {\"redacted\": true}, \"Date\": \"2000-01-01T00:00:00Z\", "
                     "\"Id\": {\"redacted\": true}, \"Count\": 4, \"Flag\": false}]") != std::string::npos);
        CHECK(d.find("hunter2") == std::string::npos && d.find("s3cret") == std::string::npos);
    }
}

// ---- HMI tags ---------------------------------------------------------------

// A segment of run-time named attributes holding one value at the start of
// its variable data.
Bytes keyedSegment(uint32_t key, const Bytes& value) {
    Bytes b;
    put32(b, 0);  // used, patched below
    put32(b, 0);  // capacity, patched below
    put32(b, 1);
    b.push_back(2);  // names in the key table
    b.push_back(0);
    put32(b, 0);
    put32(b, key);
    put32(b, 0);
    append(b, value);
    set32(b, 0, static_cast<uint32_t>(b.size()));
    set32(b, 4, static_cast<uint32_t>(b.size()));
    return b;
}

// One entry of a tag table's link list, laid out as TIA Portal V19 writes it:
// a blob without a length in front, a structure inside the structure, an
// array of strings, a string.
Bytes linkRecord(const Bytes& coid, uint32_t consumer, uint32_t handle, bool connected,
                 const std::vector<std::string>& names, uint32_t provider, const std::string& quoted) {
    Bytes inner;
    put32(inner, 8);
    put32(inner, handle);
    Bytes path;
    put32(path, 0);
    put32(path, static_cast<uint32_t>(names.size()));
    size_t at = 8 + 4 * names.size();
    for (const auto& n : names) {
        put32(path, static_cast<uint32_t>(at));
        at += 1 + n.size();
    }
    for (const auto& n : names) {
        path.push_back(static_cast<uint8_t>(1 + n.size()));
        path.insert(path.end(), n.begin(), n.end());
    }
    set32(path, 0, static_cast<uint32_t>(path.size()));
    const size_t fixed = 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4;
    const size_t coidAt = fixed, innerAt = coidAt + coid.size(), pathAt = innerAt + inner.size(),
                 quotedAt = pathAt + path.size();
    Bytes r;
    put32(r, static_cast<uint32_t>(quotedAt + 1 + quoted.size()));
    put32(r, static_cast<uint32_t>(coidAt));
    put32(r, consumer);
    put32(r, static_cast<uint32_t>(innerAt));
    r.push_back(connected ? 1 : 0);
    put32(r, static_cast<uint32_t>(pathAt));
    put32(r, provider);
    put32(r, static_cast<uint32_t>(quotedAt));
    append(r, coid);
    append(r, inner);
    append(r, path);
    r.push_back(static_cast<uint8_t>(1 + quoted.size()));
    r.insert(r.end(), quoted.begin(), quoted.end());
    return r;
}

void testHmiTags() {
    Bytes f = programProject();
    auto add = [&](const Bytes& blk) {
        size_t from = f.size();
        append(f, blk);
        appendHash(f, from);
    };
    enum : uint32_t { Project = 0x1001, Plc = 0x1002, Table = 0x1003, Device = 0x100f, Item = 0x1010 };
    enum : uint32_t { Maintainer = 0x1020, Connection, Cycle, HmiTag, MemberTag };
    enum : uint32_t { Target = 0x2101, Environment, TagTable, ItemParent = 0x2107 };
    enum : uint32_t { InverseConsumer = 0x2202, ConnectionPoint, TagConnection, TagCycle, Members, MemberMembers };
    auto name = [](const std::string& n) { return segment({fs(n), fnone()}); };
    const Rel inProject{Environment, Project, 1}, onHmi{Target, Item, 301}, inTable{TagTable, Table, 313};
    const Rel viaConnection{TagConnection, Connection, 310}, cycle{TagCycle, Cycle, 312}, linked{InverseConsumer, Maintainer, 320};

    {
        // key table of HmiTagData: 1 -> "LinkHandle", a structure
        Bytes t;
        put32(t, 0);
        put32(t, HmiTag);
        put32(t, 1);
        put32(t, 1);
        put32(t, 1);
        t.push_back(10);
        for (char c : std::string("LinkHandle")) t.push_back(static_cast<uint8_t>(c));
        put32(t, 0x4101);
        set32(t, 0, static_cast<uint32_t>(t.size()));
        add(block(0x70011, 1025, 0, 0, systemBody(t)));
    }
    // the panel and its runtime, a connection, a cycle, a tag table
    add(object(Device, 300, {name("HMI_1")}, {inProject}, {}));
    add(object(Item, 301, {name("HMI_RT_1")}, {inProject, {ItemParent, Device, 300}}, {}));
    add(object(Connection, 310, {name("HMI_Connection_1")}, {inProject, onHmi, {ConnectionPoint, Item, 311}}, {}));
    add(object(Cycle, 312, {name("1 s")}, {inProject, onHmi}, {}));
    add(object(Table, 313, {name("HMI tags")}, {inProject, onHmi}, {}));
    // the links of that table; the number 1007 is used twice
    add(object(Maintainer, 320,
               {name(""),
                segment({fr(eventList({
                             linkRecord(blobPaged("fifteen bytes..", 4096, false), 0, 1001, true, {"Speed"}, 0, "Data.Speed"),
                             linkRecord(blobPlain("x"), 1, 1002, true, {"Cfg", "On"}, 0, "Data.Cfg.On"),
                             linkRecord(blobPaged("compressed page", 64, true), 2, 1003, true, {"Go"}, 1, "Inst.Go"),
                             linkRecord(blobPlain(""), 3, 1004, true, {}, 2, "Start"),
                             linkRecord(blobPlain("x"), 4, 1005, false, {"x"}, 3, "\"Gone.DB\".x"),
                             linkRecord(blobPlain("x"), 5, 1006, true, {"Buf[2]"}, 0, "Data.Buf[2]"),
                             linkRecord(blobPlain("x"), 6, 1007, true, {"Run"}, 0, "Data.Run"),
                             linkRecord(blobPlain("x"), 7, 1007, true, {"Run"}, 0, "Data.Run"),
                         })),
                         fi(3)})},
               {inProject, onHmi}, {}));
    // sets in slot order: ICoreAttributes, IHmiTagAttributes, IHmiTagStructureAttributes,
    // ISoftlinkAttributes, IStructureItem, ITagAddress
    auto tag = [&](uint64_t id, const std::string& n, const std::string& type, const std::string& address, int handle,
                   std::vector<Rel> rels, const std::vector<Rel>& members = {}, const std::string& start = "") {
        Bytes soft;
        if (handle) {
            Bytes h;
            put32(h, 8);
            put32(h, static_cast<uint32_t>(handle));
            soft = keyedSegment(1, h);
        }
        rels.insert(rels.begin(), {onHmi, inTable, cycle});
        add(object(HmiTag, id, {name(n), segment({fi(4)}), segment({fs(start)}), soft, segment({fs(type)}), segment({fs(address)})},
                   rels, members));
    };
    tag(330, "Data_Speed", "Int", "%DB7.DBW2", 1001, {inProject, viaConnection, linked}, {}, "12");
    tag(331, "Data_Cfg_On", "Bool", "%DB7.DBX4.0", 1002, {inProject, viaConnection, linked});
    tag(332, "Inst_Go", "Bool", "%DB1.DBD0", 1003, {inProject, viaConnection, linked});  // a stale address, as seen
    tag(333, "Start", "Bool", "%I9.9", 1004, {inProject, viaConnection, linked});
    tag(334, "Lost", "Int", "%DB1.DBD0", 1005, {inProject, viaConnection, linked});
    tag(335, "Buf2", "Byte", "%DB7.DBB14", 1006, {inProject, viaConnection, linked});
    tag(336, "Internal", "UInt", "", 0, {inProject});
    tag(337, "Abs", "Word", "%MW10", 0, {inProject, viaConnection});
    tag(338, "Struct", "MotorUDT", "", 0, {inProject}, {{Members, MemberTag, 340}, {Members, MemberTag, 341}});
    add(object(MemberTag, 340, {name("A"), segment({fs("Bool")})}, {inProject}, {}));
    add(object(MemberTag, 341, {name("B"), segment({fs("Struct")})}, {inProject}, {{MemberMembers, MemberTag, 342}}));
    add(object(MemberTag, 342, {name("C"), segment({fs("Int")})}, {inProject}, {}));
    tag(339, "Copy", "Int", "", 0, {{Environment, Plc, 2}});              // belongs to a library object
    tag(343, "Unnamed", "Int", "%DB1.DBD0", 1999, {inProject, viaConnection, linked});  // a link that is not in the list
    tag(344, "Twice", "Bool", "%DB7.DBX0.0", 1007, {inProject, viaConnection, linked});

    tia::Project p(tia::Container::parse(f, {}));
    {
        // the link list reads as a list of structures with everything in it
        tia::Object o;
        const tia::Block* b = p.live(Maintainer, 320);
        CHECK(b && p.decode(*b, o));
        const tia::Value* links = o.attr("IScopedLinkMaintainerData", "Links");
        CHECK(links && links->type == tia::Value::Type::List && links->elements.size() == 8);
        if (links && links->type == tia::Value::Type::List && links->elements.size() == 8) {
            const tia::Value& l = links->elements[1];
            CHECK(l.field("Coid") && l.field("Coid")->type == tia::Value::Type::Bytes);
            std::string coid;
            CHECK(l.field("Coid") && tia::decodeBlob(l.field("Coid")->s, coid) && coid == "x");
            CHECK(tia::decodeBlob(links->elements[0].field("Coid")->s, coid) && coid == "fifteen bytes..");
            CHECK(tia::decodeBlob(links->elements[2].field("Coid")->s, coid) && coid == "compressed page");
            CHECK(l.field("ConsumerIndex")->i == 1 && l.field("ProviderIndex")->i == 0 && l.field("IsConnected")->b);
            const tia::Value* h = l.field("Handle");
            CHECK(h && h->type == tia::Value::Type::Record && h->field("Index") && h->field("Index")->i == 1002);
            const tia::Value* np = l.field("NamePath");
            CHECK(np && np->type == tia::Value::Type::List && np->elements.size() == 2 && np->elements[1].s == "On");
            CHECK(l.field("QuotedNamePath")->s == "Data.Cfg.On");
            CHECK(links->elements[3].field("NamePath")->elements.empty());
        }
        CHECK(o.attr("IScopedLinkMaintainerData", "Version") && o.attr("IScopedLinkMaintainerData", "Version")->u == 3);
    }

    tia::ProgramData d = tia::buildProgramData(p);
    CHECK(d.hmiTags.size() == 11 && d.stats.hmiTagsOutsideProject == 1);
    CHECK(d.tags.size() == 1);  // HMI tags are not PLC tags
    auto find = [&](const char* n) -> const tia::HmiTag* {
        for (const auto& t : d.hmiTags)
            if (t.name == n) return &t;
        return nullptr;
    };
    // before the hardware is known: what the HMI side alone says
    const tia::HmiTag* speed = find("Data_Speed");
    CHECK(speed != nullptr);
    if (speed) {
        CHECK(speed->hmi == "HMI_1" && speed->runtime == "HMI_RT_1" && speed->table == "HMI tags");
        CHECK(speed->dataType == "Int" && speed->startValue == "12");
        CHECK(speed->access == "symbolic" && speed->plcTag == "Data.Speed" && speed->plcTagLinked);
        CHECK(speed->connection == "HMI_Connection_1" && speed->connectionId == 311);
        CHECK(speed->acquisitionCycle == "1 s" && speed->acquisitionMode == "Visible");
        CHECK(speed->addressStored == "%DB7.DBW2" && speed->address.empty() && speed->plc.empty());
    }

    // the connection as the hardware inventory lists it, and one that is not the tag's
    tia::Inventory inv;
    tia::Connection other, mine;
    other.id = 999;
    other.partner.module = "PLC_9";
    mine.id = 311;
    mine.partner.module = "PLC_1";
    mine.partner.device = "Station_1";
    inv.connections = {other, mine};
    tia::linkHmiTags(inv, d);

    speed = find("Data_Speed");
    if (speed) {
        // a member of a block with standard access: it has an address
        CHECK(speed->plc == "PLC_1" && speed->plcDevice == "Station_1");
        CHECK(speed->plcTagFound && speed->plcDataType == "Int" && speed->address == "%DB7.DBW2");
    }
    const tia::HmiTag* on = find("Data_Cfg_On");
    CHECK(on && on->plcTag == "Data.Cfg.On" && on->plcTagFound && on->plcDataType == "Bool" && on->address == "%DB7.DBX4.0");
    // a member of an optimized block has no address; what the tag stores is left over
    const tia::HmiTag* go = find("Inst_Go");
    CHECK(go && go->plcTagFound && go->plcDataType == "Bool" && go->address.empty() && go->addressStored == "%DB1.DBD0");
    // a PLC tag: its own address counts
    const tia::HmiTag* start = find("Start");
    CHECK(start && start->access == "symbolic" && start->plcTagFound && start->plcDataType == "Bool" && start->address == "%I0.0");
    // a link TIA Portal marked as broken, to something that is not there
    const tia::HmiTag* lost = find("Lost");
    CHECK(lost && lost->access == "symbolic" && lost->plcTag == "\"Gone.DB\".x" && !lost->plcTagLinked && !lost->plcTagFound);
    CHECK(lost && lost->address.empty());
    // an element of an array is found by the array
    const tia::HmiTag* buf = find("Buf2");
    CHECK(buf && buf->plcTagFound && buf->plcDataType == "Array[0..3] of Byte" && buf->address.empty());
    const tia::HmiTag* internal = find("Internal");
    CHECK(internal && internal->access == "internal" && internal->connection.empty() && internal->plcTag.empty());
    CHECK(internal && internal->plc.empty() && internal->address.empty());
    const tia::HmiTag* abs = find("Abs");
    CHECK(abs && abs->access == "absolute" && abs->plc == "PLC_1" && abs->plcTag.empty() && abs->address == "%MW10");
    const tia::HmiTag* st = find("Struct");
    CHECK(st && st->memberCount == 3 && st->members.size() == 2);
    if (st && st->members.size() == 2) {
        CHECK(st->members[0].name == "A" && st->members[0].dataType == "Bool" && st->members[0].members.empty());
        CHECK(st->members[1].name == "B" && st->members[1].members.size() == 1 && st->members[1].members[0].name == "C");
    }
    // a tag with a link is never taken for one with an absolute address
    const tia::HmiTag* unnamed = find("Unnamed");
    CHECK(unnamed && unnamed->access == "symbolic" && unnamed->plcTag.empty() && unnamed->address.empty());
    const tia::HmiTag* twice = find("Twice");
    CHECK(twice && twice->access == "symbolic" && twice->plcTag.empty() && twice->address.empty());
    CHECK(find("Copy") == nullptr);

    CHECK(tia::acquisitionModeName("Visible") == "Cyclic in operation" && tia::acquisitionModeName("New") == "New");
    CHECK(tia::acquisitionModeName("Continuous") == "Cyclic continuous");
    CHECK(tia::acquisitionModeName("CyclicContinuous") == "CyclicContinuous");  // not what the choice of that name stores

    std::ostringstream csv;
    tia::writeHmiTagsCsv(csv, d);
    const std::string c = csv.str();
    CHECK(c.find("hmi,table,name,data_type,access,connection,plc,plc_tag,address,acquisition_cycle,acquisition_mode,"
                 "comment,start_value,members,plc_tag_found,plc_data_type,address_stored\r\n") == 0);
    CHECK(c.find("\r\nHMI_1,HMI tags,Data_Speed,Int,symbolic,HMI_Connection_1,PLC_1,Data.Speed,%DB7.DBW2,1 s,"
                 "Cyclic in operation,,12,,yes,Int,%DB7.DBW2\r\n") != std::string::npos);
    CHECK(c.find("\r\nHMI_1,HMI tags,Lost,Int,symbolic,HMI_Connection_1,PLC_1,\"\"\"Gone.DB\"\".x\",,1 s,Cyclic in operation,,,,"
                 "no,,%DB1.DBD0\r\n") != std::string::npos);
    CHECK(c.find("\r\nHMI_1,HMI tags,Internal,UInt,internal,,,,,1 s,Cyclic in operation,,,,,,\r\n") != std::string::npos);
    CHECK(c.find("\r\nHMI_1,HMI tags,Unnamed,Int,symbolic,HMI_Connection_1,PLC_1,,,1 s,Cyclic in operation,,,,,,%DB1.DBD0\r\n") !=
          std::string::npos);
    CHECK(c.find("\r\nHMI_1,HMI tags,Struct,MotorUDT,internal,,,,,1 s,Cyclic in operation,,,3,,,\r\n") != std::string::npos);

    tia::ReportContext ctx;
    std::ostringstream text, json;
    tia::writeText(text, inv, d, ctx);
    tia::writeJson(json, inv, d, ctx);
    const std::string t = text.str(), j = json.str();
    CHECK(t.find("\nHMI tags:\n") != std::string::npos);
    CHECK(t.find("  HMI_1  HMI tags  Inst_Go      Bool      HMI_Connection_1  PLC_1  Inst.Go      -            1 s\n") !=
          std::string::npos);
    CHECK(t.find("  HMI_1  HMI tags  Internal     UInt      (internal)        -      -            -            1 s\n") !=
          std::string::npos);
    CHECK(t.find("  HMI_1  HMI tags  Abs          Word      HMI_Connection_1  PLC_1  -            %MW10        1 s\n") !=
          std::string::npos);
    CHECK(t.find("(link to the PLC tag broken)") != std::string::npos);
    CHECK(t.find("(stands for a PLC tag whose name could not be read)") != std::string::npos);
    CHECK(j.find("{\"hmi\": \"HMI_1\", \"runtime\": \"HMI_RT_1\", \"table\": \"HMI tags\", \"name\": \"Inst_Go\", "
                 "\"data_type\": \"Bool\", \"access\": \"symbolic\", \"connection\": \"HMI_Connection_1\", \"plc\": \"PLC_1\", "
                 "\"plc_device\": \"Station_1\", \"plc_tag\": \"Inst.Go\", \"plc_tag_linked\": true, \"plc_tag_found\": true, "
                 "\"plc_data_type\": \"Bool\", \"address\": null, \"address_stored\": \"%DB1.DBD0\"") != std::string::npos);
    CHECK(j.find("\"members\": [{\"name\": \"A\", \"data_type\": \"Bool\", \"members\": []}, {\"name\": \"B\", "
                 "\"data_type\": \"Struct\", \"members\": [{\"name\": \"C\", \"data_type\": \"Int\", \"members\": []}]}]") !=
          std::string::npos);

    // the history counts them with the first state it finds
    tia::History h = tia::buildHistory(std::make_shared<const std::vector<uint8_t>>(f));
    CHECK(h.saves.size() == 1 && h.saves[0].afterLastSave && h.saves[0].firstState);
    size_t counted = 0;
    if (h.saves.size() == 1)
        for (const auto& kv : h.saves[0].contents)
            if (kv.first == "HMI tag") counted = kv.second;
    CHECK(counted == 11);
}

// ---- save history -----------------------------------------------------------

void testHistory() {
    Bytes f(98, 0);
    f[0] = 0x40;
    f[4] = 1;
    f[97] = 0xff;
    auto add = [&](const Bytes& blk) {
        size_t from = f.size();
        append(f, blk);
        appendHash(f, from);
    };
    auto saved = [&](uint64_t n) { add(block(0x7000c, n, 0, 0, systemBody(Bytes(8, 0)))); };
    enum : uint32_t { Project = 0x1001, Plc, Table, Content, TagT };
    enum : uint32_t { Target = 0x2101, Environment, TagTable };
    auto name = [](const std::string& n) { return segment({fs(n), fnone()}); };
    const Rel inProject{Environment, Project, 1}, onPlc{Target, Plc, 2};
    auto tag = [&](uint64_t id, const std::string& n, const std::string& address) {
        return object(TagT, id, {name(n), segment({fs("Bool")}), segment({fs(address)})},
                      {inProject, onPlc, {TagTable, Table, 3}}, {});
    };

    add(block(0x70000, 1, 0, 0, systemBody(deflated(kProgramMeta))));
    saved(1);  // a file starts with a save that holds the type model only
    add(object(Project, 1, {name("Demo")}, {}, {}));
    add(object(Plc, 2, {name("PLC_1")}, {inProject}, {}));
    add(object(Table, 3, {name("Default tag table")}, {inProject, onPlc}, {}));
    add(tag(20, "Start", "%I0.0"));
    saved(2);
    add(tag(20, "Start", "%I0.5"));
    add(tag(22, "Stop", "%I0.1"));
    saved(3);
    add(block(TagT, 20, 4, 0, Bytes{0xff}));  // deleted
    add(block(TagT, 22, 4, 0, Bytes{0xff}));  // deleted, and made again under the same name
    add(tag(23, "Stop", "%I0.7"));
    saved(4);
    saved(5);  // a save that wrote nothing
    const size_t closed = f.size();
    add(tag(23, "Halt", "%I0.7"));  // written after the last save marker

    auto data = std::make_shared<const std::vector<uint8_t>>(f);
    tia::History h = tia::buildHistory(data);
    CHECK(h.savesInFile == 5);
    CHECK(h.saves.size() == 6);
    if (h.saves.size() == 6) {
        const auto& s = h.saves;
        CHECK(s[0].number == 1 && !s[0].firstState && s[0].changes.empty() && s[0].objectsWritten == 0);
        CHECK(s[1].firstState && s[1].changes.empty() && s[1].objectsWritten == 4);
        CHECK(s[1].contents.size() == 1 && s[1].contents[0].first == "tag" && s[1].contents[0].second == 1);
        CHECK(s[2].objectsWritten == 2 && s[2].objectsDeleted == 0 && s[2].changes.size() == 2);
        if (s[2].changes.size() == 2) {
            const auto& changed = s[2].changes[0];
            CHECK(changed.change == "changed" && changed.kind == "tag" && changed.item == "PLC_1 / Default tag table / Start");
            CHECK(changed.attribute == "address" && changed.from == "%I0.0" && changed.to == "%I0.5");
            const auto& added = s[2].changes[1];
            CHECK(added.change == "added" && added.item == "PLC_1 / Default tag table / Stop");
            CHECK(added.description == "Bool, %I0.1" && added.partOf.empty());
        }
        CHECK(s[2].objectTypes.size() == 1 && s[2].objectTypes[0].type == "EAMTZTagData" && s[2].objectTypes[0].written == 2);
        // one tag removed; the other is the same tag again, not one removed and one added
        CHECK(s[3].objectsWritten == 3 && s[3].objectsDeleted == 2 && s[3].changes.size() == 3);
        if (s[3].changes.size() == 3) {
            const auto& anew = s[3].changes[0];
            CHECK(anew.change == "changed" && anew.attribute == "created anew" && anew.isTime);
            CHECK(anew.item == "PLC_1 / Default tag table / Stop" && anew.from == "object 22" && anew.to == "object 23");
            CHECK(s[3].changes[1].attribute == "address" && s[3].changes[1].from == "%I0.1" && s[3].changes[1].to == "%I0.7");
            CHECK(s[3].changes[2].change == "removed" && s[3].changes[2].item == "PLC_1 / Default tag table / Start");
        }
        CHECK(!s[4].firstState && s[4].changes.empty() && s[4].objectsWritten == 0 && !s[4].afterLastSave);
        CHECK(s[5].afterLastSave && s[5].number == 6 && s[5].changes.size() == 1);
        if (s[5].changes.size() == 1) {
            const auto& c = s[5].changes[0];
            CHECK(c.change == "changed" && c.attribute == "name" && c.from == "Stop" && c.to == "Halt");
            CHECK(c.item == "PLC_1 / Default tag table / Halt");
        }
    }
    // up to a save: nothing of what came later
    tia::History h3 = tia::buildHistory(data, 3);
    CHECK(h3.savesInFile == 5 && h3.saves.size() == 3 && h3.saves.back().changes.size() == 2);
    // a file that ends with its last save marker has no state after it
    auto closedData = std::make_shared<const std::vector<uint8_t>>(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(closed));
    tia::History hc = tia::buildHistory(closedData);
    CHECK(hc.saves.size() == 5 && !hc.saves.back().afterLastSave);

    // the three ways of writing it stay in step
    std::ostringstream csv;
    tia::writeHistoryCsv(csv, h);
    const std::string c = csv.str();
    CHECK(c.find("save,time,by,objects_written,objects_deleted,change,kind,item,part_of,attribute,from,to,"
                 "description\r\n") == 0);
    CHECK(c.find("\r\n1,,,0,0,no_project,") != std::string::npos);
    CHECK(c.find("\r\n2,,,4,0,first_state,,,,,,,1 tag\r\n") != std::string::npos);
    CHECK(c.find(",changed,tag,PLC_1 / Default tag table / Start,,address,%I0.0,%I0.5,\r\n") != std::string::npos);
    CHECK(c.find(",removed,tag,PLC_1 / Default tag table / Start,,,,,\"Bool, %I0.5\"\r\n") != std::string::npos);
    CHECK(c.find("\r\n5,,,0,0,none,") != std::string::npos);
    CHECK(c.find("\r\nafter 5,") != std::string::npos);
}

void testAccessLevels() {
    CHECK(tia::accessLevelName("S71500.CPU", "V1.8", 1) == "Full access (no protection)");
    CHECK(tia::accessLevelName("S71500.CPU", "V4.1", 4) == "No access (complete protection)");
    CHECK(tia::accessLevelName("S71200.CPU", "V2.2", 2) == "Write protection");
    CHECK(tia::accessLevelName("S71200.CPU", "V2.2", 3) == "Write/read protection");
    // the same number means something else there, and what has not been
    // checked gets no name at all
    CHECK(tia::accessLevelName("S71500.CPU", "V1.8", 3) == "HMI access");
    CHECK(tia::accessLevelName("S71200.CPU", "V4.7", 1) == "Full access (no protection)");
    CHECK(tia::accessLevelName("S71200.CPU", "V4.7", 2) == "Read access");
    CHECK(tia::accessLevelName("S71200.CPU", "V4.7", 3) == "HMI access");
    CHECK(tia::accessLevelName("S71200.CPU", "V4.7", 4) == "No access (complete protection)");
    CHECK(tia::accessLevelName("S71200.CPU", "V2.2", 4).empty());
    CHECK(tia::accessLevelName("S71200.CPU", "", 2).empty());
    CHECK(tia::accessLevelName("S7300.CPU", "V3.3", 2).empty());
    CHECK(tia::accessLevelName("S71500.CPU", "V1.8", 9).empty());
    tia::Security none;
    CHECK(none.empty());
    none.putGet.stored = true;
    CHECK(!none.empty());

    // what decides about access: the level on a CPU without user management,
    // users and roles on one that has it, nothing once access control is off
    tia::Security old;
    old.hasAccessLevel = true;
    CHECK(tia::accessProtection(old) == "access_levels");
    CHECK(tia::accessProtection(tia::Security()) == "access_levels");
    tia::Security current;
    current.userManagement = true;
    CHECK(!current.empty());
    CHECK(tia::accessProtection(current) == "users_and_roles");
    current.accessControl = {true, true};
    CHECK(tia::accessProtection(current) == "users_and_roles");
    current.accessControlViaAccessLevels = {true, true};
    CHECK(tia::accessProtection(current) == "users_and_roles_and_access_levels");
    current.accessControlViaAccessLevels = {true, false};
    CHECK(tia::accessProtection(current) == "users_and_roles");
    current.accessControl = {true, false};
    current.accessControlViaAccessLevels = {true, true};
    CHECK(tia::accessProtection(current) == "none");
}

}  // namespace

int main() {
    testBytes();
    testSha256();
    testXml();
    testTicks();
    testMeta();
    testProject();
    testRobustness();
    testStorageRule();
    testBlob();
    testProgram();
    testSaves();
    testAccessLevels();
    testStructures();
    testHmiTags();
    testHistory();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
