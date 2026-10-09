// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>

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

// "station / item" for what a system constant stands for.
std::string standsFor(const Constant& c) {
    std::string item;
    // names of internal items start with a blank or an underscore
    const size_t from = c.standsFor.find_first_not_of(" ");
    if (from != std::string::npos) item = c.standsFor.substr(from);
    if (c.standsForDevice.empty() || c.standsForDevice == item) return item.empty() ? "-" : item;
    return item.empty() ? c.standsForDevice : c.standsForDevice + " / " + item;
}

// Hardware identifiers and user constants. The other system constants (OB
// numbers, process image partitions) are the same in every project of a CPU
// type and are only counted.
void textConstants(std::ostream& out, const ProgramData& prog) {
    std::vector<const Constant*> hw, user;
    size_t others = 0;
    for (const auto& c : prog.constants) {
        if (c.kind == "hardware") hw.push_back(&c);
        else if (c.kind == "user") user.push_back(&c);
        else ++others;
    }
    if (!hw.empty()) {
        out << "\nHardware identifiers:\n";
        size_t wPlc = 3, wId = 2, wName = 4, wType = 4;
        for (const Constant* c : hw) {
            wPlc = std::max(wPlc, width(c->plc));
            wId = std::max(wId, width(c->value));
            wName = std::max(wName, width(c->name));
            wType = std::max(wType, width(c->dataType));
        }
        out << "  " << pad("PLC", wPlc + 2) << pad("ID", wId + 2) << pad("Name", wName + 2) << pad("Type", wType + 2)
            << "Stands for\n";
        for (const Constant* c : hw)
            out << "  " << pad(dash(c->plc), wPlc + 2) << pad(dash(c->value), wId + 2) << pad(c->name, wName + 2)
                << pad(dash(c->dataType), wType + 2) << standsFor(*c) << "\n";
    }
    if (!user.empty()) {
        out << "\nUser constants:\n";
        size_t wPlc = 3, wTable = 5, wName = 4, wType = 4, wValue = 5;
        bool anyComment = false;
        for (const Constant* c : user) {
            wPlc = std::max(wPlc, width(c->plc));
            wTable = std::max(wTable, width(c->table));
            wName = std::max(wName, width(c->name));
            wType = std::max(wType, width(c->dataType));
            wValue = std::max(wValue, width(oneLine(c->value)));
            anyComment = anyComment || !c->comment.empty();
        }
        out << "  " << pad("PLC", wPlc + 2) << pad("Table", wTable + 2) << pad("Name", wName + 2)
            << pad("Type", wType + 2) << (anyComment ? pad("Value", wValue + 2) + "Comment" : "Value") << "\n";
        for (const Constant* c : user) {
            out << "  " << pad(dash(c->plc), wPlc + 2) << pad(dash(c->table), wTable + 2) << pad(c->name, wName + 2)
                << pad(dash(c->dataType), wType + 2);
            if (c->comment.empty()) out << dash(oneLine(c->value)) << "\n";
            else out << pad(dash(oneLine(c->value)), wValue + 2) << oneLine(c->comment) << "\n";
        }
    }
    if (others && (!hw.empty() || !user.empty()))
        out << "  " << others << " other system constant" << (others == 1 ? "" : "s")
            << " not listed (OB numbers, process image partitions); the JSON and the constants CSV have them.\n";
}

// What to say in place of a PLC tag or an address that is not there.
std::string hmiTagNote(const HmiTag& t) {
    if (t.access == "symbolic" && t.plcTag.empty()) return "stands for a PLC tag whose name could not be read";
    if (t.access == "symbolic" && !t.plcTagLinked) return "link to the PLC tag broken";
    if (t.access == "symbolic" && !t.plc.empty() && !t.plcTagFound) return "PLC tag not found in the PLC's tags and data blocks";
    return std::string();
}

void textHmiTags(std::ostream& out, const ProgramData& prog) {
    if (prog.hmiTags.empty()) return;
    out << "\nHMI tags:\n";
    size_t wHmi = 3, wTable = 5, wName = 4, wType = 4, wConn = 10, wPlc = 3, wTag = 7, wAddr = 7;
    for (const auto& t : prog.hmiTags) {
        wHmi = std::max(wHmi, width(t.hmi));
        wTable = std::max(wTable, width(t.table));
        wName = std::max(wName, width(t.name));
        wType = std::max(wType, width(t.dataType));
        wConn = std::max(wConn, width(t.connection));
        wPlc = std::max(wPlc, width(t.plc));
        wTag = std::max(wTag, width(t.plcTag));
        wAddr = std::max(wAddr, width(t.address));
    }
    out << "  " << pad("HMI", wHmi + 2) << pad("Table", wTable + 2) << pad("Name", wName + 2) << pad("Type", wType + 2)
        << pad("Connection", wConn + 2) << pad("PLC", wPlc + 2) << pad("PLC tag", wTag + 2) << pad("Address", wAddr + 2)
        << "Cycle\n";
    for (const auto& t : prog.hmiTags) {
        out << "  " << pad(dash(t.hmi), wHmi + 2) << pad(dash(t.table), wTable + 2) << pad(t.name, wName + 2)
            << pad(dash(t.dataType), wType + 2) << pad(t.access == "internal" ? "(internal)" : dash(t.connection), wConn + 2)
            << pad(dash(t.plc), wPlc + 2) << pad(dash(t.plcTag), wTag + 2) << pad(dash(t.address), wAddr + 2)
            << dash(t.acquisitionCycle);
        const std::string note = hmiTagNote(t);
        if (!note.empty()) out << "  (" << note << ")";
        if (!t.comment.empty()) out << "  // " << oneLine(t.comment);
        out << "\n";
    }
    out << "  For a tag that stands for a PLC tag, the address is that of the PLC tag, where it has one\n"
           "  (a tag in I, Q or M, a member of a data block with standard access). TIA Portal shows none there.\n";
}

void textProgram(std::ostream& out, const ProgramData& prog, const ReportContext& ctx) {
    textBlockList(out, prog);
    textConstants(out, prog);
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
    textHmiTags(out, prog);
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

void jsonConstants(std::ostream& out, const ProgramData& prog) {
    out << "  \"constants\": [";
    for (size_t i = 0; i < prog.constants.size(); ++i) {
        const Constant& c = prog.constants[i];
        out << (i ? ",\n" : "\n") << "    {\"plc\": " << qn(c.plc) << ", \"kind\": " << qn(c.kind) << ", \"system\": "
            << tf(c.system) << ", \"table\": " << qn(c.table) << ", \"name\": " << q(c.name) << ", \"data_type\": "
            << qn(c.dataType) << ", \"value\": " << qn(c.value) << ", \"comment\": " << qn(c.comment)
            << ", \"stands_for\": " << qn(c.standsFor) << ", \"stands_for_device\": " << qn(c.standsForDevice) << "}";
    }
    out << (prog.constants.empty() ? "],\n" : "\n  ],\n");
}

void jsonHmiMembers(std::ostream& out, const std::vector<HmiTagMember>& members) {
    out << "[";
    for (size_t i = 0; i < members.size(); ++i) {
        out << (i ? ", " : "") << "{\"name\": " << q(members[i].name) << ", \"data_type\": " << qn(members[i].dataType)
            << ", \"members\": ";
        jsonHmiMembers(out, members[i].members);
        out << "}";
    }
    out << "]";
}

void jsonProgram(std::ostream& out, const ProgramData& prog) {
    jsonBlockList(out, prog);
    jsonConstants(out, prog);
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
    out << "  \"hmi_tags\": [";
    for (size_t i = 0; i < prog.hmiTags.size(); ++i) {
        const HmiTag& t = prog.hmiTags[i];
        out << (i ? ",\n" : "\n") << "    {\"hmi\": " << qn(t.hmi) << ", \"runtime\": " << qn(t.runtime) << ", \"table\": "
            << qn(t.table) << ", \"name\": " << q(t.name) << ", \"data_type\": " << qn(t.dataType) << ", \"access\": "
            << q(t.access) << ", \"connection\": " << qn(t.connection) << ", \"plc\": " << qn(t.plc)
            << ", \"plc_device\": " << qn(t.plcDevice) << ", \"plc_tag\": " << qn(t.plcTag) << ", \"plc_tag_linked\": ";
        if (t.access == "symbolic") out << tf(t.plcTagLinked);
        else out << "null";
        out << ", \"plc_tag_found\": ";
        if (!t.plcTag.empty() && !t.plc.empty()) out << tf(t.plcTagFound);
        else out << "null";
        out << ", \"plc_data_type\": " << qn(t.plcDataType) << ", \"address\": " << qn(t.address)
            << ", \"address_stored\": " << qn(t.addressStored) << ", \"address_mode\": " << qn(t.addressMode)
            << ", \"acquisition_cycle\": " << qn(t.acquisitionCycle) << ", \"acquisition_mode\": "
            << qn(acquisitionModeName(t.acquisitionMode)) << ", \"acquisition_mode_stored\": " << qn(t.acquisitionMode)
            << ", \"comment\": " << qn(t.comment) << ", \"start_value\": " << qn(t.startValue)
            << ", \"member_count\": " << t.memberCount << ", \"members\": ";
        jsonHmiMembers(out, t.members);
        out << "}";
    }
    out << (prog.hmiTags.empty() ? "],\n" : "\n  ],\n");
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

void writeHmiTagsCsv(std::ostream& out, const ProgramData& prog) {
    out << "hmi,table,name,data_type,access,connection,plc,plc_tag,address,acquisition_cycle,acquisition_mode,comment,"
           "start_value,members,plc_tag_found,plc_data_type,address_stored\r\n";
    for (const auto& t : prog.hmiTags)
        csvRow(out, {t.hmi, t.table, t.name, t.dataType, t.access, t.connection, t.plc, t.plcTag, t.address,
                     t.acquisitionCycle, acquisitionModeName(t.acquisitionMode), t.comment, t.startValue,
                     t.memberCount ? std::to_string(t.memberCount) : std::string(),
                     !t.plcTag.empty() && !t.plc.empty() ? (t.plcTagFound ? "yes" : "no") : "", t.plcDataType,
                     t.addressStored});
}

void writeConstantsCsv(std::ostream& out, const ProgramData& prog) {
    out << "plc,kind,table,name,data_type,value,comment,stands_for,stands_for_device\r\n";
    for (const auto& c : prog.constants)
        csvRow(out, {c.plc, c.kind, c.table, c.name, c.dataType, c.value, c.comment, c.standsFor, c.standsForDevice});
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

namespace {

std::string unreadReason(const BlockCode& b) {
    if (b.protectedLater) return "know-how protected in a later save";
    return b.protection.empty() ? std::string("protected") : b.protection + " protected";
}

// What the cross-reference lists: not the entries the project keeps for its
// own bookkeeping, and not plain numbers.
bool listedReference(const CodeReference& r) {
    return r.kind != "call interface" && r.kind != "expression" && r.kind != "constant" && !r.text.empty();
}

}  // namespace

void writeCrossReferenceCsv(std::ostream& out, const CodeData& code) {
    out << "plc,block,block_name,network,network_title,access,kind,item,data_type,data_block\r\n";
    for (const BlockCode& b : code.blocks) {
        const std::string number = b.hasNumber ? b.type + std::to_string(b.number) : b.type;
        if (b.isProtected) {
            csvRow(out, {b.plc, number, b.name, "", "", "", "not read", unreadReason(b), "", ""});
            continue;
        }
        struct Row {
            size_t network;
            std::string access, kind, item, dataType, container;
        };
        std::vector<Row> rows;
        for (const CodeReference& r : b.references) {
            if (!listedReference(r)) continue;
            for (const CodeUse& u : r.uses) {
                if (u.hidden) continue;
                Row row{u.network, u.access, r.kind, r.text, r.dataType, r.container};
                // one row per network and kind of access, however often
                bool seen = false;
                for (const Row& x : rows)
                    if (x.network == row.network && x.access == row.access && x.kind == row.kind && x.item == row.item)
                        seen = true;
                if (!seen) rows.push_back(std::move(row));
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& c) { return a.network < c.network; });
        for (const Row& r : rows) {
            std::string title;
            if (r.network >= 1 && r.network <= b.networks.size()) title = b.networks[r.network - 1].title;
            csvRow(out, {b.plc, number, b.name, r.network ? std::to_string(r.network) : "", title, r.access, r.kind,
                         r.item, r.dataType, r.container});
        }
    }
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

namespace {

// ---- save history ----

std::string plural(const std::string& kind, size_t n) {
    return std::to_string(n) + " " + kind + (n == 1 ? "" : "s");
}

std::string orNotSet(const std::string& s) { return s.empty() ? "(not set)" : s; }

void jsonEvents(std::ostream& out, const std::vector<ProjectEvent>& events) {
    out << "[";
    for (size_t i = 0; i < events.size(); ++i) {
        const ProjectEvent& e = events[i];
        out << (i ? ", " : "") << "{\"date\": " << qn(e.date) << ", \"event\": " << qn(e.event) << ", \"text\": "
            << qn(projectEventText(e)) << ", \"version\": " << qn(e.version) << ", \"old_version\": "
            << qn(e.oldVersion) << ", \"log_file\": " << qn(e.logFile) << "}";
    }
    out << "]";
}

std::string contentsText(const HistorySave& s) {
    std::string out;
    for (const auto& kv : s.contents) out += (out.empty() ? "" : ", ") + plural(kv.first, kv.second);
    return out.empty() ? "an empty project" : out;
}

std::string writtenTypes(const HistorySave& s, size_t limit) {
    std::string out;
    for (size_t i = 0; i < s.objectTypes.size() && i < limit; ++i)
        out += (out.empty() ? "" : ", ") + std::to_string(s.objectTypes[i].written) + " " + s.objectTypes[i].type;
    if (s.objectTypes.size() > limit) out += ", ...";
    return out;
}

// The changes of one save as lines of text. Parts of something that was
// added or removed as a whole are counted under it; what changed in one item
// goes on one line; many items with the same change share a line.
// Lines of code below a change, the first so many.
void textCodeLines(std::ostream& out, const std::string& text, const std::string& in) {
    if (text.empty()) return;
    const size_t kMax = 20;
    size_t from = 0, n = 0, total = 1;
    for (char c : text)
        if (c == '\n') ++total;
    while (from <= text.size() && n < kMax) {
        size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        out << in << text.substr(from, nl - from) << "\n";
        from = nl + 1;
        ++n;
    }
    if (total > n) out << in << "... " << plural("more line", total - n) << "\n";
}

void textChanges(std::ostream& out, const HistorySave& s, const char* in) {
    const size_t kMaxLines = 12;  // per kind of change, the rest is counted
    const auto& ch = s.changes;

    // added / removed
    for (const char* what : {"added", "removed"}) {
        const char sign = what[0] == 'a' ? '+' : '-';
        std::map<std::string, size_t> byKey;
        for (size_t i = 0; i < ch.size(); ++i)
            if (ch[i].change == what) byKey[ch[i].key] = i;
        // the outermost item of this change that a part belongs to
        auto root = [&](size_t i) {
            for (int depth = 0; depth < 16 && !ch[i].partOf.empty(); ++depth) {
                auto p = byKey.find(ch[i].parentKey);
                if (p == byKey.end()) break;
                i = p->second;
            }
            return i;
        };
        std::map<std::string, size_t> lines;  // kind -> lines printed
        std::map<std::string, size_t> more;   // kind -> items not printed
        std::vector<std::string> moreOrder;
        for (size_t i = 0; i < ch.size(); ++i) {
            const HistoryChange& c = ch[i];
            if (c.change != what || !c.partOf.empty()) continue;
            if (lines[c.kind] >= kMaxLines) {
                if (!more[c.kind]++) moreOrder.push_back(c.kind);
                continue;
            }
            ++lines[c.kind];
            out << in << sign << " " << c.kind << " " << c.item;
            if (!c.description.empty()) out << "  (" << c.description << ")";
            out << "\n";
            if (c.kind == "network") textCodeLines(out, sign == '+' ? c.to : c.from, std::string(in) + "      ");
            // its parts: modules and interfaces by name, the rest counted
            std::vector<std::string> order;
            std::map<std::string, size_t> counts;
            for (size_t k = 0; k < ch.size(); ++k) {
                const HistoryChange& part = ch[k];
                if (k == i || part.change != what || part.partOf.empty() || root(k) != i) continue;
                if (part.kind == "module" || part.kind == "interface") {
                    out << in << "    " << part.kind << " " << part.item;
                    if (!part.description.empty()) out << "  (" << part.description << ")";
                    out << "\n";
                } else {
                    if (!counts.count(part.kind)) order.push_back(part.kind);
                    ++counts[part.kind];
                }
            }
            if (!order.empty()) {
                out << in << "    with ";
                for (size_t k = 0; k < order.size(); ++k) out << (k ? ", " : "") << plural(order[k], counts[order[k]]);
                out << "\n";
            }
        }
        for (const auto& kind : moreOrder)
            out << in << sign << " ... and " << plural("more " + kind, more[kind]) << " " << what << "\n";
    }

    // changed: one text per item
    std::vector<std::string> keys;
    std::map<std::string, std::vector<size_t>> perItem;
    for (size_t i = 0; i < ch.size(); ++i) {
        if (ch[i].change != "changed") continue;
        if (!perItem.count(ch[i].key)) keys.push_back(ch[i].key);
        perItem[ch[i].key].push_back(i);
    }
    struct Line {
        std::string kind, text;
        std::vector<std::string> items;
        size_t code = static_cast<size_t>(-1);  // the change that holds the lines of code
    };
    std::vector<size_t> codeChanges;
    std::vector<Line> linesOut;
    std::map<std::pair<std::string, std::string>, size_t> lineIndex;
    for (const auto& k : keys) {
        const auto& idx = perItem[k];
        std::string text;
        // "modified" is said only when nothing else explains it; memory
        // sizes and the compile state only when the block was not compiled
        // in this save, which explains them.
        bool other = false, compiled = false;
        for (size_t i : idx) {
            if (!ch[i].isMarker) other = true;
            if (ch[i].attribute == "compiled") compiled = true;
        }
        for (size_t i : idx) {
            const HistoryChange& c = ch[i];
            std::string part;
            if (c.isMarker) {
                if (other) continue;
                part = "modified, in something that is not reported";
            } else if (c.isTime) {
                part = c.attribute;
            } else if (c.attribute == "needs compiling") {
                if (c.to.empty() && compiled) continue;
                part = c.to.empty() ? "no longer needs compiling" : "needs compiling";
            } else if (c.isResult && (compiled || c.to.empty())) {
                continue;  // a block that needs compiling has no sizes
            } else if (c.attribute == "code") {
                // printed below the line, as lines taken out and put in
                part = "code";
                codeChanges.push_back(i);
            } else {
                part = c.attribute + ": " + orNotSet(c.from) + " -> " + orNotSet(c.to);
            }
            text += (text.empty() ? "" : "; ") + part;
        }
        if (text.empty()) text = "compiled";
        if (!codeChanges.empty()) {
            // code is told per network, never summed up over several
            const HistoryChange& first = ch[idx.front()];
            linesOut.push_back({first.kind, text, {first.item}, codeChanges.back()});
            codeChanges.clear();
            continue;
        }
        const HistoryChange& first = ch[idx.front()];
        auto id = std::make_pair(first.kind, text);
        auto it = lineIndex.find(id);
        if (it == lineIndex.end()) {
            lineIndex[id] = linesOut.size();
            linesOut.push_back({first.kind, text, {first.item}});
        } else {
            linesOut[it->second].items.push_back(first.item);
        }
    }
    for (const auto& l : linesOut) {
        if (l.items.size() < 4) {
            for (const auto& item : l.items) out << in << "~ " << l.kind << " " << item << ": " << l.text << "\n";
            if (l.code != static_cast<size_t>(-1)) {
                textCodeLines(out, ch[l.code].from, std::string(in) + "      - ");
                textCodeLines(out, ch[l.code].to, std::string(in) + "      + ");
            }
        } else {
            out << in << "~ " << plural(l.kind, l.items.size()) << ": " << l.text << "\n" << in << "    ";
            for (size_t i = 0; i < l.items.size() && i < 4; ++i) out << (i ? ", " : "") << l.items[i];
            if (l.items.size() > 4) out << ", and " << l.items.size() - 4 << " more";
            out << "\n";
        }
    }
}

void textHistory(std::ostream& out, const History& h) {
    out << "\nSave history:\n";
    if (!h.events.empty()) {
        out << "  Recorded by TIA Portal:\n";
        for (const auto& e : h.events) {
            const std::string text = projectEventText(e);
            out << "    " << pad(dash(e.date), 26);
            if (text.empty()) {
                out << e.event;
                if (!e.version.empty()) out << ", version " << e.version;
                if (!e.oldVersion.empty()) out << ", from " << e.oldVersion;
            } else {
                out << text;
            }
            out << "\n";
        }
    }
    bool anyProjectTime = false;
    for (const auto& s : h.saves) {
        if (s.afterLastSave) out << "  After save " << s.number - 1 << "  ";
        else out << "  Save " << s.number << "  ";
        if (!s.problem.empty()) {
            out << "could not be read: " << s.problem << "\n";
            continue;
        }
        if (!s.time.empty()) out << s.time;
        else out << "no time";
        if (s.timeSource == "latest_change") anyProjectTime = true;
        if (!s.by.empty()) out << "  by " << s.by;
        out << "  -  " << plural("object", s.objectsWritten) << " written";
        if (s.objectsDeleted) out << ", " << s.objectsDeleted << " of them as deleted";
        out << "\n";
        const char* in = "      ";
        if (s.firstState) {
            out << in << "First state of the project in the file: " << contentsText(s) << "\n";
        } else if (s.beforeProject) {
            out << in << "No project in the file yet\n";
        } else if (s.changes.empty()) {
            out << in << "No change in what is reported";
            if (s.objectsWritten) out << "; written: " << writtenTypes(s, 6);
            out << "\n";
        } else {
            textChanges(out, s, in);
        }
    }
    out << "  Times are UTC.";
    if (anyProjectTime)
        out << " A save in this file layout carries no time of its own: shown is the latest\n"
               "  change it holds, so the save was made then or shortly after.";
    out << "\n";
    for (const auto& n : h.notes) out << "  Note: " << n << "\n";
}

void jsonHistory(std::ostream& out, const History& h) {
    out << "  \"history\": {\"saves_in_file\": " << h.savesInFile << ", \"events\": ";
    jsonEvents(out, h.events);
    out << ", \"saves\": [";
    for (size_t si = 0; si < h.saves.size(); ++si) {
        const HistorySave& s = h.saves[si];
        out << (si ? ",\n" : "\n") << "    {\"save\": ";
        if (s.afterLastSave) out << "null";
        else out << s.number;
        out << ", \"after_last_save\": " << tf(s.afterLastSave) << ", \"time\": " << qn(s.time)
            << ", \"time_source\": " << qn(s.timeSource) << ", \"by\": " << qn(s.by) << ", \"before_project\": "
            << tf(s.beforeProject) << ", \"first_state\": " << tf(s.firstState) << ", \"problem\": " << qn(s.problem)
            << ", \"objects_written\": " << s.objectsWritten << ", \"objects_deleted\": " << s.objectsDeleted
            << ", \"object_types\": [";
        for (size_t i = 0; i < s.objectTypes.size(); ++i)
            out << (i ? ", " : "") << "{\"type\": " << q(s.objectTypes[i].type) << ", \"written\": "
                << s.objectTypes[i].written << ", \"deleted\": " << s.objectTypes[i].deleted << "}";
        out << "], \"contents\": {";
        for (size_t i = 0; i < s.contents.size(); ++i)
            out << (i ? ", " : "") << q(s.contents[i].first) << ": " << s.contents[i].second;
        out << "}, \"changes\": [";
        for (size_t i = 0; i < s.changes.size(); ++i) {
            const HistoryChange& c = s.changes[i];
            out << (i ? ",\n" : "\n") << "      {\"change\": " << q(c.change) << ", \"kind\": " << q(c.kind)
                << ", \"item\": " << q(c.item);
            if (c.change == "changed")
                out << ", \"attribute\": " << q(c.attribute) << ", \"from\": " << qn(c.from) << ", \"to\": "
                    << qn(c.to);
            else
                out << ", \"description\": " << qn(c.description) << ", \"part_of\": " << qn(c.partOf);
            if (c.kind == "network" && c.change != "changed")
                out << ", \"code\": " << qn(c.change == "added" ? c.to : c.from);
            out << "}";
        }
        out << (s.changes.empty() ? "]}" : "\n    ]}");
    }
    out << (h.saves.empty() ? "]" : "\n  ]") << ", \"notes\": [";
    for (size_t i = 0; i < h.notes.size(); ++i) out << (i ? ", " : "") << q(h.notes[i]);
    out << "]},\n";
}


// ---- block code ----

std::string blockLabel(const BlockCode& b) {
    std::string out = b.plc.empty() ? b.name : b.plc + " / " + b.name;
    if (b.hasNumber) out += " [" + b.type + std::to_string(b.number) + "]";
    else out += " [" + b.type + "]";
    return out;
}

void textCode(std::ostream& out, const CodeData& code) {
    out << "\nBlock code:\n";
    if (code.blocks.empty()) {
        out << "  (no code blocks)\n";
        return;
    }
    for (const BlockCode& b : code.blocks) {
        out << "  " << blockLabel(b);
        if (!b.language.empty()) out << "  " << b.language;
        if (b.isProtected) {
            out << ": " << unreadReason(b) << ", not read\n";
            continue;
        }
        out << ", " << b.networks.size() << " network" << (b.networks.size() == 1 ? "" : "s") << "\n";
        for (const auto& n : b.notes) out << "    (" << n << ")\n";
        // a block written as text is one network without a title
        const bool plain = b.networks.size() == 1 && b.networks[0].title.empty() && b.networks[0].comment.empty() &&
                           b.networks[0].content == "scl";
        for (const Network& n : b.networks) {
            if (!plain) {
                out << "    Network " << n.number;
                if (!n.title.empty()) out << ": " << oneLine(n.title);
                if (!n.language.empty() && n.language != b.language) out << "  (" << n.language << ")";
                if (n.content == "empty") out << "  (empty)";
                out << "\n";
                if (!n.comment.empty()) out << "      // " << oneLine(n.comment) << "\n";
            }
            for (const auto& note : n.notes) out << "      (" << note << ")\n";
            for (const auto& l : n.lines) out << "      " << l << "\n";
        }
    }
    // who calls whom
    bool any = false;
    for (const BlockCode& b : code.blocks) {
        std::vector<std::string> calls;
        for (const CodeReference& r : b.references) {
            if (r.kind != "block") continue;
            std::vector<size_t> where;
            for (const CodeUse& u : r.uses)
                if (u.access == "call" && !u.hidden && std::find(where.begin(), where.end(), u.network) == where.end())
                    where.push_back(u.network);
            if (where.empty()) continue;
            std::sort(where.begin(), where.end());
            std::string s = r.text + " (network";
            if (where.size() > 1) s += "s";
            for (size_t i = 0; i < where.size(); ++i) s += (i ? ", " : " ") + std::to_string(where[i]);
            calls.push_back(s + ")");
        }
        if (calls.empty()) continue;
        if (!any) out << "\nCalls:\n";
        any = true;
        out << "  " << blockLabel(b) << " calls " << joined(calls) << "\n";
    }
}

void jsonStrings(std::ostream& out, const std::vector<std::string>& v) {
    out << "[";
    for (size_t i = 0; i < v.size(); ++i) out << (i ? ", " : "") << q(v[i]);
    out << "]";
}

void jsonCode(std::ostream& out, const CodeData& code) {
    out << "  \"code\": [";
    for (size_t bi = 0; bi < code.blocks.size(); ++bi) {
        const BlockCode& b = code.blocks[bi];
        out << (bi ? ",\n" : "\n") << "    {\"plc\": " << qn(b.plc) << ", \"type\": " << q(b.type) << ", \"number\": ";
        if (b.hasNumber) out << b.number;
        else out << "null";
        out << ", \"name\": " << q(b.name) << ", \"language\": " << qn(b.language) << ", \"protected\": "
            << tf(b.isProtected) << ", \"protection\": " << qn(b.protection) << ", \"protected_later\": "
            << tf(b.protectedLater) << ", \"notes\": ";
        jsonStrings(out, b.notes);
        out << ", \"networks\": [";
        for (size_t ni = 0; ni < b.networks.size(); ++ni) {
            const Network& n = b.networks[ni];
            out << (ni ? ",\n" : "\n") << "      {\"number\": " << n.number << ", \"network_id\": " << n.networkId
                << ", \"title\": " << qn(n.title) << ", \"comment\": " << qn(n.comment) << ", \"language\": "
                << qn(n.language) << ", \"language_stored\": " << qn(n.languageStored) << ", \"content\": "
                << q(n.content) << ", \"notes\": ";
            jsonStrings(out, n.notes);
            out << ", \"lines\": ";
            jsonStrings(out, n.lines);
            out << ", \"elements\": [";
            for (size_t ei = 0; ei < n.elements.size(); ++ei) {
                const NetworkElement& e = n.elements[ei];
                out << (ei ? ", " : "") << "{\"uid\": " << e.uid << ", \"kind\": " << q(e.kind) << ", \"name\": "
                    << q(e.name) << ", \"instance\": " << qn(e.instance) << ", \"options\": [";
                for (size_t oi = 0; oi < e.options.size(); ++oi)
                    out << (oi ? ", " : "") << "{\"name\": " << q(e.options[oi].first) << ", \"value\": "
                        << q(e.options[oi].second) << "}";
                out << "], \"pins\": [";
                for (size_t pi = 0; pi < e.pins.size(); ++pi) {
                    out << (pi ? ", " : "") << "{\"name\": " << q(e.pins[pi].name) << ", \"direction\": "
                        << (e.pins[pi].output ? "\"out\"" : "\"in\"") << ", \"connected\": ";
                    jsonStrings(out, e.pins[pi].connected);
                    out << "}";
                }
                out << "]}";
            }
            out << "]}";
        }
        out << (b.networks.empty() ? "]" : "\n    ]") << ", \"references\": [";
        bool first = true;
        for (const CodeReference& r : b.references) {
            if (r.kind == "call interface" || r.kind == "expression") continue;
            out << (first ? "\n" : ",\n") << "      {\"kind\": " << q(r.kind) << ", \"kind_stored\": " << q(r.kindStored)
                << ", \"text\": " << q(r.text) << ", \"data_type\": " << qn(r.dataType) << ", \"data_block\": "
                << qn(r.container) << ", \"uses\": [";
            first = false;
            for (size_t ui = 0; ui < r.uses.size(); ++ui) {
                const CodeUse& u = r.uses[ui];
                out << (ui ? ", " : "") << "{\"network\": ";
                if (u.network) out << u.network;
                else out << "null";
                out << ", \"access\": " << qn(u.access) << ", \"access_stored\": " << qn(u.accessStored)
                    << ", \"uid\": " << u.uid << ", \"hidden\": " << tf(u.hidden) << "}";
            }
            out << "]}";
        }
        out << (first ? "]}" : "\n    ]}");
    }
    out << (code.blocks.empty() ? "],\n" : "\n  ],\n");
}

}  // namespace

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
    if (ctx.code) textCode(out, *ctx.code);
    if (ctx.history) textHistory(out, *ctx.history);
    out << "\n" << inv.stats.liveObjects << " objects in " << inv.stats.blocks << " blocks";
    if (inv.stats.deletedObjects) out << ", " << inv.stats.deletedObjects << " deleted";
    if (inv.stats.saves) out << ", " << inv.stats.saves << " save" << (inv.stats.saves == 1 ? "" : "s") << " recorded";
    if (inv.stats.saves && inv.stats.objectsAfterLastSave && !ctx.shownSave)
        out << " and " << inv.stats.objectsAfterLastSave << " object" << (inv.stats.objectsAfterLastSave == 1 ? "" : "s")
            << " written after the last";
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
        << inv.stats.saves << ", \"objects_after_last_save\": " << inv.stats.objectsAfterLastSave
        << ", \"shown_save\": ";
    if (ctx.shownSave) out << ctx.shownSave;
    else out << "null";
    out << ", \"saves\": [";
    for (size_t i = 0; i < inv.saves.size(); ++i) out << (i ? ", " : "") << q(inv.saves[i]);
    out << "]},\n";
    if (inv.project.found) {
        out << "  \"project\": {\"name\": " << q(inv.project.name) << ", \"created\": " << qn(inv.project.created)
            << ", \"modified\": " << qn(inv.project.modified) << ", \"author\": " << qn(inv.project.author)
            << ", \"last_modified_by\": " << qn(inv.project.lastModifiedBy) << ", \"events\": ";
        jsonEvents(out, inv.events);
        out << "},\n";
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
    if (ctx.code) jsonCode(out, *ctx.code);
    if (ctx.history) jsonHistory(out, *ctx.history);
    out << "  \"stats\": {\"decoded_hardware_objects\": " << inv.stats.decodedObjects
        << ", \"objects_with_problems\": " << inv.stats.objectsWithProblems << ", \"unattached_items\": "
        << inv.stats.unattachedItems << "},\n";
    out << "  \"warnings\": [";
    for (size_t i = 0; i < inv.warnings.size(); ++i) out << (i ? ", " : "") << q(inv.warnings[i]);
    for (size_t i = 0; i < prog.warnings.size(); ++i)
        out << ((i || !inv.warnings.empty()) ? ", " : "") << q(prog.warnings[i]);
    out << "]\n}\n";
}

// ---- tiaconv diff ----

namespace {

std::string sideText(const DiffSide& s) {
    std::string out = s.project.empty() ? std::string("(no project in the file)") : "project " + s.project;
    if (s.shownSave) out += ", as after save " + std::to_string(s.shownSave) + " of " + std::to_string(s.saves);
    else if (s.saves) out += ", " + std::to_string(s.saves) + " save" + (s.saves == 1 ? "" : "s") + " recorded";
    if (!s.modified.empty()) out += "; modified " + s.modified + (s.by.empty() ? "" : " by " + s.by);
    return out;
}

void jsonSide(std::ostream& out, const DiffSide& s) {
    out << "{\"source\": " << q(s.source) << ", \"project\": " << qn(s.project) << ", \"modified\": " << qn(s.modified)
        << ", \"modified_by\": " << qn(s.by) << ", \"saves\": " << s.saves << ", \"shown_save\": ";
    if (s.shownSave) out << s.shownSave;
    else out << "null";
    out << "}";
}

}  // namespace

void writeDiffText(std::ostream& out, const ProjectDiff& d, const DiffSide& oldSide, const DiffSide& newSide) {
    out << "Old: " << oldSide.source << "\n     " << sideText(oldSide) << "\n";
    out << "New: " << newSide.source << "\n     " << sideText(newSide) << "\n";
    if (!d.sameLineage)
        out << "\nThe two do not share the identities of their objects (one was not saved from the other, or the\n"
               "project was made anew): items are paired by kind and name, "
            << d.matchedByName << " of them.\n";
    size_t substantial = 0;
    for (const auto& c : d.changes)
        if (substantialChange(c)) ++substantial;
    if (d.changes.empty()) {
        out << "\nNo differences in what tiaconv reports.\n";
    } else {
        out << "\nDifferences:\n";
        HistorySave s;
        s.changes = d.changes;
        textChanges(out, s, "  ");
        if (!substantial) out << "  Only time stamps and compiling differ.\n";
    }
    if (d.unreadBlocks)
        out << "\nThe code of " << plural("block", d.unreadBlocks)
            << " is not compared: know-how protected in one of the two, or both.\n";
    out << "Times are UTC.\n";
}

void writeDiffJson(std::ostream& out, const ProjectDiff& d, const DiffSide& oldSide, const DiffSide& newSide,
                   const std::string& toolVersion) {
    out << "{\n  \"tool\": {\"name\": \"tiaconv\", \"version\": " << q(toolVersion) << "},\n  \"old\": ";
    jsonSide(out, oldSide);
    out << ",\n  \"new\": ";
    jsonSide(out, newSide);
    size_t substantial = 0;
    for (const auto& c : d.changes)
        if (substantialChange(c)) ++substantial;
    out << ",\n  \"same_lineage\": " << tf(d.sameLineage) << ", \"matched_by_name\": " << d.matchedByName
        << ", \"blocks_not_compared\": " << d.unreadBlocks << ", \"substantial_changes\": " << substantial
        << ",\n  \"changes\": [";
    for (size_t i = 0; i < d.changes.size(); ++i) {
        const HistoryChange& c = d.changes[i];
        out << (i ? ",\n" : "\n") << "    {\"change\": " << q(c.change) << ", \"kind\": " << q(c.kind) << ", \"item\": "
            << q(c.item);
        if (c.change == "changed")
            out << ", \"attribute\": " << q(c.attribute) << ", \"from\": " << qn(c.from) << ", \"to\": " << qn(c.to)
                << ", \"time_stamp\": " << tf(c.isTime || c.isMarker) << ", \"follows_from_compiling\": " << tf(c.isResult);
        else
            out << ", \"description\": " << qn(c.description) << ", \"part_of\": " << qn(c.partOf);
        if (c.kind == "network" && c.change != "changed") out << ", \"code\": " << qn(c.change == "added" ? c.to : c.from);
        out << "}";
    }
    out << (d.changes.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

void writeDiffCsv(std::ostream& out, const ProjectDiff& d) {
    csvRow(out, {"change", "kind", "item", "part_of", "attribute", "from", "to", "description", "time_stamp"});
    for (const auto& c : d.changes)
        csvRow(out, {c.change, c.kind, c.item, c.partOf, c.attribute, c.from, c.to, c.description,
                     c.change == "changed" && !substantialChange(c) ? "yes" : ""});
}

void writeHistoryCsv(std::ostream& out, const History& h) {
    csvRow(out, {"save", "time", "by", "objects_written", "objects_deleted", "change", "kind", "item", "part_of",
                 "attribute", "from", "to", "description"});
    for (const auto& s : h.saves) {
        const std::string n = s.afterLastSave ? "after " + std::to_string(s.number - 1) : std::to_string(s.number),
                          w = std::to_string(s.objectsWritten),
                          d = std::to_string(s.objectsDeleted);
        auto row = [&](const std::string& change, const HistoryChange* c, const std::string& description) {
            csvRow(out, {n, s.time, s.by, w, d, change, c ? c->kind : "", c ? c->item : "", c ? c->partOf : "",
                         c ? c->attribute : "", c ? c->from : "", c ? c->to : "", description});
        };
        if (!s.problem.empty()) row("not_read", nullptr, s.problem);
        else if (s.firstState) row("first_state", nullptr, contentsText(s));
        else if (s.beforeProject) row("no_project", nullptr, "");
        else if (s.changes.empty()) row("none", nullptr, writtenTypes(s, 1000));
        for (const auto& c : s.changes) row(c.change, &c, c.description);
    }
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

bool secretAttribute(const std::string& name) {
    for (const char* part : {"assword", "Salt", "ProtectionIV", "VerificationTag"})
        if (name.find(part) != std::string::npos) return true;
    return false;
}

// A structure that names what it holds in one of its own fields, as the
// settings of an HMI connection do: {"Name": "Password", "Value": "..."}.
bool namesSecret(const Value& record) {
    for (const Value& f : record.elements)
        if (f.type == Value::Type::String && secretAttribute(f.s)) return true;
    return false;
}

// `secret`: the value belongs to something that is, or goes with, a password.
// Texts in it are not written, at any depth; bytes are only ever written by
// their size.
void writeValue(std::ostream& out, const Value& v, bool secret = false) {
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
            if (secret && !v.s.empty()) out << "{\"redacted\": true}";
            else out << q(v.s);
            break;
        case Value::Type::DateTime: out << q(v.s); break;
        case Value::Type::Bytes: out << "{\"bytes\": " << v.s.size() << "}"; break;
        case Value::Type::Text: {
            if (secret) {
                out << "{\"redacted\": true}";
                break;
            }
            // one entry per language id; "-" for the entry without a language
            out << "{\"text\": {";
            for (size_t i = 0; i < v.texts.size(); ++i)
                out << (i ? ", " : "") << q(v.texts[i].first == 0xffff ? "-" : std::to_string(v.texts[i].first)) << ": "
                    << q(v.texts[i].second);
            out << "}}";
            break;
        }
        case Value::Type::Opaque: out << "{\"opaque\": true}"; break;
        case Value::Type::Record: {
            // the field that does the naming is itself written
            const bool named = !secret && namesSecret(v);
            out << "{";
            for (size_t i = 0; i < v.elements.size() && i < v.names.size(); ++i) {
                out << (i ? ", " : "") << q(v.names[i]) << ": ";
                const Value& f = v.elements[i];
                const bool naming = named && f.type == Value::Type::String && secretAttribute(f.s);
                writeValue(out, f, secret || secretAttribute(v.names[i]) || (named && !naming));
            }
            out << "}";
            break;
        }
        case Value::Type::List:
            out << "[";
            for (size_t i = 0; i < v.elements.size(); ++i) {
                out << (i ? ", " : "");
                writeValue(out, v.elements[i], secret);
            }
            out << "]";
            break;
    }
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
        writeValue(out, kv.second, secretAttribute(kv.first));
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
