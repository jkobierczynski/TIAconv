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

// Connection IDs the way TIA Portal shows them.
std::string hexId(int64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ", ") + s;
    return out;
}

// Security settings of a controller. Only what the project stores is stated
// as a fact; the rest is listed as not set, because the default depends on
// the CPU and its firmware.
void textSecurity(std::ostream& out, const Module& m) {
    const Security& s = m.security;
    const char* in = "        ";
    std::vector<std::string> unset;
    out << in << "Security settings:\n";
    in = "          ";
    const std::string protection = accessProtection(s);
    // CPUs with user management first say what decides, because the access
    // level below is only part of it there, or nothing at all.
    if (protection == "none") {
        out << in << "Access control: disabled, the CPU has no access protection\n";
    } else if (protection != "access_levels") {
        out << in << "Access control: enabled" << (s.accessControl.stored ? "" : " (TIA Portal default)")
            << ", by users and roles"
            << (protection == "users_and_roles_and_access_levels" ? " and by access levels with passwords" : "") << "\n";
    }
    if (s.hasAccessLevel) {
        // With user management the level is not chosen: TIA Portal derives
        // it from the rights of the Anonymous user and stores it.
        out << in << (protection == "access_levels" || protection == "none" ? "Access level: " : "Access without login: ");
        if (s.accessLevelName.empty()) out << "level " << s.accessLevel;
        else out << s.accessLevelName;
        if (protection == "none") out << "  (stored, not in force)";
        out << "\n";
    } else if (protection == "access_levels") {
        unset.push_back("access level");
    }
    if (protection != "access_levels" && protection != "none")
        out << in << "Users and roles: not read, the project stores their names and passwords in protected form\n";
    auto line = [&](const Setting& v, const char* label, const char* on, const char* off, const char* unsetName) {
        if (v.stored) out << in << label << ": " << (v.on ? on : off) << "\n";
        else unset.push_back(unsetName);
    };
    line(s.putGet, "PUT/GET access", "permitted", "not permitted", "PUT/GET access");
    if (s.webServer.stored) {
        out << in << "Web server: " << (s.webServer.on ? "activated" : "not activated");
        if (s.webServer.on && s.webServerHttpsOnly.stored) out << (s.webServerHttpsOnly.on ? ", HTTPS only" : ", HTTP allowed");
        if (s.webServer.on && !s.webServerInterfaces.empty()) out << ", access enabled on " << joined(s.webServerInterfaces);
        out << "\n";
    } else {
        unset.push_back("web server");
    }
    line(s.opcUaServer, "OPC UA server", "activated", "not activated", "OPC UA server");
    if (s.hasTimeSyncRole) {
        out << in << "Time synchronisation: ";
        if (s.timeSyncRole == 2) out << "NTP" << (s.ntpServers.empty() ? "" : ", server " + joined(s.ntpServers));
        else out << "mode " << s.timeSyncRole;
        out << "\n";
    } else {
        unset.push_back("NTP");
    }
    line(s.displayProtection, "Display protection", "on", "off", "display protection");
    // Settings of current CPUs: said only when stored, an older CPU has none of them.
    if (s.hasCommunicationMode) {
        out << in << "PG/PC and HMI communication: ";
        if (s.communicationMode == 0) out << "legacy communication permitted";
        else out << "mode " << s.communicationMode;
        out << "\n";
    }
    if (s.configDataProtection.stored)
        out << in << "Protection of confidential configuration data: " << (s.configDataProtection.on ? "on" : "off")
            << "\n";
    if (!unset.empty()) out << in << "Not set in the project (TIA Portal default applies): " << joined(unset) << "\n";
}

std::string settingJson(const Setting& v) { return v.stored ? tf(v.on) : "null"; }

void jsonSecurity(std::ostream& out, const Security& s) {
    out << "{\"access_protection\": " << q(accessProtection(s)) << ", \"user_management\": " << tf(s.userManagement)
        << ", \"function_right_set\": " << qn(s.functionRightSet) << ", \"access_level\": ";
    if (s.hasAccessLevel) out << s.accessLevel;
    else out << "null";
    out << ", \"access_level_name\": " << qn(s.accessLevelName) << ", \"put_get\": " << settingJson(s.putGet)
        << ", \"web_server\": " << settingJson(s.webServer) << ", \"web_server_https_only\": "
        << settingJson(s.webServerHttpsOnly) << ", \"web_server_interfaces\": [";
    for (size_t i = 0; i < s.webServerInterfaces.size(); ++i) out << (i ? ", " : "") << q(s.webServerInterfaces[i]);
    out << "], \"opc_ua_server\": " << settingJson(s.opcUaServer) << ", \"time_sync_role\": ";
    if (s.hasTimeSyncRole) out << s.timeSyncRole;
    else out << "null";
    out << ", \"ntp_servers\": [";
    for (size_t i = 0; i < s.ntpServers.size(); ++i) out << (i ? ", " : "") << q(s.ntpServers[i]);
    out << "], \"display_protection\": " << settingJson(s.displayProtection) << ", \"access_control\": "
        << settingJson(s.accessControl) << ", \"access_control_via_access_levels\": "
        << settingJson(s.accessControlViaAccessLevels) << ", \"pg_hmi_communication_mode\": ";
    if (s.hasCommunicationMode) out << s.communicationMode;
    else out << "null";
    out << ", \"legacy_pg_hmi_communication\": " << (s.hasCommunicationMode && s.communicationMode == 0 ? "true" : "null")
        << ", \"config_data_protection\": " << settingJson(s.configDataProtection) << "}";
}

std::string settingCsv(const Setting& v) { return v.stored ? (v.on ? "yes" : "no") : ""; }

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

std::string blockAddress(const BlockInfo& b) {
    if (b.type.empty()) return "-";
    return b.hasNumber ? b.type + std::to_string(b.number) : b.type;
}

std::string blockKind(const BlockInfo& b) {
    if (b.kind == "instance" && !b.instanceOf.empty()) return "instance of " + b.instanceOf;
    return b.kind.empty() ? "-" : b.kind;
}

std::string blockProtection(const BlockInfo& b) {
    std::string s = b.protection;
    if (!b.copyProtection.empty()) {
        const std::string to = b.copyProtection == "cpu" ? "CPU"
                               : b.copyProtection == "memory-card" ? "memory card" : b.copyProtection;
        s += (s.empty() ? "" : ", ") + ("bound to " + to);
    }
    // a data block that programs in the CPU cannot write to
    if (b.writeProtectedInDevice.stored && b.writeProtectedInDevice.on)
        s += (s.empty() ? "" : ", ") + std::string("write-protected in device");
    return s.empty() ? "-" : s;
}

// A data block or data type is not written in a programming language; the
// project repeats the block type there.
std::string textLanguage(const BlockInfo& b) {
    return b.language.empty() || b.language == b.type ? "-" : b.language;
}

// Whether the block is compiled and unchanged since: "yes", "no" (it has to
// be compiled again), "-" when the project does not say.
std::string textCompiled(const BlockInfo& b) {
    if (b.compileNeeded.empty()) return "-";
    return b.compileNeeded == "UpToDate" ? "yes" : "no";
}

// "2026-10-05T19:01:42.061Z" as "2026-10-05 19:01"; the times are UTC.
std::string shortTime(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') return iso.empty() ? "-" : iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5);
}

// What the firmware provides (system functions, system data types) and the
// data types that come with library blocks: not listed in the text report.
bool firmwareItem(const BlockInfo& b) {
    return b.type == "SFB" || b.type == "SFC" || b.type == "SDT" || (b.system && b.type == "UDT");
}

void textBlockList(std::ostream& out, const ProgramData& prog) {
    std::vector<const BlockInfo*> rows;
    size_t hidden = 0;
    for (const auto& b : prog.blockList) {
        if (firmwareItem(b)) ++hidden;
        else rows.push_back(&b);
    }
    if (rows.empty() && !hidden) return;
    out << "\nBlocks:\n";
    if (!rows.empty()) {
        size_t wPlc = 3, wAddr = 5, wName = 4, wKind = 4, wLang = 8, wProt = 10, wFolder = 6;
        for (const BlockInfo* b : rows) {
            wPlc = std::max(wPlc, width(b->plc));
            wAddr = std::max(wAddr, width(blockAddress(*b)));
            wName = std::max(wName, width(b->name));
            wKind = std::max(wKind, width(blockKind(*b)));
            wLang = std::max(wLang, width(textLanguage(*b)));
            wProt = std::max(wProt, width(blockProtection(*b)));
            wFolder = std::max(wFolder, width(b->folder));
        }
        out << "  " << pad("PLC", wPlc + 2) << pad("Block", wAddr + 2) << pad("Name", wName + 2) << pad("Kind", wKind + 2)
            << pad("Language", wLang + 2) << pad("Protection", wProt + 2) << pad("Folder", wFolder + 2)
            << pad("Compiled", 10) << pad("Modified", 18) << "Downloaded\n";
        for (const BlockInfo* b : rows)
            out << "  " << pad(dash(b->plc), wPlc + 2) << pad(blockAddress(*b), wAddr + 2) << pad(b->name, wName + 2)
                << pad(blockKind(*b), wKind + 2) << pad(textLanguage(*b), wLang + 2)
                << pad(blockProtection(*b), wProt + 2) << pad(dash(b->folder), wFolder + 2)
                << pad(textCompiled(*b), 10) << pad(shortTime(b->modified), 18) << shortTime(b->downloaded) << "\n";
        out << "  Times are UTC.\n";
    }
    if (hidden)
        out << "  " << hidden << " system function" << (hidden == 1 ? "" : "s")
            << " or data type" << (hidden == 1 ? "" : "s")
            << " in use not listed (SFB, SFC, system data types); the JSON and the block list CSV have them.\n";
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
    textBlockList(out, prog);
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
            if (!b.title.empty()) out << "  // " << oneLine(b.title);
            else if (!b.comment.empty()) out << "  // " << oneLine(b.comment);
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

std::string flagJson(const Flag& f) { return f.stored ? tf(f.on) : "null"; }
std::string flagCsv(const Flag& f) { return f.stored ? (f.on ? "yes" : "no") : ""; }

void jsonBlockList(std::ostream& out, const ProgramData& prog) {
    out << "  \"blocks\": [";
    for (size_t i = 0; i < prog.blockList.size(); ++i) {
        const BlockInfo& b = prog.blockList[i];
        out << (i ? ",\n" : "\n") << "    {\"plc\": " << qn(b.plc) << ", \"type\": " << qn(b.type) << ", \"number\": ";
        if (b.hasNumber) out << b.number;
        else out << "null";
        out << ", \"name\": " << q(b.name) << ", \"kind\": " << qn(b.kind) << ", \"instance_of\": "
            << qn(b.instanceOf) << ", \"language\": " << qn(b.language) << ", \"language_stored\": "
            << qn(b.languageStored) << ", \"protection\": " << qn(b.protection) << ", \"protection_stored\": "
            << qn(b.protectionStored) << ", \"copy_protection\": " << qn(b.copyProtection)
            << ", \"copy_protection_stored\": " << qn(b.copyProtectionStored) << ", \"copy_protection_serial\": "
            << qn(b.copyProtectionSerial) << ", \"write_protection\": " << flagJson(b.writeProtection)
            << ", \"compile_needed\": " << qn(b.compileNeeded) << ", \"folder\": " << qn(b.folder)
            << ", \"system\": " << tf(b.system) << ", \"title\": " << qn(b.title) << ", \"comment\": "
            << qn(b.comment) << ", \"author\": " << qn(b.author) << ", \"family\": " << qn(b.family)
            << ", \"user_id\": " << qn(b.userId) << ", \"version\": " << qn(b.version)
            << ", \"optimized_access\": " << (b.hasAccess ? tf(b.symbolicAccessOnly) : "null") << ", \"created\": "
            << qn(b.created) << ", \"modified\": " << qn(b.modified) << ", \"code_modified\": "
            << qn(b.codeModified) << ", \"interface_modified\": " << qn(b.interfaceModified) << ", \"compiled\": "
            << qn(b.compiled) << ", \"downloaded\": " << qn(b.downloaded) << ", \"download_history\": [";
        for (size_t n = 0; n < b.downloads.size(); ++n) out << (n ? ", " : "") << q(b.downloads[n]);
        out << "], \"load_memory\": ";
        if (b.hasLoadMemory) out << b.loadMemory;
        else out << "null";
        out << ", \"work_memory\": ";
        if (b.hasWorkMemory) out << b.workMemory;
        else out << "null";
        out << ", \"networks\": ";
        if (b.hasNetworks) out << b.networks;
        else out << "null";
        out << ", \"write_protected_in_device\": " << flagJson(b.writeProtectedInDevice)
            << ", \"only_in_load_memory\": " << flagJson(b.onlyInLoadMemory) << ", \"accessible_from_opc_ua\": "
            << flagJson(b.accessibleFromOpcUa) << ", \"accessible_from_web_server\": "
            << flagJson(b.accessibleFromWebServer) << "}";
    }
    out << (prog.blockList.empty() ? "],\n" : "\n  ],\n");
}

void jsonProgram(std::ostream& out, const ProgramData& prog) {
    jsonBlockList(out, prog);
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
        out << ", \"address\": " << qn(b.address) << ", \"title\": " << qn(b.title) << ", \"comment\": "
            << qn(b.comment) << ", \"kind\": "
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

void writeBlockListCsv(std::ostream& out, const ProgramData& prog) {
    out << "plc,type,number,name,kind,instance_of,language,protection,copy_protection,copy_protection_serial,"
           "compile_needed,folder,system,title,comment,"
           "author,family,user_id,version,optimized_access,created,modified,code_modified,interface_modified,"
           "compiled,downloaded,load_memory,work_memory,networks,write_protected_in_device,only_in_load_memory,"
           "accessible_from_opc_ua,accessible_from_web_server\r\n";
    for (const auto& b : prog.blockList)
        csvRow(out, {b.plc, b.type, b.hasNumber ? std::to_string(b.number) : "", b.name, b.kind, b.instanceOf,
                     b.language, b.protection, b.copyProtection, b.copyProtectionSerial, b.compileNeeded, b.folder,
                     b.system ? "yes" : "no", b.title,
                     b.comment, b.author, b.family, b.userId, b.version,
                     b.hasAccess ? (b.symbolicAccessOnly ? "yes" : "no") : "", b.created, b.modified, b.codeModified,
                     b.interfaceModified, b.compiled, b.downloaded,
                     b.hasLoadMemory ? std::to_string(b.loadMemory) : "",
                     b.hasWorkMemory ? std::to_string(b.workMemory) : "",
                     b.hasNetworks ? std::to_string(b.networks) : "", flagCsv(b.writeProtectedInDevice),
                     flagCsv(b.onlyInLoadMemory), flagCsv(b.accessibleFromOpcUa), flagCsv(b.accessibleFromWebServer)});
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
            if (!m.ioController.empty())
                out << "        IO device of " << m.ioController << (m.ioSystem.empty() ? "" : " (" + m.ioSystem + ")") << "\n";
            if (m.kind == "controller") textSecurity(out, m);
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
    if (!inv.ioSystems.empty()) {
        out << "\nIO systems:\n";
        for (const auto& sys : inv.ioSystems) {
            out << "  " << dash(sys.name);
            if (sys.hasNumber) out << " (" << sys.number << ")";
            out << "  controller " << dash(sys.controller);
            if (!sys.subnet.empty()) out << "  subnet " << sys.subnet;
            out << "\n";
            if (sys.devices.empty()) out << "      no devices assigned\n";
            size_t wDev = 0, wMod = 0;
            for (const auto& d : sys.devices) {
                wDev = std::max(wDev, width(d.device));
                wMod = std::max(wMod, width(d.module));
            }
            for (const auto& d : sys.devices)
                out << "      " << pad(dash(d.device), wDev + 2) << pad(dash(d.module), wMod + 2) << dash(d.ip) << "\n";
        }
    }
    if (!inv.portLinks.empty()) {
        out << "\nPort connections:\n";
        auto end = [](const PortEnd& e) {
            std::string s = e.device;
            if (!e.module.empty()) s += (s.empty() ? "" : " / ") + e.module;
            if (!e.port.empty()) s += (s.empty() ? "" : " / ") + e.port;
            return s.empty() ? std::string("-") : s;
        };
        size_t wA = 0;
        for (const auto& l : inv.portLinks) wA = std::max(wA, width(end(l.a)));
        for (const auto& l : inv.portLinks) out << "  " << pad(end(l.a), wA + 2) << "<->  " << end(l.b) << "\n";
    }
    if (!inv.connections.empty()) {
        out << "\nConnections:\n";
        auto end = [](const ConnectionEnd& e, const std::string& address) {
            std::string s = e.device;
            if (!e.module.empty() && e.module != e.device) s += (s.empty() ? "" : " / ") + e.module;
            if (!e.interface.empty()) s += " / " + e.interface;
            const std::string& ip = e.ip.empty() ? address : e.ip;
            if (!ip.empty()) s += (s.empty() ? "" : " ") + ("(" + ip + ")");
            return s.empty() ? std::string("-") : s;
        };
        auto partner = [&](const Connection& cn) {
            // nothing of the partner is in the project: only what was typed in
            if (cn.partner.device.empty() && cn.partner.module.empty())
                return cn.partnerAddress.empty() ? std::string("unspecified partner")
                                                 : "partner outside the project, " + cn.partnerAddress;
            return end(cn.partner, cn.partnerAddress);
        };
        size_t wName = 4, wKind = 4, wFrom = 4, wTo = 2;
        for (const auto& cn : inv.connections) {
            wName = std::max(wName, width(cn.name));
            wKind = std::max(wKind, width(cn.kind));
            wFrom = std::max(wFrom, width(end(cn.local, std::string())));
            wTo = std::max(wTo, width(partner(cn)));
        }
        out << "  " << pad("Name", wName + 2) << pad("Type", wKind + 2) << pad("From", wFrom + 2) << pad("To", wTo + 2)
            << "Notes\n";
        for (const auto& cn : inv.connections) {
            std::vector<std::string> notes;
            // TIA Portal shows these IDs for S7 connections, in hexadecimal.
            if (cn.kind == "S7" && cn.hasLocalId) notes.push_back("local ID " + hexId(cn.localId) + " (hex)");
            if (cn.kind == "S7" && cn.hasPartnerId) notes.push_back("partner ID " + hexId(cn.partnerId) + " (hex)");
            if (cn.overTcpIp.stored && cn.overTcpIp.on) notes.push_back("S7 over TCP/IP");
            if (cn.oneWay.stored) notes.push_back(cn.oneWay.on ? "one-way" : "two-way");
            if (cn.bothSides) notes.push_back("configured on both sides");
            out << "  " << pad(dash(cn.name), wName + 2) << pad(dash(cn.kind), wKind + 2)
                << pad(end(cn.local, std::string()), wFrom + 2) << pad(partner(cn), wTo + 2) << joined(notes) << "\n";
        }
    }
    textProgram(out, prog, ctx);
    out << "\n" << inv.stats.liveObjects << " objects in " << inv.stats.blocks << " blocks";
    if (inv.stats.deletedObjects) out << ", " << inv.stats.deletedObjects << " deleted";
    if (inv.stats.saves) out << ", " << inv.stats.saves << " save" << (inv.stats.saves == 1 ? "" : "s") << " recorded";
    if (ctx.shownSave) out << "; shown as it was after save " << ctx.shownSave;
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
        << tf(ctx.hashesVerified) << ", \"hash_errors\": " << ctx.hashErrors << ", \"save_count\": "
        << inv.stats.saves << ", \"shown_save\": ";
    if (ctx.shownSave) out << ctx.shownSave;
    else out << "null";
    out << ", \"saves\": [";
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
                << ", \"author\": " << qn(m.author) << ", \"modified\": " << qn(m.modified);
            if (!m.ioController.empty())
                out << ", \"io_controller\": " << q(m.ioController) << ", \"io_system\": " << qn(m.ioSystem);
            if (m.kind == "controller") {
                out << ", \"security\": ";
                jsonSecurity(out, m.security);
            }
            out << ", \"interfaces\": [";
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
    out << "  \"io_systems\": [";
    for (size_t si = 0; si < inv.ioSystems.size(); ++si) {
        const IoSystem& sys = inv.ioSystems[si];
        out << (si ? ",\n" : "\n") << "    {\"name\": " << qn(sys.name) << ", \"type\": " << qn(sys.kind)
            << ", \"number\": ";
        if (sys.hasNumber) out << sys.number;
        else out << "null";
        out << ", \"controller_device\": " << qn(sys.controllerDevice) << ", \"controller\": " << qn(sys.controller)
            << ", \"subnet\": " << qn(sys.subnet) << ", \"devices\": [";
        for (size_t di = 0; di < sys.devices.size(); ++di) {
            const IoDevice& d = sys.devices[di];
            out << (di ? ", " : "") << "{\"device\": " << qn(d.device) << ", \"module\": " << qn(d.module)
                << ", \"interface\": " << qn(d.interface) << ", \"ip\": " << qn(d.ip) << "}";
        }
        out << "]}";
    }
    out << (inv.ioSystems.empty() ? "],\n" : "\n  ],\n");
    out << "  \"port_links\": [";
    for (size_t li = 0; li < inv.portLinks.size(); ++li) {
        const PortLink& l = inv.portLinks[li];
        auto end = [&](const PortEnd& e) {
            out << "{\"device\": " << qn(e.device) << ", \"module\": " << qn(e.module) << ", \"port\": " << qn(e.port)
                << "}";
        };
        out << (li ? ",\n" : "\n") << "    {\"a\": ";
        end(l.a);
        out << ", \"b\": ";
        end(l.b);
        out << "}";
    }
    out << (inv.portLinks.empty() ? "],\n" : "\n  ],\n");
    out << "  \"connections\": [";
    for (size_t ci = 0; ci < inv.connections.size(); ++ci) {
        const Connection& cn = inv.connections[ci];
        auto end = [&](const ConnectionEnd& e) {
            out << "{\"device\": " << qn(e.device) << ", \"module\": " << qn(e.module) << ", \"interface\": "
                << qn(e.interface) << ", \"ip\": " << qn(e.ip) << "}";
        };
        out << (ci ? ",\n" : "\n") << "    {\"name\": " << qn(cn.name) << ", \"type\": " << qn(cn.kind)
            << ", \"local\": ";
        end(cn.local);
        out << ", \"partner\": ";
        end(cn.partner);
        out << ", \"partner_address\": " << qn(cn.partnerAddress) << ", \"local_id\": ";
        if (cn.hasLocalId) out << cn.localId << ", \"local_id_hex\": " << q(hexId(cn.localId));
        else out << "null, \"local_id_hex\": null";
        out << ", \"s7_over_tcp_ip\": " << settingJson(cn.overTcpIp) << ", \"one_way\": " << settingJson(cn.oneWay)
            << ", \"active_establishment\": " << settingJson(cn.activeEstablishment)
            << ", \"configured_on_both_sides\": " << tf(cn.bothSides) << ", \"partner_id\": ";
        if (cn.hasPartnerId) out << cn.partnerId << ", \"partner_id_hex\": " << q(hexId(cn.partnerId));
        else out << "null, \"partner_id_hex\": null";
        out << "}";
    }
    out << (inv.connections.empty() ? "],\n" : "\n  ],\n");
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
           "profinet_name_auto,subnet,access_level,access_level_name,put_get,web_server,web_server_https_only,"
           "web_server_interfaces,opc_ua_server,ntp_servers,display_protection,access_control,"
           "legacy_pg_hmi_communication,config_data_protection,io_controller,io_system,access_protection\r\n";
    auto row = [&](const Device& d, const Module& m, const Interface* i) {
        const bool ctl = m.kind == "controller";
        const Security& s = m.security;
        const std::string cols[] = {d.name, d.type, d.inProject ? "yes" : "no",
                                    m.hasPosition ? std::to_string(m.position) : "", m.kind, m.name, m.type,
                                    m.typeName, m.orderNumber, m.firmware, m.container,
                                    i ? i->name : "", i ? i->item : "", i ? i->ip : "", i ? i->mask : "",
                                    i ? i->router : "", (i && i->hasIpSettings) ? (i->ipAssignedElsewhere ? "yes" : "no") : "",
                                    (i && i->hasIpSetByUser) ? (i->ipSetByUser ? "yes" : "no") : "",
                                    (i && i->hasBusAddress) ? std::to_string(i->busAddress) : "",
                                    i ? i->profinetName : "", i ? (i->profinetNameAuto ? "yes" : "no") : "",
                                    i ? i->subnet : "",
                                    // settings of a controller, on each of its rows
                                    ctl && s.hasAccessLevel ? std::to_string(s.accessLevel) : "",
                                    ctl ? s.accessLevelName : "", ctl ? settingCsv(s.putGet) : "",
                                    ctl ? settingCsv(s.webServer) : "", ctl ? settingCsv(s.webServerHttpsOnly) : "",
                                    ctl ? joined(s.webServerInterfaces) : "", ctl ? settingCsv(s.opcUaServer) : "",
                                    ctl ? joined(s.ntpServers) : "", ctl ? settingCsv(s.displayProtection) : "",
                                    ctl ? settingCsv(s.accessControl) : "",
                                    ctl && s.hasCommunicationMode && s.communicationMode == 0 ? "yes" : "",
                                    ctl ? settingCsv(s.configDataProtection) : "", m.ioController, m.ioSystem,
                                    ctl ? accessProtection(s) : ""};
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

bool secretAttribute(const std::string& name) {
    for (const char* part : {"assword", "Salt", "ProtectionIV", "VerificationTag"})
        if (name.find(part) != std::string::npos) return true;
    return false;
}

void writeValues(std::ostream& out, const NamedValues& vals) {
    out << "{";
    bool first = true;
    for (const auto& kv : vals) {
        if (!first) out << ", ";
        first = false;
        out << q(kv.first) << ": ";
        // Whatever a project keeps in a password attribute, or next to one for
        // checking a password (salt, initialisation vector, verification tag
        // of a protected block), stays out of the output: a text is not shown,
        // and bytes only ever by their size.
        if (secretAttribute(kv.first) && kv.second.type == Value::Type::String && !kv.second.s.empty())
            out << "{\"redacted\": true}";
        else writeValue(out, kv.second);
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
