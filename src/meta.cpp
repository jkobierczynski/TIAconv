// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta.hpp"

#include <algorithm>
#include <cstdlib>

#include "bytes.hpp"
#include "xml.hpp"

namespace tia {
namespace {

struct BasicType {
    uint32_t id;
    const char* name;
    uint32_t size;
    ValueKind kind;
};

// Ids and sizes of the built-in types. Types without an id were only seen by name.
const BasicType kBasic[] = {
    {0x80000001, "xs:boolean", 1, ValueKind::Bool},
    {0x80000002, "xs:unsignedByte", 1, ValueKind::UInt},
    {0x80000003, "pe:CharT", 2, ValueKind::UInt},
    {0x80000004, "xs:short", 2, ValueKind::Int},
    {0x80000005, "xs:int", 4, ValueKind::Int},
    {0x80000006, "xs:long", 8, ValueKind::Int},
    {0x80000007, "xs:float", 4, ValueKind::Float},
    {0x80000008, "xs:double", 8, ValueKind::Float},
    {0x80000009, "xs:decimal", 4, ValueKind::UInt},
    {0x8000000a, "xs:dateTime", 8, ValueKind::DateTime},
    {0x8000000b, "xs:string", 4, ValueKind::String},
    {0x8000000c, "pe:GuidT", 16, ValueKind::Guid},
    {0x8000000d, "pe:BlobT", 4, ValueKind::Blob},
    {0x8000000e, "pe:XmlT", 4, ValueKind::Blob},
    {0x80000012, "xs:ushort", 2, ValueKind::UInt},
    {0x80000013, "xs:uint", 4, ValueKind::UInt},
    {0x80000014, "xs:ulong", 8, ValueKind::UInt},
    {0x80000015, "pe:MapT", 4, ValueKind::Blob},
    {0x8000001f, "pe:CoreTextAttributeT", 4, ValueKind::Text},
    {0, "xs:unsignedShort", 2, ValueKind::UInt},
    {0, "xs:unsignedInt", 4, ValueKind::UInt},
    {0, "xs:unsignedLong", 8, ValueKind::UInt},
    {0, "xs:byte", 1, ValueKind::Int},
};

const BasicType* basicByName(const std::string& n) {
    for (const auto& b : kBasic)
        if (n == b.name) return &b;
    return nullptr;
}

// Names without a namespace or prefix are relative to the enclosing namespace.
std::string qualify(const std::string& name, const std::string& ns) {
    if (name.find(':') == std::string::npos && name.find('.') == std::string::npos) return ns + "." + name;
    return name;
}

bool parseId(const XmlNode& n, uint32_t& out) {
    const std::string* v = n.attr("id");
    if (!v || v->empty()) return false;
    out = static_cast<uint32_t>(std::strtoul(v->c_str(), nullptr, 0));
    return true;
}

}  // namespace

std::string TypeDef::shortName() const {
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

const char* MetaModel::basicTypeName(uint32_t id) {
    if (id == 0) return nullptr;
    for (const auto& b : kBasic)
        if (b.id == id) return b.name;
    return nullptr;
}

void MetaModel::load(const std::string& xml) {
    auto root = parseXml(xml);
    for (const auto& pkg : root->children) {
        if (pkg->name != "Package") continue;
        for (const auto& nsNode : pkg->children) {
            if (nsNode->name != "Namespace") continue;
            const std::string ns = nsNode->attrOr("name", "");
            for (const auto& c : nsNode->children) {
                const std::string* nm = c->attr("name");
                if (!nm) continue;
                TypeDef t;
                t.name = ns + "." + *nm;
                t.hasId = parseId(*c, t.id);
                if (c->name == "Enumeration") {
                    t.kind = TypeKind::Enumeration;
                    t.base = c->attrOr("base", "xs:int");
                    for (const auto& k : c->children) {
                        if (k->name != "Constant") continue;
                        const std::string* v = k->attr("value");
                        if (v) t.constants[std::strtoll(v->c_str(), nullptr, 0)] = k->attrOr("name", "");
                    }
                } else if (c->name == "AttributeSet") {
                    t.kind = TypeKind::AttributeSet;
                    t.persistent = c->attrIs("persistent", "true");
                    t.expando = c->attr("expando") != nullptr && !c->attrIs("expando", "false");
                    for (const auto& a : c->children) {
                        if (a->name != "Attribute") continue;
                        AttributeDef ad;
                        ad.name = a->attrOr("name", "");
                        ad.type = qualify(a->attrOr("type", ""), ns);
                        ad.constant = a->attrIs("constant", "true");
                        ad.intrinsic = a->attrIs("intrinsic", "true");
                        ad.isVirtual = a->attrIs("virtual", "true");
                        t.attributes.push_back(std::move(ad));
                    }
                } else if (c->name == "ObjectType") {
                    t.kind = TypeKind::ObjectType;
                    for (const auto& k : c->children) {
                        if (k->name == "Base") {
                            t.bases.emplace_back(qualify(k->attrOr("ref", ""), ns), k->attrIs("primary", "true"));
                        } else if (k->name == "Implements") {
                            ImplementsDef im;
                            im.ref = qualify(k->attrOr("ref", ""), ns);
                            for (const auto& a : k->children) {
                                if (a->name != "Attribute") continue;
                                const std::string* cv = a->attr("constant");
                                im.attributes.emplace_back(a->attrOr("name", ""), !cv ? -1 : (*cv == "true" ? 1 : 0));
                            }
                            t.implements.push_back(std::move(im));
                        } else if (k->name == "Relation") {
                            RelationDef r;
                            if (!parseId(*k, r.id)) continue;
                            r.owner = t.name;
                            r.name = k->attrOr("name", "");
                            r.cardinality = k->attrOr("cardinality", "");
                            r.behaviour = k->attrOr("behaviourType", "");
                            size_t dot = r.behaviour.rfind('.');
                            if (dot != std::string::npos) r.behaviour = r.behaviour.substr(dot + 1);
                            relations_[r.id] = std::move(r);
                        }
                    }
                } else if (c->name == "Structure") {
                    t.kind = TypeKind::Structure;
                } else if (c->name == "Array") {
                    t.kind = TypeKind::Array;
                } else {
                    continue;
                }
                types_[t.name] = std::move(t);
            }
        }
    }
    byId_.clear();
    for (const auto& kv : types_)
        if (kv.second.hasId) byId_[kv.second.id] = &kv.second;
    layouts_.clear();
}

const TypeDef* MetaModel::find(const std::string& qualifiedName) const {
    auto it = types_.find(qualifiedName);
    return it == types_.end() ? nullptr : &it->second;
}

const TypeDef* MetaModel::findById(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second;
}

const RelationDef* MetaModel::relation(uint32_t id) const {
    auto it = relations_.find(id);
    return it == relations_.end() ? nullptr : &it->second;
}

uint32_t MetaModel::relationId(const std::string& ownerShortName, const std::string& name) const {
    const std::string suffix = "." + ownerShortName;
    for (const auto& kv : relations_) {
        const RelationDef& r = kv.second;
        if (r.name != name) continue;
        if (r.owner.size() > suffix.size() &&
            r.owner.compare(r.owner.size() - suffix.size(), suffix.size(), suffix) == 0)
            return r.id;
    }
    return 0;
}

Storage MetaModel::storage(const std::string& typeName) const {
    if (const BasicType* b = basicByName(typeName)) return {b->size, b->kind};
    const TypeDef* t = find(typeName);
    if (!t) return {4, ValueKind::Unknown};
    switch (t->kind) {
        case TypeKind::Enumeration: {
            const BasicType* b = basicByName(t->base);
            return {b ? b->size : 4u, ValueKind::Enum};
        }
        case TypeKind::ObjectType:
            return {0, ValueKind::ObjectRef};
        default:
            return {4, ValueKind::Relative};
    }
}

// Order in which the statements of a type and its bases apply: depth first,
// and when a type is reached several times only its last position counts. A
// type therefore always comes after every type derived from it, so a derived
// type's statement overrides its base's whichever branch reaches the base
// first.
void MetaModel::linearize(const std::string& typeName, std::vector<std::string>& order,
                          std::vector<std::string>& path) const {
    if (path.size() > 64 || std::find(path.begin(), path.end(), typeName) != path.end()) return;
    const TypeDef* t = find(typeName);
    if (!t || t->kind != TypeKind::ObjectType) return;
    order.push_back(typeName);
    path.push_back(typeName);
    for (const auto& b : t->bases) linearize(b.first, order, path);
    path.pop_back();
}

// Gathers the attribute sets an object type implements and decides which
// attributes are stored. Types are taken in linearized order; for each
// attribute the first explicit constant="true|false" met decides, an
// <Attribute> without `constant` decides nothing, and without any statement
// the attribute set's own declaration applies.
//
// A V13 project carries the resolved result as a second document
// (StorageMetaInfoXML); this rule reproduces it for all 438 object types. In
// newer versions it makes every attribute segment of the sample projects add
// up exactly. See tools/check_storage_rule.py.
void MetaModel::collect(const std::string& typeName,
                        std::map<std::string, std::map<std::string, bool>>& sets) const {
    std::vector<std::string> visits, path;
    linearize(typeName, visits, path);
    std::vector<std::string> order;
    for (auto it = visits.rbegin(); it != visits.rend(); ++it)
        if (std::find(order.begin(), order.end(), *it) == order.end()) order.push_back(*it);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const TypeDef* t = find(*it);
        for (const auto& im : t->implements) {
            auto& stored = sets[im.ref];
            const TypeDef* set = find(im.ref);
            if (!set) continue;
            for (const auto& ov : im.attributes) {
                if (ov.second == -1 || stored.count(ov.first)) continue;
                for (const auto& a : set->attributes)
                    if (a.name == ov.first) {
                        stored[ov.first] = ov.second == 0 && !a.intrinsic;
                        break;
                    }
            }
        }
    }
}

const std::vector<LayoutSet>& MetaModel::layout(const TypeDef& objectType) const {
    auto cached = layouts_.find(objectType.name);
    if (cached != layouts_.end()) return cached->second;

    std::map<std::string, std::map<std::string, bool>> sets;  // ordered by qualified name = slot order
    collect(objectType.name, sets);

    std::vector<LayoutSet> out;
    for (const auto& kv : sets) {
        LayoutSet ls;
        ls.name = kv.first;
        ls.set = find(kv.first);
        if (ls.set) {
            for (const auto& a : ls.set->attributes) {
                LayoutAttribute la;
                la.def = &a;
                auto ov = kv.second.find(a.name);
                la.persisted = ov != kv.second.end() ? ov->second : (!a.constant && !a.intrinsic);
                la.storage = storage(a.type);
                ls.attributes.push_back(la);
            }
        }
        out.push_back(std::move(ls));
    }
    return layouts_.emplace(objectType.name, std::move(out)).first->second;
}

bool MetaModel::derivesFrom(const std::string& typeName, const std::string& baseName) const {
    if (typeName == baseName) return true;
    std::vector<std::string> todo{typeName};
    std::vector<std::string> seen;
    while (!todo.empty()) {
        std::string cur = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        const TypeDef* t = find(cur);
        if (!t || t->kind != TypeKind::ObjectType) continue;
        for (const auto& b : t->bases) {
            if (b.first == baseName) return true;
            todo.push_back(b.first);
        }
    }
    return false;
}

}  // namespace tia

namespace tia {

bool MetaModel::derivesFromShort(const std::string& typeName, const std::string& shortName) const {
    std::vector<std::string> todo{typeName};
    std::vector<std::string> seen;
    while (!todo.empty()) {
        std::string cur = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        size_t dot = cur.rfind('.');
        if ((dot == std::string::npos ? cur : cur.substr(dot + 1)) == shortName) return true;
        const TypeDef* t = find(cur);
        if (!t || t->kind != TypeKind::ObjectType) continue;
        for (const auto& b : t->bases) todo.push_back(b.first);
    }
    return false;
}

}  // namespace tia
