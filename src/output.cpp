// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>

namespace tia {
namespace {

// JSON string body; invalid UTF-8 is replaced so the output always parses.
std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        o += buf;
                    } else {
                        o += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        size_t len = (c >= 0xf0 && c <= 0xf4) ? 4 : (c >= 0xe0) ? 3 : (c >= 0xc2) ? 2 : 0;
        bool ok = len != 0 && c < 0xf5 && i + len <= n;
        for (size_t k = 1; ok && k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) ok = false;
        if (ok) {
            o.append(s, i, len);
            i += len;
        } else {
            o += "\xef\xbf\xbd";
            ++i;
        }
    }
    return o;
}

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

// "value" or null for an empty string
std::string qn(const std::string& s) { return s.empty() ? "null" : q(s); }

const char* tf(bool b) { return b ? "true" : "false"; }

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}

std::string hexId(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

std::string netTypeName(int64_t t) {
    // Inferred from sample projects: 3 on PROFINET/Ethernet nodes, 2 on MPI/PROFIBUS nodes.
    if (t == 3) return "ethernet";
    if (t == 2) return "profibus-mpi";
    return std::string();
}

std::string dash(const std::string& s) { return s.empty() ? "-" : s; }

// Display width of a UTF-8 string, counting code points.
size_t width(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) ++w;
    return w;
}

std::string pad(const std::string& s, size_t w) {
    size_t cur = width(s);
    return cur >= w ? s : s + std::string(w - cur, ' ');
}

}  // namespace

namespace {

// Comments can hold line breaks; the text report keeps one line per item.
std::string oneLine(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\r') continue;
        out += (c == '\n' || c == '\t') ? ' ' : c;
    }
    return out;
}

std::string blockKind(const DataBlock& b) {
    if (b.kind == "instance" && !b.instanceOf.empty()) return "instance of " + b.instanceOf;
    return b.kind.empty() ? "-" : b.kind;
}

std::string blockAccess(const DataBlock& b) {
    if (!b.hasAccess) return "-";
    return b.symbolicAccessOnly ? "optimized" : "standard";
}

void textMembers(std::ostream& out, const std::vector<BlockMember>& members, int depth) {
    for (const auto& m : members) {
        out << std::string(8 + 2 * static_cast<size_t>(depth), ' ');
        if (m.hasOffset) out << pad(formatOffset(m), 8);
        out << m.name << " : " << dash(m.dataType);
        if (m.hasStartValue) out << " := " << m.startValue;
        if (!m.section.empty()) out << "   [" << m.section << "]";
        if (m.unresolved) out << "   (type not followed)";
        if (!m.comment.empty()) out << "   // " << oneLine(m.comment);
        out << "\n";
        textMembers(out, m.members, depth + 1);
    }
}

void textProgram(std::ostream& out, const ProgramData& prog, const ReportContext& ctx) {
    if (!prog.tags.empty()) {
        out << "\nTags:\n";
        size_t wPlc = 3, wTable = 5, wName = 4, wType = 4;
        for (const auto& t : prog.tags) {
            wPlc = std::max(wPlc, width(t.plc));
            wTable = std::max(wTable, width(t.table));
            wName = std::max(wName, width(t.name));
            wType = std::max(wType, width(t.dataType));
        }
        size_t wAddr = 7;
        bool anyComment = false;
        for (const auto& t : prog.tags) {
            wAddr = std::max(wAddr, width(t.address));
            anyComment = anyComment || !t.comment.empty();
        }
        out << "  " << pad("PLC", wPlc + 2) << pad("Table", wTable + 2) << pad("Name", wName + 2)
            << pad("Type", wType + 2) << (anyComment ? pad("Address", wAddr + 2) + "Comment" : "Address") << "\n";
        for (const auto& t : prog.tags) {
            out << "  " << pad(dash(t.plc), wPlc + 2) << pad(dash(t.table), wTable + 2) << pad(t.name, wName + 2)
                << pad(dash(t.dataType), wType + 2);
            if (t.comment.empty()) out << dash(t.address) << "\n";
            else out << pad(dash(t.address), wAddr + 2) << oneLine(t.comment) << "\n";
        }
    }
    if (!prog.blocks.empty()) {
        out << "\nData blocks:\n";
        size_t wPlc = 3, wName = 4, wKind = 4;
        for (const auto& b : prog.blocks) {
            wPlc = std::max(wPlc, width(b.plc));
            wName = std::max(wName, width(b.name));
            wKind = std::max(wKind, width(blockKind(b)));
        }
        out << "  " << pad("PLC", wPlc + 2) << pad("Block", 9) << pad("Name", wName + 2) << pad("Kind", wKind + 2)
            << pad("Access", 11) << "Members\n";
        for (const auto& b : prog.blocks) {
            out << "  " << pad(dash(b.plc), wPlc + 2) << pad(b.hasNumber ? "DB" + std::to_string(b.number) : "-", 9)
                << pad(b.name, wName + 2) << pad(blockKind(b), wKind + 2) << pad(blockAccess(b), 11)
                << b.memberCount;
            for (const auto& n : b.notes) out << "  (" << n << ")";
            if (!b.comment.empty()) out << "  // " << oneLine(b.comment);
            out << "\n";
            if (ctx.members) textMembers(out, b.members, 0);
        }
    }
}

void jsonMembers(std::ostream& out, const std::vector<BlockMember>& members, const std::string& indent) {
    if (members.empty()) {
        out << "[]";
        return;
    }
    out << "[";
    for (size_t i = 0; i < members.size(); ++i) {
        const BlockMember& m = members[i];
        out << (i ? ",\n" : "\n") << indent << "  {\"name\": " << q(m.name) << ", \"data_type\": " << qn(m.dataType);
        if (!m.section.empty()) out << ", \"section\": " << q(m.section);
        if (m.hasOffset) out << ", \"offset\": " << q(formatOffset(m)) << ", \"offset_bits\": " << m.offsetBits;
        if (m.hasStartValue) out << ", \"start_value\": " << q(m.startValue);
        if (!m.comment.empty()) out << ", \"comment\": " << q(m.comment);
        if (m.unresolved) out << ", \"unresolved\": true";
        if (!m.members.empty()) {
            out << ", \"members\": ";
            jsonMembers(out, m.members, indent + "  ");
        }
        out << "}";
    }
    out << "\n" << indent << "]";
}

void jsonProgram(std::ostream& out, const ProgramData& prog) {
    out << "  \"tags\": [";
    for (size_t i = 0; i < prog.tags.size(); ++i) {
        const Tag& t = prog.tags[i];
        out << (i ? ",\n" : "\n") << "    {\"plc\": " << qn(t.plc) << ", \"table\": " << qn(t.table) << ", \"name\": "
            << q(t.name) << ", \"data_type\": " << qn(t.dataType) << ", \"address\": " << qn(t.address)
            << ", \"comment\": " << qn(t.comment) << "}";
    }
    out << (prog.tags.empty() ? "],\n" : "\n  ],\n");
    out << "  \"data_blocks\": [";
    for (size_t i = 0; i < prog.blocks.size(); ++i) {
        const DataBlock& b = prog.blocks[i];
        out << (i ? ",\n" : "\n") << "    {\"plc\": " << qn(b.plc) << ", \"name\": " << q(b.name) << ", \"number\": ";
        if (b.hasNumber) out << b.number;
        else out << "null";
        out << ", \"address\": " << qn(b.address) << ", \"comment\": " << qn(b.comment) << ", \"kind\": "
            << qn(b.kind) << ", \"instance_of\": "
            << qn(b.instanceOf) << ", \"optimized_access\": " << (b.hasAccess ? tf(b.symbolicAccessOnly) : "null")
            << ", \"member_count\": " << b.memberCount << ", \"notes\": [";
        for (size_t n = 0; n < b.notes.size(); ++n) out << (n ? ", " : "") << q(b.notes[n]);
        out << "], \"members\": ";
        jsonMembers(out, b.members, "    ");
        out << "}";
    }
    out << (prog.blocks.empty() ? "],\n" : "\n  ],\n");
}

void csvRow(std::ostream& out, std::initializer_list<std::string> cols) {
    bool first = true;
    for (const auto& c : cols) {
        if (!first) out << ',';
        first = false;
        out << csvField(c);
    }
    out << "\r\n";
}

void csvMembers(std::ostream& out, const DataBlock& b, const std::vector<BlockMember>& members,
                const std::string& prefix) {
    for (const auto& m : members) {
        const std::string path = prefix.empty() ? m.name : prefix + "." + m.name;
        csvRow(out, {b.plc, b.hasNumber ? "DB" + std::to_string(b.number) : "", b.name, b.kind, b.instanceOf,
                     blockAccess(b), m.section, path, m.dataType, formatOffset(m), m.startValue, m.comment});
        csvMembers(out, b, m.members, path);
    }
}

}  // namespace

void writeTagsCsv(std::ostream& out, const ProgramData& prog) {
    out << "plc,table,name,data_type,address,comment\r\n";
    for (const auto& t : prog.tags) csvRow(out, {t.plc, t.table, t.name, t.dataType, t.address, t.comment});
}

void writeBlocksCsv(std::ostream& out, const ProgramData& prog) {
    out << "plc,block,block_name,kind,instance_of,access,section,member,data_type,offset,start_value,comment\r\n";
    for (const auto& b : prog.blocks) {
        if (b.members.empty())
            csvRow(out, {b.plc, b.hasNumber ? "DB" + std::to_string(b.number) : "", b.name, b.kind, b.instanceOf,
                         blockAccess(b), "", "", "", "", "", ""});
        csvMembers(out, b, b.members, std::string());
    }
}

void writeText(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx) {
    out << "Source:   " << ctx.source << "  (layout " << ctx.layout << ")\n";
    if (inv.project.found) {
        out << "Project:  " << inv.project.name << "\n";
        out << "Created:  " << dash(inv.project.created) << "  by " << dash(inv.project.author) << "\n";
        out << "Modified: " << dash(inv.project.modified) << "  by " << dash(inv.project.lastModifiedBy) << "\n";
    }
    size_t shown = 0, hidden = 0;
    for (const auto& d : inv.devices) {
        if (!d.inProject && !ctx.allDevices) {
            ++hidden;
            continue;
        }
        ++shown;
        out << "\nDevice: " << d.name;
        if (!d.type.empty()) out << "  [" << d.type << "]";
        if (!d.inProject) out << "  (outside the project tree, under " << dash(d.parentType) << ")";
        out << "\n";
        size_t wName = 4, wType = 4, wOrder = 12, wFw = 8;
        for (const auto& m : d.modules) {
            wName = std::max(wName, width(m.name));
            wType = std::max(wType, width(m.typeName.empty() ? m.type : m.typeName));
            wOrder = std::max(wOrder, width(m.orderNumber));
            wFw = std::max(wFw, width(m.firmware));
        }
        out << "  " << pad("Pos", 6) << pad("Name", wName + 2) << pad("Type", wType + 2)
            << pad("Order number", wOrder + 2) << pad("Firmware", wFw + 2) << "In\n";
        for (const auto& m : d.modules) {
            out << "  " << pad(m.hasPosition ? std::to_string(m.position) : "-", 6) << pad(m.name, wName + 2)
                << pad(dash(m.typeName.empty() ? m.type : m.typeName), wType + 2) << pad(dash(m.orderNumber), wOrder + 2)
                << pad(dash(m.firmware), wFw + 2) << dash(m.container) << "\n";
            for (const auto& i : m.interfaces) {
                out << "        " << i.name;
                if (!i.item.empty()) out << " (" << i.item << ")";
                if (i.hasIpSettings) {
                    out << "  IP " << dash(i.ip) << " / " << dash(i.mask);
                    if (!i.router.empty()) out << "  router " << i.router;
                    if (!i.ipProtocolUsed) out << "  [IP not used]";
                    if (i.ipAssignedElsewhere) out << "  [address assigned outside the project]";
                }
                if (i.hasBusAddress) out << "  bus address " << i.busAddress;
                if (!i.profinetName.empty()) out << "  PROFINET name \"" << i.profinetName << "\"";
                else if (i.profinetNameAuto) out << "  PROFINET name automatic";
                if (!i.subnet.empty()) out << "  subnet " << i.subnet;
                out << "\n";
            }
        }
    }
    if (shown == 0) out << "\nNo devices found.\n";
    if (!inv.subnets.empty()) {
        out << "\nSubnets:\n";
        for (const auto& s : inv.subnets) {
            out << "  " << s.name << "  (" << s.members.size() << " node" << (s.members.size() == 1 ? "" : "s") << ")\n";
            for (const auto& m : s.members)
                out << "      " << pad(dash(m.ip), 17) << m.device << " / " << m.module << " / " << m.node << "\n";
        }
    }
    textProgram(out, prog, ctx);
    out << "\n" << inv.stats.liveObjects << " objects in " << inv.stats.blocks << " blocks";
    if (inv.stats.deletedObjects) out << ", " << inv.stats.deletedObjects << " deleted";
    if (!inv.saves.empty()) out << ", " << inv.saves.size() << " save" << (inv.saves.size() == 1 ? "" : "s") << " recorded";
    if (ctx.hashesVerified) out << ", block hashes " << (ctx.hashErrors ? "FAILED" : "ok");
    out << ".\n";
    if (hidden)
        out << hidden << " device object" << (hidden == 1 ? "" : "s")
            << " outside the project tree not shown (use --all-devices).\n";
    for (const auto& w : inv.warnings) out << "Warning: " << w << "\n";
    for (const auto& w : prog.warnings) out << "Warning: " << w << "\n";
}

void writeJson(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx) {
    out << "{\n";
    out << "  \"tool\": {\"name\": \"tiaconv\", \"version\": " << q(ctx.toolVersion) << "},\n";
    out << "  \"source\": {\"path\": " << q(ctx.source) << ", \"layout\": " << q(ctx.layout)
        << ", \"blocks\": " << inv.stats.blocks << ", \"live_objects\": " << inv.stats.liveObjects
        << ", \"deleted_objects\": " << inv.stats.deletedObjects << ", \"hashes_verified\": "
        << tf(ctx.hashesVerified) << ", \"hash_errors\": " << ctx.hashErrors << ", \"saves\": [";
    for (size_t i = 0; i < inv.saves.size(); ++i) out << (i ? ", " : "") << q(inv.saves[i]);
    out << "]},\n";
    if (inv.project.found) {
        out << "  \"project\": {\"name\": " << q(inv.project.name) << ", \"created\": " << qn(inv.project.created)
            << ", \"modified\": " << qn(inv.project.modified) << ", \"author\": " << qn(inv.project.author)
            << ", \"last_modified_by\": " << qn(inv.project.lastModifiedBy) << "},\n";
    } else {
        out << "  \"project\": null,\n";
    }
    out << "  \"devices\": [";
    for (size_t di = 0; di < inv.devices.size(); ++di) {
        const Device& d = inv.devices[di];
        out << (di ? ",\n" : "\n") << "    {\"name\": " << q(d.name) << ", \"type\": " << qn(d.type)
            << ", \"in_project\": " << tf(d.inProject) << ", \"parent_type\": " << qn(d.parentType)
            << ", \"modules\": [";
        for (size_t mi = 0; mi < d.modules.size(); ++mi) {
            const Module& m = d.modules[mi];
            out << (mi ? ",\n" : "\n") << "      {\"name\": " << q(m.name) << ", \"kind\": " << q(m.kind)
                << ", \"type\": " << qn(m.type) << ", \"type_name\": " << qn(m.typeName)
                << ", \"order_number\": " << qn(m.orderNumber) << ", \"firmware\": " << qn(m.firmware)
                << ", \"position\": ";
            if (m.hasPosition) out << m.position;
            else out << "null";
            out << ", \"item_type\": " << m.itemType << ", \"container\": " << qn(m.container)
                << ", \"author\": " << qn(m.author) << ", \"modified\": " << qn(m.modified) << ", \"interfaces\": [";
            for (size_t ii = 0; ii < m.interfaces.size(); ++ii) {
                const Interface& i = m.interfaces[ii];
                out << (ii ? ",\n" : "\n") << "        {\"name\": " << q(i.name) << ", \"item\": " << qn(i.item)
                    << ", \"node_id\": " << qn(i.nodeId) << ", \"node_type\": " << i.nodeType
                    << ", \"net\": " << qn(netTypeName(i.nodeType));
                if (i.hasIpSettings)
                    out << ", \"ip\": " << qn(i.ip) << ", \"mask\": " << qn(i.mask) << ", \"router\": " << qn(i.router)
                        << ", \"ip_protocol_used\": " << tf(i.ipProtocolUsed) << ", \"ip_assigned_elsewhere\": "
                        << tf(i.ipAssignedElsewhere) << ", \"ip_configuration\": " << i.ipConfiguration
                        << ", \"ip_set_by_user\": " << (i.hasIpSetByUser ? tf(i.ipSetByUser) : "null");
                if (i.hasBusAddress) out << ", \"bus_address\": " << i.busAddress;
                out << ", \"profinet_name\": " << qn(i.profinetName) << ", \"profinet_name_auto\": "
                    << tf(i.profinetNameAuto) << ", \"profinet_name_stored\": " << qn(i.profinetNameStored)
                    << ", \"configured_mac\": " << qn(i.configuredMac)
                    << ", \"subnet\": " << qn(i.subnet) << "}";
            }
            out << (m.interfaces.empty() ? "]}" : "\n      ]}");
        }
        out << (d.modules.empty() ? "]}" : "\n    ]}");
    }
    out << (inv.devices.empty() ? "],\n" : "\n  ],\n");
    out << "  \"subnets\": [";
    for (size_t si = 0; si < inv.subnets.size(); ++si) {
        const Subnet& s = inv.subnets[si];
        out << (si ? ",\n" : "\n") << "    {\"name\": " << q(s.name) << ", \"net_type\": " << s.netType
            << ", \"net\": " << qn(netTypeName(s.netType)) << ", \"members\": [";
        for (size_t mi = 0; mi < s.members.size(); ++mi) {
            const SubnetMember& m = s.members[mi];
            out << (mi ? ", " : "") << "{\"device\": " << q(m.device) << ", \"module\": " << q(m.module)
                << ", \"node\": " << q(m.node) << ", \"ip\": " << qn(m.ip) << "}";
        }
        out << "]}";
    }
    out << (inv.subnets.empty() ? "],\n" : "\n  ],\n");
    jsonProgram(out, prog);
    out << "  \"stats\": {\"decoded_hardware_objects\": " << inv.stats.decodedObjects
        << ", \"objects_with_problems\": " << inv.stats.objectsWithProblems << ", \"unattached_items\": "
        << inv.stats.unattachedItems << "},\n";
    out << "  \"warnings\": [";
    for (size_t i = 0; i < inv.warnings.size(); ++i) out << (i ? ", " : "") << q(inv.warnings[i]);
    for (size_t i = 0; i < prog.warnings.size(); ++i)
        out << ((i || !inv.warnings.empty()) ? ", " : "") << q(prog.warnings[i]);
    out << "]\n}\n";
}

void writeCsv(std::ostream& out, const Inventory& inv, const ReportContext& ctx) {
    out << "device,device_type,in_project,position,kind,name,type,type_name,order_number,firmware,container,"
           "interface,interface_item,ip,mask,router,ip_assigned_elsewhere,ip_set_by_user,bus_address,profinet_name,"
           "profinet_name_auto,subnet\r\n";
    auto row = [&](const Device& d, const Module& m, const Interface* i) {
        const std::string cols[] = {d.name, d.type, d.inProject ? "yes" : "no",
                                    m.hasPosition ? std::to_string(m.position) : "", m.kind, m.name, m.type,
                                    m.typeName, m.orderNumber, m.firmware, m.container,
                                    i ? i->name : "", i ? i->item : "", i ? i->ip : "", i ? i->mask : "",
                                    i ? i->router : "", (i && i->hasIpSettings) ? (i->ipAssignedElsewhere ? "yes" : "no") : "",
                                    (i && i->hasIpSetByUser) ? (i->ipSetByUser ? "yes" : "no") : "",
                                    (i && i->hasBusAddress) ? std::to_string(i->busAddress) : "",
                                    i ? i->profinetName : "", i ? (i->profinetNameAuto ? "yes" : "no") : "",
                                    i ? i->subnet : ""};
        bool first = true;
        for (const auto& col : cols) {
            if (!first) out << ',';
            first = false;
            out << csvField(col);
        }
        out << "\r\n";
    };
    for (const auto& d : inv.devices) {
        if (!d.inProject && !ctx.allDevices) continue;
        for (const auto& m : d.modules) {
            if (m.interfaces.empty()) row(d, m, nullptr);
            for (const auto& i : m.interfaces) row(d, m, &i);
        }
    }
}

namespace {

void writeValue(std::ostream& out, const Value& v) {
    switch (v.type) {
        case Value::Type::Null: out << "null"; break;
        case Value::Type::Bool: out << tf(v.b); break;
        case Value::Type::Int: out << v.i; break;
        case Value::Type::UInt: out << v.u; break;
        case Value::Type::Float:
            if (std::isfinite(v.f)) out << std::setprecision(17) << v.f;
            else out << "null";
            break;
        case Value::Type::String:
        case Value::Type::DateTime: out << q(v.s); break;
        case Value::Type::Bytes: out << "{\"bytes\": " << v.s.size() << "}"; break;
        case Value::Type::Text: {
            // one entry per language id; "-" for the entry without a language
            out << "{\"text\": {";
            for (size_t i = 0; i < v.texts.size(); ++i)
                out << (i ? ", " : "") << q(v.texts[i].first == 0xffff ? "-" : std::to_string(v.texts[i].first)) << ": "
                    << q(v.texts[i].second);
            out << "}}";
            break;
        }
        case Value::Type::Opaque: out << "{\"opaque\": true}"; break;
    }
}

void writeValues(std::ostream& out, const NamedValues& vals) {
    out << "{";
    bool first = true;
    for (const auto& kv : vals) {
        if (!first) out << ", ";
        first = false;
        out << q(kv.first) << ": ";
        writeValue(out, kv.second);
    }
    out << "}";
}

}  // namespace

void writeObjects(std::ostream& out, const Project& project) {
    const Container& c = project.container();
    const MetaModel& meta = project.meta();
    for (const auto& kv : c.latest()) {
        const Block& b = c.blocks()[kv.second];
        if (c.isSystem(b) || b.deleted()) continue;
        Object o;
        try {
            if (!project.decode(b, o)) {
                out << "{\"type_id\": " << q(hexId(b.type)) << ", \"id\": " << b.id << ", \"undecoded\": true}\n";
                continue;
            }
        } catch (const ParseError& e) {
            out << "{\"type_id\": " << q(hexId(b.type)) << ", \"id\": " << b.id << ", \"error\": " << q(e.what())
                << "}\n";
            continue;
        }
        out << "{\"type\": " << q(o.def->name) << ", \"type_id\": " << q(hexId(o.type)) << ", \"id\": " << o.id
            << ", \"name\": " << qn(o.attrString("ICoreAttributes", "Name")) << ", \"attributes\": {";
        bool first = true;
        for (const auto& s : o.sets) {
            if (!first) out << ", ";
            first = false;
            out << q(s.first) << ": ";
            writeValues(out, s.second);
        }
        out << "}, \"expando\": ";
        writeValues(out, o.expando);
        out << ", \"relations\": [";
        first = true;
        for (const auto& r : o.relations) {
            if (!first) out << ", ";
            first = false;
            const RelationDef* rd = meta.relation(r.relation);
            const TypeDef* tt = meta.findById(r.targetType);
            out << "{\"relation\": " << (rd ? q(rd->name) : std::string("null")) << ", \"slot\": " << r.slot
                << ", \"target_type\": " << (tt ? q(tt->shortName()) : q(hexId(r.targetType)))
                << ", \"target_id\": " << r.targetId << "}";
        }
        out << "]";
        if (!o.problems.empty()) {
            out << ", \"problems\": [";
            for (size_t i = 0; i < o.problems.size(); ++i) out << (i ? ", " : "") << q(o.problems[i]);
            out << "]";
        }
        out << "}\n";
    }
}

}  // namespace tia
