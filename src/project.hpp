// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A project = container + type model + the key tables for "expando"
// (dynamically named) attributes. Decodes object blocks into attributes and
// relations.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "container.hpp"
#include "meta.hpp"

namespace tia {

struct Value {
    enum class Type { Null, Bool, Int, UInt, Float, String, Bytes, DateTime, Text, Opaque };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0;
    std::string s;  // String, Bytes (raw), DateTime (ISO 8601), Text (see below)
    // Text: one entry per language, Windows language id (0x0409 = en-US) and
    // text; 0xFFFF marks the entry without a language. `s` holds the text to
    // show: that entry when it is not empty, otherwise the first that is.
    std::vector<std::pair<uint16_t, std::string>> texts;

    bool isNull() const { return type == Type::Null; }
    // Numeric view of Bool / Int / UInt values.
    bool asUInt(uint64_t& out) const;
    bool truthy() const;
    const std::string& str() const { return s; }
};

using NamedValues = std::vector<std::pair<std::string, Value>>;

struct RelationEntry {
    uint32_t relation = 0;  // relation id; 0 in typed lists that do not carry one
    uint32_t targetType = 0;
    uint64_t targetId = 0;
    uint32_t slot = 0;
};

struct Object {
    uint32_t type = 0;
    uint64_t id = 0;
    const TypeDef* def = nullptr;
    std::vector<std::pair<std::string, NamedValues>> sets;  // attribute-set short name -> values
    NamedValues expando;
    std::vector<RelationEntry> relations;
    std::vector<std::string> problems;

    const Value* attr(const std::string& setShortName, const std::string& name) const;
    const Value* expandoValue(const std::string& name) const;
    std::string attrString(const std::string& setShortName, const std::string& name) const;
    // Target of the first relation entry with this id.
    bool relationTarget(uint32_t relationId, std::pair<uint32_t, uint64_t>& out) const;
};

class Project {
public:
    explicit Project(Container container);
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    const Container& container() const { return container_; }
    const MetaModel& meta() const { return meta_; }

    size_t metaDocuments() const { return metaXml_.size(); }
    // The embedded type model documents as stored in the file (XML).
    const std::vector<std::string>& metaXml() const { return metaXml_; }
    size_t expandoTables() const { return expando_.size(); }

    // Decodes the latest version of an object. Returns false for system
    // objects and types the model does not describe.
    bool decode(const Block& b, Object& out) const;

    // Latest live (not deleted) version of an object, or nullptr.
    const Block* live(uint32_t type, uint64_t id) const;

private:
    struct ExpandoKey {
        std::string name;
        uint32_t typeId = 0;
    };

    Container container_;
    MetaModel meta_;
    std::vector<std::string> metaXml_;
    std::map<uint32_t, std::map<uint32_t, ExpandoKey>> expando_;  // object type id -> key -> name/type

    void loadSystemObjects();
    bool loadExpandoTable(Span payload);
    Value readValue(const Storage& st, const TypeDef* enumType, Span seg, size_t pos) const;
    static Value readText(Span seg, size_t offset);
    void decodeExpando(uint32_t objectType, Span seg, Object& out) const;
    void decodeRelations(Span block, const Block& b, size_t firstSlot, Object& out) const;
};

// .NET DateTime ticks (kind bits in the top two bits) as ISO 8601; empty for zero.
std::string formatTicks(uint64_t ticks);

// Unpacks a stored blob value (Value::Type::Bytes). Blobs are kept either
// plain, as pages, or as zlib-compressed pages. Returns false when the data
// is not a recognisable blob.
bool decodeBlob(const std::string& raw, std::string& out);

}  // namespace tia
