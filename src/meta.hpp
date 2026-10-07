// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The type model that every project file carries as an embedded XML document:
// object types, the attribute sets they implement, relations, enumerations.
// It is what makes the object blocks decodable without hard-coded offsets.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tia {

enum class TypeKind { Enumeration, AttributeSet, ObjectType, Structure, Array };

// How a value of some type is stored inside an attribute-set segment.
enum class ValueKind {
    Bool,
    UInt,
    Int,
    Float,
    DateTime,
    Guid,
    String,     // 4-byte offset to a length-prefixed string
    Blob,       // 4-byte offset to length-prefixed bytes
    Relative,   // 4-byte offset to a structure or array
    Text,       // 4-byte offset to a text in one or more languages
    Enum,
    ObjectRef,  // stored as a relation, occupies no space in the segment
    Unknown
};

struct Storage {
    uint32_t size = 0;
    ValueKind kind = ValueKind::Unknown;
};

struct AttributeDef {
    std::string name;
    std::string type;  // qualified type name
    bool constant = false;
    bool intrinsic = false;
    bool isVirtual = false;
};

struct ImplementsDef {
    std::string ref;  // qualified attribute-set name
    // attribute name -> constant override: -1 not stated, 0 false, 1 true
    std::vector<std::pair<std::string, int>> attributes;
};

struct RelationDef {
    uint32_t id = 0;
    std::string owner;  // qualified object type name
    std::string name;
    std::string cardinality;
    std::string behaviour;
    bool inverse = false;  // declared as the other direction of a relation of another type
};

struct TypeDef {
    TypeKind kind = TypeKind::ObjectType;
    std::string name;  // qualified
    uint32_t id = 0;
    bool hasId = false;

    // Enumeration
    std::string base;
    std::map<int64_t, std::string> constants;
    // AttributeSet
    bool persistent = false;
    bool expando = false;
    std::vector<AttributeDef> attributes;
    // ObjectType
    std::vector<std::pair<std::string, bool>> bases;  // name, primary
    std::vector<ImplementsDef> implements;
    // Structure / Array are only needed for their storage size.

    std::string shortName() const;
};

struct LayoutAttribute {
    const AttributeDef* def = nullptr;
    bool persisted = false;
    Storage storage;
};

struct LayoutSet {
    std::string name;             // qualified attribute-set name
    const TypeDef* set = nullptr;  // null when the model does not define it
    std::vector<LayoutAttribute> attributes;
};

class MetaModel {
public:
    // Adds the types of one MetaInfo document. Throws ParseError on bad XML.
    void load(const std::string& xml);

    bool empty() const { return types_.empty(); }
    size_t typeCount() const { return types_.size(); }

    const TypeDef* find(const std::string& qualifiedName) const;
    const TypeDef* findById(uint32_t id) const;
    const RelationDef* relation(uint32_t id) const;
    // Relation id by owner short name and relation name, 0 when unknown.
    uint32_t relationId(const std::string& ownerShortName, const std::string& name) const;

    Storage storage(const std::string& typeName) const;

    // Attribute sets of an object type in slot order.
    const std::vector<LayoutSet>& layout(const TypeDef& objectType) const;

    bool derivesFrom(const std::string& typeName, const std::string& baseName) const;
    // True when the type or one of its bases has this unqualified name. Type
    // namespaces moved between TIA versions; short names stayed.
    bool derivesFromShort(const std::string& typeName, const std::string& shortName) const;

    static const char* basicTypeName(uint32_t id);

private:
    std::map<std::string, TypeDef> types_;
    std::map<uint32_t, const TypeDef*> byId_;
    std::map<uint32_t, RelationDef> relations_;
    mutable std::map<std::string, std::vector<LayoutSet>> layouts_;

    void linearize(const std::string& typeName, std::vector<std::string>& order,
                   std::vector<std::string>& path) const;
    void collect(const std::string& typeName, std::map<std::string, std::map<std::string, bool>>& sets) const;
};

}  // namespace tia
