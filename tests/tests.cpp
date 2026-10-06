// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-contained tests. The container test builds a small synthetic project
// in memory, so no Siemens project file is needed (or redistributed).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "container.hpp"
#include "inventory.hpp"
#include "meta.hpp"
#include "miniz.h"
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
    "<Implements ref=\"IB\"><Attribute name=\"Name\"/></Implements><Implements ref=\"IA\"/></ObjectType>"
    "</Namespace></Package></MetaInfo>";

void testMeta() {
    tia::MetaModel m;
    m.load(kMeta);
    const tia::TypeDef* thing = m.findById(0x1001);
    CHECK(thing && thing->name == "T.Thing");
    CHECK(m.derivesFrom("T.Thing", "T.Base") && !m.derivesFrom("T.Base", "T.Thing"));
    CHECK(m.relationId("Base", "Parent") == 0x2001);
    CHECK(m.relation(0x2001) && m.relation(0x2001)->behaviour == "parent");
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
    "</Namespace></Package></MetaInfo>";

struct Field {
    char kind;  // s string, b blob, i 32-bit number, o boolean
    std::string text;
    uint32_t number;
};
Field fs(const std::string& v) { return {'s', v, 0}; }
Field fb(const Bytes& v) { return {'b', str(v), 0}; }
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
    CHECK(g.comment == "nur deutsch");  // only one language: that one
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
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
