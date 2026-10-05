// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "program.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

#include "xml.hpp"

namespace tia {
namespace {

using Key = std::pair<uint32_t, uint64_t>;

constexpr int kMaxDepth = 16;
constexpr size_t kMaxMembersPerBlock = 200000;

// One stored piece of a block interface: an XML document plus the ordered
// list of further pieces that its members refer to by SubPartIndex.
struct Part {
    Key key{0, 0};
    std::unique_ptr<XmlNode> root;  // null when there is no (readable) payload
    std::vector<Key> subparts;      // {0,0} for an empty position
    std::vector<Key> related;       // every relation target, for finding value parts
    std::unique_ptr<XmlNode> values;  // <Values> document kept on the object itself
    // V13 and older: the part listing the types used by the members; member
    // SubPartIndex values refer to its list, not to the root's own.
    std::shared_ptr<Part> externals;
    bool externalsLooked = false;
};

std::string attrStr(const Object& o, const char* set, const char* name) { return o.attrString(set, name); }

bool attrInt(const Object& o, const char* set, const char* name, int64_t& out) {
    const Value* v = o.attr(set, name);
    uint64_t u = 0;
    if (!v || !v->asUInt(u)) return false;
    out = static_cast<int64_t>(u);
    return true;
}

// The text of a comment attribute; empty when there is none.
std::string attrText(const Object& o, const char* set, const char* name) {
    const Value* v = o.attr(set, name);
    return v && v->type == Value::Type::Text ? v->s : std::string();
}

bool attrBool(const Object& o, const char* set, const char* name, bool& out) {
    const Value* v = o.attr(set, name);
    if (!v || v->type != Value::Type::Bool) return false;
    out = v->b;
    return true;
}

class Builder {
public:
    explicit Builder(const Project& p) : project_(p), meta_(p.meta()) {
        relTarget_ = meta_.relationId("CoreObject", "Target");
        relEnvironment_ = meta_.relationId("CoreObject", "Environment");
        relTagTable_ = meta_.relationId("TagTableContentData", "TagTable");
        relGeneratedFrom_ = meta_.relationId("CanBeGeneratedFromFunctionalObjectData", "IsGeneratedFrom");
        relInterface_ = meta_.relationId("BlockInterfaceBaseData", "CurrentInterface");
        if (!relInterface_) relInterface_ = meta_.relationId("BlockInterfaceBaseData", "Source");
        relComments_ = meta_.relationId("BlockInterfaceBaseData", "InterfaceComments");
        relSubparts_ = meta_.relationId("InterfaceVersionRootData", "UsedParts");
        if (!relSubparts_) {
            relSubparts_ = meta_.relationId("XmlPartData", "ReferencedXmlParts");
            legacyParts_ = relSubparts_ != 0;
        }
    }

    ProgramData run() {
        ProgramData out;
        const Container& c = project_.container();
        for (const auto& kv : c.latest()) {
            const Block& b = c.blocks()[kv.second];
            if (c.isSystem(b) || b.deleted()) continue;
            const TypeDef* t = meta_.findById(b.type);
            if (!t || t->kind != TypeKind::ObjectType) continue;
            int role = roleOf(*t);
            if (role == 0) continue;
            Object o;
            try {
                if (!project_.decode(b, o)) continue;
            } catch (const ParseError&) {
                continue;
            }
            if (role == 3 || role == 4) noteComments(o);
            if (role == 1) {
                addCatalogueEntry(o);
            } else if (role == 2) {
                if (!inProject(o)) {
                    ++out.stats.tagsOutsideProject;
                    continue;
                }
                out.tags.push_back(makeTag(o));
            } else if (role == 3) {
                pendingBlocks_.push_back({b.type, b.id});
            }
        }
        // Blocks are built after the whole type catalogue is known.
        for (const Key& k : pendingBlocks_) {
            const Block* b = project_.live(k.first, k.second);
            Object o;
            if (!b) continue;
            try {
                if (!project_.decode(*b, o)) continue;
            } catch (const ParseError&) {
                continue;
            }
            if (!inProject(o)) {
                ++out.stats.blocksOutsideProject;
                continue;
            }
            DataBlock db = makeBlock(o);
            if (db.members.empty() && !db.notes.empty()) ++out.stats.blocksWithoutInterface;
            out.blocks.push_back(std::move(db));
        }
        std::stable_sort(out.tags.begin(), out.tags.end(), [](const Tag& a, const Tag& b) {
            if (a.plc != b.plc) return a.plc < b.plc;
            if (a.table != b.table) return a.table < b.table;
            return a.id < b.id;
        });
        std::stable_sort(out.blocks.begin(), out.blocks.end(), [](const DataBlock& a, const DataBlock& b) {
            if (a.plc != b.plc) return a.plc < b.plc;
            if (a.hasNumber != b.hasNumber) return a.hasNumber;
            if (a.number != b.number) return a.number < b.number;
            return a.id < b.id;
        });
        if (out.stats.blocksWithoutInterface)
            out.warnings.push_back(std::to_string(out.stats.blocksWithoutInterface) +
                                   " data block(s) listed without members (interface not readable)");
        return out;
    }

private:
    const Project& project_;
    const MetaModel& meta_;
    uint32_t relTarget_ = 0, relEnvironment_ = 0, relTagTable_ = 0, relGeneratedFrom_ = 0, relInterface_ = 0,
             relSubparts_ = 0, relComments_ = 0;
    bool legacyParts_ = false;
    // interface root -> the object holding the comments of its members
    std::map<Key, Key> commentParts_;
    std::map<Key, std::map<std::string, std::string>> comments_;
    std::map<uint32_t, int> roles_;
    std::map<Key, std::string> names_;
    std::map<Key, std::shared_ptr<Part>> parts_;
    // data type names by family ("S71200") and number
    std::map<std::string, std::map<uint32_t, std::string>> catalogue_;
    std::vector<Key> pendingBlocks_;

    // 1 type catalogue entry, 2 tag, 3 data block, 4 anything else with an
    // interface (code blocks, PLC data types): only its comments are of use
    int roleOf(const TypeDef& t) {
        auto it = roles_.find(t.id);
        if (it != roles_.end()) return it->second;
        int r = 0;
        if (meta_.derivesFromShort(t.name, "DataTypeItemData")) r = 1;
        else if (meta_.derivesFromShort(t.name, "EAMTZTagData")) r = 2;
        else if (meta_.derivesFromShort(t.name, "DataBlockData")) r = 3;
        else if (relComments_ && meta_.derivesFromShort(t.name, "BlockInterfaceBaseData")) r = 4;
        roles_[t.id] = r;
        return r;
    }

    bool decodeKey(const Key& k, Object& o) const {
        const Block* b = project_.live(k.first, k.second);
        if (!b) return false;
        try {
            return project_.decode(*b, o);
        } catch (const ParseError&) {
            return false;
        }
    }

    // Member comments are kept per block or type, in an object of their own
    // that names each member by its ID path.
    void noteComments(const Object& o) {
        Key iface, part;
        if (relInterface_ && relComments_ && o.relationTarget(relInterface_, iface) &&
            o.relationTarget(relComments_, part))
            commentParts_[iface] = part;
    }

    const std::map<std::string, std::string>* commentsOf(const Key& root) {
        auto c = comments_.find(root);
        if (c != comments_.end()) return &c->second;
        auto& out = comments_[root];
        // Interfaces of library blocks carry the comments themselves; blocks
        // and types of the project keep them in a separate object.
        std::vector<Key> holders{root};
        auto part = commentParts_.find(root);
        if (part != commentParts_.end()) holders.push_back(part->second);
        for (const Key& k : holders) {
            Object o;
            if (!decodeKey(k, o)) continue;
            for (const auto& e : o.expando)
                if (e.second.type == Value::Type::Text && !e.second.s.empty()) out[e.first] = e.second.s;
        }
        return &out;
    }

    std::string nameOf(const Key& k) {
        auto it = names_.find(k);
        if (it != names_.end()) return it->second;
        Object o;
        std::string n = decodeKey(k, o) ? attrStr(o, "ICoreAttributes", "Name") : std::string();
        names_[k] = n;
        return n;
    }

    // Objects kept for library types and versions hang under something other
    // than the project.
    bool inProject(const Object& o) const {
        Key env;
        if (!o.relationTarget(relEnvironment_, env)) return true;
        const TypeDef* t = meta_.findById(env.first);
        return t && meta_.derivesFromShort(t->name, "ProjectData");
    }

    std::string plcOf(const Object& o) {
        Key k;
        return o.relationTarget(relTarget_, k) ? nameOf(k) : std::string();
    }

    void addCatalogueEntry(const Object& o) {
        int64_t id = 0;
        if (!attrInt(o, "IDataTypeContent", "DataTypeID", id) || id <= 0) return;
        std::string scope = attrStr(o, "IDataTypeContent", "Scope");
        std::string name = attrStr(o, "ICoreAttributes", "Name");
        if (scope.empty() || name.empty()) return;
        catalogue_[scope].emplace(static_cast<uint32_t>(id), name);
    }

    Tag makeTag(const Object& o) {
        Tag t;
        t.id = o.id;
        t.name = attrStr(o, "ICoreAttributes", "Name");
        t.address = attrStr(o, "ITagAddress", "LogicalAddress");
        t.comment = attrText(o, "ICoreAttributes", "Comment");
        t.dataType = attrStr(o, "IStructureItem", "DisplayTypeName");
        if (t.dataType.empty()) t.dataType = attrStr(o, "IStructureItem", "DataTypeRefName");
        t.plc = plcOf(o);
        Key table;
        if (o.relationTarget(relTagTable_, table)) t.table = nameOf(table);
        return t;
    }

    std::shared_ptr<Part> loadPart(const Key& k) {
        auto it = parts_.find(k);
        if (it != parts_.end()) return it->second;
        auto part = std::make_shared<Part>();
        part->key = k;
        parts_[k] = part;
        Object o;
        if (!decodeKey(k, o)) return part;
        const Value* payload = o.attr("IInterfacePartData", "Payload");
        if (!payload) payload = o.attr("IXmlPartData", "PayLoad");
        part->root = document(payload);
        // The root of a PLC data type keeps the defaults of its members in an
        // attribute of its own instead of a separate part.
        part->values = document(o.expandoValue("Values"));
        for (const auto& r : o.relations) {
            if (relSubparts_ && r.relation == relSubparts_) part->subparts.emplace_back(r.targetType, r.targetId);
            if (r.targetType || r.targetId) part->related.emplace_back(r.targetType, r.targetId);
        }
        return part;
    }

    static std::unique_ptr<XmlNode> document(const Value* blob) {
        if (!blob || blob->type != Value::Type::Bytes) return nullptr;
        std::string xml;
        if (!decodeBlob(blob->s, xml) || xml.empty()) return nullptr;
        if (xml.compare(0, 3, "\xef\xbb\xbf") == 0) xml.erase(0, 3);
        try {
            return parseXml(xml);
        } catch (const ParseError&) {
            return nullptr;
        }
    }

    // <V p="51:52" v="1"/> since V14, <Value Path="51:52" Value="1"/> before.
    // The path is the chain of member IDs.
    static void readValues(const XmlNode& values, std::map<std::string, std::string>& out) {
        for (const auto& v : values.children) {
            const std::string* p = nullptr;
            const std::string* val = nullptr;
            if (v->name == "V") {
                p = v->attr("p");
                val = v->attr("v");
            } else if (v->name == "Value") {
                p = v->attr("Path");
                val = v->attr("Value");
            }
            if (p && val) out[*p] = *val;
        }
    }

    // Start values that belong to one interface document: written inside it,
    // or kept in a separate <Values> document next to it.
    void valuesOf(const std::shared_ptr<Part>& part, std::map<std::string, std::string>& out) {
        if (!part) return;
        if (part->values && part->values->name == "Values") readValues(*part->values, out);
        if (part->root) {
            if (part->root->name == "Values") readValues(*part->root, out);
            for (const auto& c : part->root->children)
                if (c->name == "Values") readValues(*c, out);
        }
        for (const Key& k : part->related) {
            std::shared_ptr<Part> p = loadPart(k);
            if (p->root && p->root->name == "Values") readValues(*p->root, out);
        }
    }

    // The part whose list a member's SubPartIndex refers to.
    std::shared_ptr<Part> typeOwner(const std::shared_ptr<Part>& owner) {
        if (!legacyParts_ || !owner) return owner;
        if (!owner->externalsLooked) {
            owner->externalsLooked = true;
            for (const Key& k : owner->subparts) {
                if (!k.first && !k.second) continue;
                std::shared_ptr<Part> p = loadPart(k);
                if (p->root && p->root->name == "ExternalTypes") {
                    owner->externals = p;
                    break;
                }
            }
        }
        return owner->externals ? owner->externals : owner;
    }

    std::string typeName(const XmlNode& m, const std::string& family) const {
        if (const std::string* t = m.attr("Type")) return *t;
        const std::string* rid = m.attr("RID");
        if (!rid) return std::string();
        uint32_t id = static_cast<uint32_t>(std::strtoul(rid->c_str(), nullptr, 0)) & 0xffff;
        // An anonymous structure has no type number; its members follow inline
        // or in a part of their own.
        if (id == 0 && (hasMembers(m) || m.attr("SubPartIndex"))) return "Struct";
        for (const std::string& scope : {family, std::string("S71500"), std::string("S71200"), std::string("S7300400")}) {
            auto c = catalogue_.find(scope);
            if (c == catalogue_.end()) continue;
            auto n = c->second.find(id);
            if (n != c->second.end()) return n->second;
        }
        return "type#" + std::to_string(id);
    }

    struct Context {
        std::string family;
        bool offsets = false;
        const std::map<std::string, std::string>* overrides = nullptr;  // full paths from the block root
        size_t* budget = nullptr;
    };

    // Members of `container`. `owner` is the part whose subparts list the
    // SubPartIndex attributes refer to. `path` is the chain of member IDs from
    // the block root, `local` the chain inside the current type definition.
    // `rootLevel` is set for the top element of a block or type interface,
    // the only place where the sections of a function block appear.
    void members(const XmlNode& container, const std::shared_ptr<Part>& owner, const Context& ctx,
                 const std::string& path, const std::string& local,
                 const std::map<std::string, std::string>* defaults,
                 const std::map<std::string, std::string>* comments, uint64_t baseBits, bool offsets, int depth,
                 bool rootLevel, const std::string& section, std::vector<BlockMember>& out) {
        if (depth > kMaxDepth) return;
        for (const auto& child : container.children) {
            if (child->name != "Member") continue;
            if (*ctx.budget == 0) return;
            const XmlNode& x = *child;
            const std::string id = x.attrOr("ID", "");
            const std::string name = x.attrOr("Name", "");
            const std::string* sub = x.attr("SubPartIndex");
            const std::string* rid = x.attr("RID");
            const std::string* stdo = x.attr("StdO");

            // Sections of a function block interface: fixed low IDs, no type.
            const long idNum = std::strtol(id.c_str(), nullptr, 10);
            if (rootLevel && idNum > 0 && idNum < 16 && (!rid || *rid == "0xFFFF")) {
                if (name == "Temp" || name == "Constant" || name == "Return") continue;
                const XmlNode* inner = &x;
                std::shared_ptr<Part> innerOwner = owner;
                std::shared_ptr<Part> held;
                if (!hasMembers(x)) {
                    if (!sub) continue;
                    held = subpart(owner, *sub);
                    if (!held || !held->root) continue;  // an empty section
                    inner = held->root.get();
                    if (!held->subparts.empty()) innerOwner = held;
                }
                // Members of a section count from the start of the section.
                const uint64_t sectionBits = baseBits + (offsets && stdo ? std::strtoull(stdo->c_str(), nullptr, 10) : 0);
                members(*inner, innerOwner, ctx, path, local, defaults, comments, sectionBits, offsets && stdo,
                        depth + 1, false, name, out);
                continue;
            }

            --*ctx.budget;
            BlockMember m;
            m.name = name;
            m.section = section;
            m.dataType = typeName(x, ctx.family);
            const std::string fullPath = path.empty() ? id : path + ":" + id;
            const std::string localPath = local.empty() ? id : local + ":" + id;
            if (ctx.overrides) {
                auto v = ctx.overrides->find(fullPath);
                if (v != ctx.overrides->end()) {
                    m.startValue = v->second;
                    m.hasStartValue = true;
                }
            }
            if (!m.hasStartValue && defaults) {
                auto v = defaults->find(localPath);
                if (v != defaults->end()) {
                    m.startValue = v->second;
                    m.hasStartValue = true;
                }
            }
            if (comments) {
                auto c = comments->find(localPath);
                if (c != comments->end()) m.comment = c->second;
            }
            uint64_t bits = baseBits;
            if (offsets && stdo) {
                bits = baseBits + std::strtoull(stdo->c_str(), nullptr, 10);
                m.hasOffset = true;
                m.offsetBits = bits;
                m.isBit = m.dataType == "Bool";
            }
            // An array's children describe one element, so offsets stop there.
            const bool array = m.dataType.compare(0, 5, "Array") == 0;
            const bool childOffsets = m.hasOffset && !array;

            if (hasMembers(x)) {
                members(x, owner, ctx, fullPath, localPath, defaults, comments, bits, childOffsets, depth + 1, false,
                        std::string(), m.members);
            } else if (sub) {
                // Named types (RID 0x02......) are listed apart in V13 and
                // older; anonymous structures are always in the root's list.
                const bool named = rid && (std::strtoul(rid->c_str(), nullptr, 0) >> 24) == 0x02;
                std::shared_ptr<Part> p = subpart(named ? typeOwner(owner) : owner, *sub);
                if (!p || !p->root) {
                    m.unresolved = true;
                } else {
                    // A type definition brings its own default values and its own
                    // list of nested parts.
                    const bool typeRoot = p->root->name == "Root";
                    std::map<std::string, std::string> own;
                    const std::map<std::string, std::string>* d = defaults;
                    const std::map<std::string, std::string>* c = comments;
                    std::string nextLocal = localPath;
                    if (typeRoot) {
                        valuesOf(p, own);
                        d = &own;
                        c = commentsOf(p->key);
                        nextLocal.clear();
                    }
                    members(*p->root, (typeRoot || !p->subparts.empty()) ? p : owner, ctx, fullPath, nextLocal, d, c,
                            bits, childOffsets, depth + 1, typeRoot, std::string(), m.members);
                }
            }
            out.push_back(std::move(m));
        }
    }

    static bool hasMembers(const XmlNode& n) {
        for (const auto& c : n.children)
            if (c->name == "Member") return true;
        return false;
    }

    std::shared_ptr<Part> subpart(const std::shared_ptr<Part>& owner, const std::string& index) {
        char* end = nullptr;
        unsigned long i = std::strtoul(index.c_str(), &end, 10);
        if (!owner || end == index.c_str() || i >= owner->subparts.size()) return nullptr;
        const Key k = owner->subparts[i];
        if (!k.first && !k.second) return nullptr;
        return loadPart(k);
    }

    static size_t count(const std::vector<BlockMember>& v) {
        size_t n = v.size();
        for (const auto& m : v) n += count(m.members);
        return n;
    }

    static bool anyUnresolved(const std::vector<BlockMember>& v) {
        for (const auto& m : v)
            if (m.unresolved || anyUnresolved(m.members)) return true;
        return false;
    }

    DataBlock makeBlock(const Object& o) {
        DataBlock db;
        db.id = o.id;
        db.name = attrStr(o, "ICoreAttributes", "Name");
        db.address = attrStr(o, "ITagAddress", "LogicalAddress");
        db.comment = attrText(o, "ICoreAttributes", "Comment");
        db.plc = plcOf(o);
        db.hasNumber = attrInt(o, "IGeneralBlockSourceData", "Number", db.number) ||
                       attrInt(o, "IGeneralBlockData", "Number", db.number);
        db.hasAccess = attrBool(o, "IGeneralBlockSourceData", "OnlySymbolicAccess", db.symbolicAccessOnly) ||
                       attrBool(o, "IIecplObjectData", "OnlySymbolicAccess", db.symbolicAccessOnly);

        const char* general = o.attr("IGeneralDataBlockSourceData", "Type") ? "IGeneralDataBlockSourceData"
                                                                             : "IGeneralDatablockData";
        const std::string type = attrStr(o, general, "Type");
        if (type == "SharedDB") db.kind = "global";
        else if (type.compare(0, 3, "IDB") == 0) db.kind = "instance";
        else db.kind = type;
        if (db.kind == "instance") {
            db.instanceOf = attrStr(o, general, "OfName");
            Key src;
            if (db.instanceOf.empty() && o.relationTarget(relGeneratedFrom_, src)) db.instanceOf = nameOf(src);
        }

        std::string family;
        if (const Value* v = o.expandoValue("TargetFamily"))
            if (v->type == Value::Type::String) family = v->s;

        Key iface;
        if (!relInterface_ || !o.relationTarget(relInterface_, iface)) {
            db.notes.push_back("no interface found");
            return db;
        }
        std::shared_ptr<Part> top = loadPart(iface);

        // Start values: a <Values> document on the block's own interface object
        // or on one of the objects related to it.
        std::map<std::string, std::string> overrides;
        valuesOf(top, overrides);

        // An instance block's own interface object only holds values; the
        // members come from the interface of the block it is an instance of.
        std::shared_ptr<Part> source = top;
        if (!source->root || source->root->name != "Root") {
            source = nullptr;
            for (const Key& k : top->subparts) {
                if (!k.first && !k.second) continue;
                std::shared_ptr<Part> p = loadPart(k);
                if (p->root && p->root->name == "Root") {
                    source = p;
                    break;
                }
            }
        }
        if (!source) {
            db.notes.push_back("interface not readable");
            return db;
        }
        std::map<std::string, std::string> rootDefaults;
        if (source != top) valuesOf(source, rootDefaults);

        size_t budget = kMaxMembersPerBlock;
        Context ctx;
        ctx.family = family;
        ctx.overrides = &overrides;
        ctx.budget = &budget;
        const bool offsets = db.hasAccess && !db.symbolicAccessOnly;
        members(*source->root, source, ctx, std::string(), std::string(), &rootDefaults, commentsOf(source->key), 0,
                offsets, 0, true, std::string(), db.members);
        db.memberCount = count(db.members);
        if (budget == 0) db.notes.push_back("member list cut off");
        if (anyUnresolved(db.members)) db.notes.push_back("some nested types could not be followed");
        return db;
    }
};

}  // namespace

std::string formatOffset(const BlockMember& m) {
    if (!m.hasOffset) return std::string();
    std::string s = std::to_string(m.offsetBits / 8);
    if (m.isBit) s += "." + std::to_string(m.offsetBits % 8);
    return s;
}

ProgramData buildProgramData(const Project& project) { return Builder(project).run(); }

}  // namespace tia
