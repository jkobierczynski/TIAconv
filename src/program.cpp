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
        relFolder_ = meta_.relationId("FolderElementData", "AggregatingFolder");
        relBinaries_ = meta_.relationId("Parent4LoadableBinaryData", "LoadableBinaries");
        relSources_ = meta_.relationId("Parent4SourceData", "Sources");
        relBlockComment_ = meta_.relationId("GeneralBlockSourceData", "BlockComment");
        relBlockCommentCode_ = meta_.relationId("CodeBlockData", "BlockComment");
        relBlockCommentData_ = meta_.relationId("DataBlockData", "BlockComment");
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
            const bool listed = isListedBlock(*t);
            if (role == 0 && !listed) continue;
            Object o;
            try {
                if (!project_.decode(b, o)) continue;
            } catch (const ParseError&) {
                continue;
            }
            if (role == 3 || role == 4) noteComments(o);
            if (listed) {
                if (inProject(o)) out.blockList.push_back(makeInfo(o));
                else ++out.stats.listedBlocksOutsideProject;
            }
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
        std::stable_sort(out.blockList.begin(), out.blockList.end(), [](const BlockInfo& a, const BlockInfo& b) {
            if (a.plc != b.plc) return a.plc < b.plc;
            const int ra = typeRank(a.type), rb = typeRank(b.type);
            if (ra != rb) return ra < rb;
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
             relSubparts_ = 0, relComments_ = 0, relFolder_ = 0, relBinaries_ = 0, relSources_ = 0,
             relBlockComment_ = 0, relBlockCommentCode_ = 0, relBlockCommentData_ = 0;
    std::map<uint32_t, bool> listed_;
    struct FolderRec {
        std::string name, subtype;
        Key parent{0, 0};
        bool hasParent = false;
    };
    std::map<Key, FolderRec> folders_;
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

    // Blocks and data types: everything TIA Portal lists under "Program
    // blocks" and "PLC data types".
    bool isListedBlock(const TypeDef& t) {
        auto it = listed_.find(t.id);
        if (it != listed_.end()) return it->second;
        bool yes = false;
        for (const char* base : {"CodeBlockData", "DataBlockData", "UserTypeData", "SystemDatatypeData"})
            yes = yes || meta_.derivesFromShort(t.name, base);
        listed_[t.id] = yes;
        return yes;
    }

    // The order of TIA Portal's block overview: organization blocks first.
    static int typeRank(const std::string& type) {
        static const char* const order[] = {"OB", "FB", "FC", "DB", "UDT", "SFB", "SFC", "SDT"};
        for (int i = 0; i < 8; ++i)
            if (type == order[i]) return i;
        return 8;
    }

    const FolderRec& folder(const Key& k) {
        auto it = folders_.find(k);
        if (it != folders_.end()) return it->second;
        FolderRec r;
        Object o;
        if (decodeKey(k, o)) {
            r.name = attrStr(o, "ICoreAttributes", "Name");
            r.subtype = attrStr(o, "ICoreAttributes", "Subtype");
            r.hasParent = relFolder_ && o.relationTarget(relFolder_, r.parent);
        }
        return folders_[k] = r;
    }

    // Where the block sits in the project tree, with the names of the English
    // user interface for the folders TIA Portal provides.
    void folderOf(const Object& o, BlockInfo& b) {
        Key k;
        if (!relFolder_ || !o.relationTarget(relFolder_, k)) return;
        std::vector<std::string> path;
        for (int depth = 0; depth < 32; ++depth) {
            const FolderRec& f = folder(k);
            const std::string& st = f.subtype;
            static const char kSub[] = ".Subfolder";
            const size_t n = sizeof kSub - 1;
            if (st == "ProgramBlocksFolder") path.push_back("Program blocks");
            else if (st == "SystemBlocksFolder") path.push_back("System blocks");
            else if (st == "ProgramResourcesFolder") path.push_back("Program resources");
            else if (st == "ControllerDataTypeFolder") path.push_back("PLC data types");
            else if (st == "SystemDataTypeFolder") path.push_back("System data types");
            else if (st == "TechnologicalParamFolder") path.push_back("Technology objects");
            else if (st.size() > n && st.compare(st.size() - n, n, kSub) == 0) path.push_back(f.name);
            else if (!st.empty() || !f.name.empty()) path.push_back(f.name.empty() ? st : f.name);
            if (st == "SystemBlocksFolder" || st == "SystemDataTypeFolder") b.system = true;
            if (!f.hasParent) break;
            k = f.parent;
        }
        for (auto it = path.rbegin(); it != path.rend(); ++it) b.folder += (b.folder.empty() ? "" : "/") + *it;
    }

    void dbKind(const Object& o, std::string& kind, std::string& instanceOf) {
        const char* general = o.attr("IGeneralDataBlockSourceData", "Type") ? "IGeneralDataBlockSourceData"
                                                                             : "IGeneralDatablockData";
        const std::string type = attrStr(o, general, "Type");
        if (type == "SharedDB") kind = "global";
        else if (type.compare(0, 3, "IDB") == 0) kind = "instance";
        else kind = type;
        if (kind == "instance") {
            instanceOf = attrStr(o, general, "OfName");
            Key src;
            if (instanceOf.empty() && o.relationTarget(relGeneratedFrom_, src)) instanceOf = nameOf(src);
        }
    }

    static void readFlag(const Object& o, const char* name, Flag& f) {
        const Value* v = o.expandoValue(name);
        if (!v || v->type != Value::Type::Bool) return;
        f.stored = true;
        f.on = v->b;
    }

    BlockInfo makeInfo(const Object& o) {
        const char* core = "ICoreAttributes";
        const char* gen = o.attr("IGeneralBlockSourceData", "Number") ? "IGeneralBlockSourceData" : "IGeneralBlockData";
        BlockInfo b;
        b.id = o.id;
        b.name = attrStr(o, core, "Name");
        b.plc = plcOf(o);
        b.hasNumber = attrInt(o, gen, "Number", b.number) || attrInt(o, "IGeneralUserDatatypeData", "Number", b.number) ||
                      attrInt(o, "IGeneralSystemDatatypeData", "Number", b.number);

        // "OB.ProgramCycle", "FB", "DB.TO.PID.Compact.Compact_3.0"
        const std::string subtype = attrStr(o, core, "Subtype");
        const size_t dot = subtype.find('.');
        b.type = attrStr(o, "IGeneralBlockSourceData", "BlockType");
        if (b.type.empty()) b.type = attrStr(o, "IGeneralBlockData", "DosType");
        if (b.type.empty()) b.type = subtype.substr(0, dot);
        if (o.def && meta_.derivesFromShort(o.def->name, "DataBlockData")) dbKind(o, b.kind, b.instanceOf);
        else if (dot != std::string::npos) b.kind = subtype.substr(dot + 1);

        b.languageStored = attrStr(o, gen, "BlockLanguage");
        b.language = languageName(b.languageStored);
        b.protectionStored = attrStr(o, core, "Protection");
        if (b.protectionStored == "KnowHowProtection") b.protection = "know-how";
        else if (b.protectionStored == "WriteProtection") b.protection = "write";
        else if (b.protectionStored == "SystemKnowHowProtection") b.protection = "system";
        else if (b.protectionStored != "NoProtection") b.protection = b.protectionStored;
        // Write protection of a code block is a setting of its own, next to
        // the protection mode.
        readFlag(o, "WriteProtection", b.writeProtection);
        if (b.writeProtection.stored && b.writeProtection.on && b.protection != "write")
            b.protection += b.protection.empty() ? "write" : ", write";
        b.copyProtectionStored = attrStr(o, "IGeneralBindingData", "CopyProtectionMode");
        if (b.copyProtectionStored.empty()) b.copyProtectionStored = attrStr(o, "IGeneralBlockData", "CopyProtectionMode");
        b.copyProtectionSerial = attrStr(o, "IGeneralBindingData", "CopyProtectionSerialNumber");
        if (b.copyProtectionSerial.empty())
            b.copyProtectionSerial = attrStr(o, "IGeneralBlockData", "CopyProtectionSerialNumber");
        if (b.copyProtectionStored == "BindToPLC") b.copyProtection = "cpu";
        else if (b.copyProtectionStored == "BindToSDCard") b.copyProtection = "memory-card";
        else if (b.copyProtectionStored != "NoBinding") b.copyProtection = b.copyProtectionStored;
        folderOf(o, b);

        b.title = attrText(o, core, "Comment");
        for (uint32_t rel : {relBlockComment_, relBlockCommentCode_, relBlockCommentData_}) {
            Key k;
            Object text;
            if (rel && o.relationTarget(rel, k) && decodeKey(k, text)) {
                b.comment = attrText(text, "ICoreTextRepository", "Text");
                break;
            }
        }
        b.author = attrStr(o, "IPlcHeaderData", "HeaderAuthor");
        b.family = attrStr(o, "IPlcHeaderData", "HeaderFamily");
        b.userId = attrStr(o, "IPlcHeaderData", "HeaderName");
        b.version = attrStr(o, "IPlcHeaderData", "HeaderVersion");
        b.hasAccess = attrBool(o, "IGeneralBlockSourceData", "OnlySymbolicAccess", b.symbolicAccessOnly) ||
                      attrBool(o, "IIecplObjectData", "OnlySymbolicAccess", b.symbolicAccessOnly);

        b.created = attrStr(o, core, "CreationTime");
        b.modified = attrStr(o, "ITimestampData", "Modified");
        b.codeModified = attrStr(o, "ITimestampData", "CodeModified");
        b.interfaceModified = attrStr(o, "ITimestampData", "InterfaceModified");

        // The result of the last compilation is an object of its own; older
        // projects keep the two times on the block.
        b.compileNeeded = attrStr(o, "IGeneralBlockSourceData", "CompileArtifacts");
        if (b.compileNeeded.empty()) b.compileNeeded = attrStr(o, "IGeneralBlockData", "CompileStatus");
        b.compiled = attrStr(o, "IGeneralBlockData", "CompileTime");
        b.downloaded = attrStr(o, "IGeneralBlockData", "DownloadTime");
        Key bin;
        Object result;
        // Memory sizes are those of the last compilation. TIA Portal shows
        // none for a block that has to be compiled again, so neither does
        // tiaconv; a data type takes load memory only.
        const bool stale = !b.compileNeeded.empty() && b.compileNeeded != "UpToDate";
        const bool dataType = b.type == "UDT" || b.type == "SDT";
        if (relBinaries_ && o.relationTarget(relBinaries_, bin) && decodeKey(bin, result)) {
            for (const char* set : {"IGeneralBlockResultData", "ILoadableObjectOmspData"}) {
                if (b.compiled.empty()) b.compiled = attrStr(result, set, "CompileTime");
                if (b.downloaded.empty()) b.downloaded = attrStr(result, set, "DownloadTime");
                if (stale) continue;
                b.hasLoadMemory = b.hasLoadMemory || attrInt(result, set, "LoadMemoryRequired", b.loadMemory);
                if (!dataType)
                    b.hasWorkMemory = b.hasWorkMemory || attrInt(result, set, "WorkMemoryRequired", b.workMemory);
            }
        }
        // The size TIA Portal lists under Program info > Resources is kept on
        // the block itself; it is removed when the block changes.
        if (const Value* v = o.expandoValue("LoadMemorySize")) {
            uint64_t u = 0;
            if (v->asUInt(u) && !stale) {
                b.hasLoadMemory = true;
                b.loadMemory = static_cast<int64_t>(u);
            }
        }
        if (const Value* v = o.expandoValue("DownloadHistory"))
            if (v->type == Value::Type::String) b.downloads = parseDownloadHistory(v->s);
        if (b.downloaded.empty() && !b.downloads.empty()) b.downloaded = b.downloads.front();

        if (relSources_ && o.def && meta_.derivesFromShort(o.def->name, "CodeBlockData")) {
            for (const auto& r : o.relations)
                if (r.relation == relSources_ && (r.targetType || r.targetId)) ++b.networks;
            b.hasNetworks = b.networks > 0;
        }
        readFlag(o, "IsWriteProtectedInAS", b.writeProtectedInDevice);
        readFlag(o, "Unlinked", b.onlyInLoadMemory);
        readFlag(o, "DBAccessibleFromOPCUA", b.accessibleFromOpcUa);
        readFlag(o, "DBAccessibleFromWebserver", b.accessibleFromWebServer);
        return b;
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
        db.title = attrText(o, "ICoreAttributes", "Comment");
        for (uint32_t rel : {relBlockComment_, relBlockCommentData_}) {
            Key k;
            Object text;
            if (rel && o.relationTarget(rel, k) && decodeKey(k, text)) {
                db.comment = attrText(text, "ICoreTextRepository", "Text");
                break;
            }
        }
        db.plc = plcOf(o);
        db.hasNumber = attrInt(o, "IGeneralBlockSourceData", "Number", db.number) ||
                       attrInt(o, "IGeneralBlockData", "Number", db.number);
        db.hasAccess = attrBool(o, "IGeneralBlockSourceData", "OnlySymbolicAccess", db.symbolicAccessOnly) ||
                       attrBool(o, "IIecplObjectData", "OnlySymbolicAccess", db.symbolicAccessOnly);

        dbKind(o, db.kind, db.instanceOf);

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

// "FB3-638682897237619877;FB3-...;FB?-0": the block's address at each
// download and the time as .NET ticks, newest first; 0 = never.
std::vector<std::string> parseDownloadHistory(const std::string& stored) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < stored.size() && out.size() < 64) {
        size_t end = stored.find(';', pos);
        if (end == std::string::npos) end = stored.size();
        const std::string entry = stored.substr(pos, end - pos);
        pos = end + 1;
        const size_t dash = entry.rfind('-');
        if (dash == std::string::npos || dash + 1 >= entry.size() || entry.size() - dash > 20) continue;
        if (entry.find_first_not_of("0123456789", dash + 1) != std::string::npos) continue;
        // The ticks carry no time zone. The newest entry is the block's
        // DownloadTime to the millisecond, which is UTC.
        const uint64_t ticks = std::stoull(entry.substr(dash + 1));
        if (ticks >> 62) continue;  // not a time
        std::string when = formatTicks(ticks);
        if (when.empty()) continue;
        if (when.back() != 'Z') when += 'Z';
        out.push_back(when);
    }
    return out;
}

std::string languageName(const std::string& stored) {
    // The two graphical languages of STEP 7; seen next to TIA Portal's name.
    if (stored == "LAD_CLASSIC") return "LAD";
    if (stored == "FBD_CLASSIC") return "FBD";
    return stored;
}

std::string formatOffset(const BlockMember& m) {
    if (!m.hasOffset) return std::string();
    std::string s = std::to_string(m.offsetBits / 8);
    if (m.isBit) s += "." + std::to_string(m.offsetBits % 8);
    return s;
}

ProgramData buildProgramData(const Project& project) { return Builder(project).run(); }

}  // namespace tia
