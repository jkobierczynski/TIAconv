// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "history.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "code.hpp"
#include "container.hpp"
#include "program.hpp"
#include "project.hpp"

namespace tia {

namespace {

constexpr unsigned kRef = 1;     // the value names another item of the project
constexpr unsigned kTime = 2;    // a time stamp TIA Portal keeps
constexpr unsigned kMarker = 4;  // says only that something changed
constexpr unsigned kResult = 8;  // follows from compiling the block

struct Attr {
    std::string name, value;
    unsigned flags = 0;
};

// One reported thing as it is after a save, flattened to named values.
struct Entity {
    std::string key, parent, kind, label, description;
    std::vector<Attr> attrs;
    // a network: its code as text; a block: its code is not read
    std::vector<std::string> code;
    bool codeUnread = false;
};

struct Snapshot {
    bool hasProject = false;
    std::string created, modified, by;
    std::vector<Entity> list;
    std::map<std::string, size_t> index;
    std::vector<ProjectEvent> events;

    void add(Entity e) {
        if (index.count(e.key)) return;  // an id seen twice: keep the first
        index[e.key] = list.size();
        list.push_back(std::move(e));
    }
};

std::string yesNo(bool b) { return b ? "yes" : "no"; }
std::string setting(const Setting& s) { return s.stored ? yesNo(s.on) : std::string(); }
std::string flag(const Flag& f) { return f.stored ? yesNo(f.on) : std::string(); }

std::string path(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += " / ";
        out += p;
    }
    return out;
}

std::string commaList(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
    return out;
}

std::string key(const char* prefix, uint64_t id) { return std::string(prefix) + ":" + std::to_string(id); }

void securityAttrs(const Module& m, std::vector<Attr>& a) {
    const Security& s = m.security;
    a.push_back({"access protection", accessProtection(s), 0});
    a.push_back({"access level",
                 s.hasAccessLevel ? (s.accessLevelName.empty() ? "level " + std::to_string(s.accessLevel) : s.accessLevelName)
                                  : std::string(),
                 0});
    a.push_back({"access control", setting(s.accessControl), 0});
    a.push_back({"access control via access levels", setting(s.accessControlViaAccessLevels), 0});
    a.push_back({"PUT/GET access", setting(s.putGet), 0});
    a.push_back({"web server", setting(s.webServer), 0});
    a.push_back({"web server HTTPS only", setting(s.webServerHttpsOnly), 0});
    a.push_back({"web server access on", commaList(s.webServerInterfaces), 0});
    a.push_back({"OPC UA server", setting(s.opcUaServer), 0});
    a.push_back({"display protection", setting(s.displayProtection), 0});
    a.push_back({"protection of confidential configuration data", setting(s.configDataProtection), 0});
    a.push_back({"PG/PC and HMI communication",
                 !s.hasCommunicationMode ? std::string()
                 : s.communicationMode == 0 ? std::string("legacy communication permitted")
                                            : "mode " + std::to_string(s.communicationMode),
                 0});
    a.push_back({"time synchronisation",
                 !s.hasTimeSyncRole ? std::string()
                 : s.timeSyncRole == 2 ? std::string("NTP") : "mode " + std::to_string(s.timeSyncRole),
                 0});
    a.push_back({"NTP servers", commaList(s.ntpServers), 0});
}

std::string endText(const ConnectionEnd& e) { return path({e.device, e.module, e.interface}); }

void addMembers(Snapshot& snap, const std::string& blockKey, const std::string& blockLabel,
                const std::vector<BlockMember>& members, const std::string& prefix, int depth) {
    if (depth > 32) return;
    for (const auto& m : members) {
        const std::string name = prefix.empty() ? m.name : prefix + "." + m.name;
        Entity e;
        // Members have no identity of their own in what tiaconv reads: a
        // renamed member is a removed and an added one.
        e.key = "mem:" + blockKey + ":" + m.section + ":" + name;  // see rekey()
        e.parent = blockKey;
        e.kind = "member";
        e.label = blockLabel + " / " + name;
        e.description = m.dataType + (m.hasStartValue ? " = " + m.startValue : std::string());
        e.attrs.push_back({"data type", m.dataType, 0});
        e.attrs.push_back({"start value", m.hasStartValue ? m.startValue : std::string(), 0});
        e.attrs.push_back({"comment", m.comment, 0});
        snap.add(std::move(e));
        addMembers(snap, blockKey, blockLabel, m.members, name, depth + 1);
    }
}

Snapshot takeSnapshot(const Project& project, const ProtectedVersions& protectedUpTo) {
    Snapshot snap;
    if (project.meta().empty()) return snap;
    const Inventory inv = buildInventory(project);
    ProgramData prog = buildProgramData(project);
    linkHmiTags(inv, prog);
    const CodeData code = buildCode(project, prog, &protectedUpTo);

    snap.hasProject = inv.project.found;
    snap.created = inv.project.created;
    snap.modified = inv.project.modified;
    snap.by = inv.project.lastModifiedBy;
    snap.events = inv.events;
    if (inv.project.found) {
        Entity e;
        e.key = "project";
        e.kind = "project";
        e.label = inv.project.name;
        e.attrs.push_back({"name", inv.project.name, 0});
        snap.add(std::move(e));
    }
    for (const auto& ev : inv.events) {
        Entity e;
        e.key = "event:" + ev.date + ":" + ev.event;
        e.kind = "project event";
        const std::string text = projectEventText(ev);
        e.label = text.empty() ? ev.event : text;
        e.description = ev.date;
        snap.add(std::move(e));
    }

    for (const auto& s : inv.subnets) {
        Entity e;
        e.key = key("net", s.id);
        e.kind = "subnet";
        e.label = s.name;
        e.attrs.push_back({"name", s.name, 0});
        snap.add(std::move(e));
    }

    std::map<std::string, std::string> deviceKeys;  // device name -> its key
    std::map<std::string, std::string> plcKeys;  // PLC name -> key of its module
    for (const auto& d : inv.devices) {
        if (!d.inProject) continue;
        Entity de;
        de.key = key("dev", d.id);
        de.kind = "device";
        de.label = d.name;
        de.description = d.type;
        de.attrs.push_back({"name", d.name, 0});
        de.attrs.push_back({"type", d.type, 0});
        const std::string deviceKey = de.key;
        deviceKeys.emplace(d.name, deviceKey);
        snap.add(std::move(de));
        for (const auto& m : d.modules) {
            Entity me;
            me.key = key("mod", m.id);
            me.parent = deviceKey;
            me.kind = "module";
            me.label = path({d.name, m.name});
            me.description = commaList([&] {
                std::vector<std::string> parts;
                for (const std::string* p : {&m.typeName, &m.orderNumber, &m.firmware})
                    if (!p->empty()) parts.push_back(*p);
                return parts;
            }());
            me.attrs.push_back({"name", m.name, 0});
            me.attrs.push_back({"type", m.typeName, 0});
            me.attrs.push_back({"order number", m.orderNumber, 0});
            me.attrs.push_back({"firmware", m.firmware, 0});
            me.attrs.push_back({"position", m.hasPosition ? std::to_string(m.position) : std::string(), 0});
            me.attrs.push_back({"plugged into", m.container, kRef});
            me.attrs.push_back({"IO controller", m.ioController, kRef});
            me.attrs.push_back({"IO system", m.ioSystem, kRef});
            if (m.kind == "controller") {
                securityAttrs(m, me.attrs);
                plcKeys.emplace(m.name, me.key);
            }
            const std::string moduleKey = me.key;
            snap.add(std::move(me));
            for (const auto& i : m.interfaces) {
                Entity ie;
                ie.key = key("if", i.id);
                ie.parent = moduleKey;
                ie.kind = "interface";
                ie.label = path({d.name, m.name, i.name});
                ie.description = i.ip.empty() ? std::string() : "IP " + i.ip;
                ie.attrs.push_back({"name", i.name, 0});
                ie.attrs.push_back({"IP address", i.ip, 0});
                ie.attrs.push_back({"subnet mask", i.mask, 0});
                ie.attrs.push_back({"router", i.router, 0});
                ie.attrs.push_back({"IP protocol used", i.hasIpSettings ? yesNo(i.ipProtocolUsed) : std::string(), 0});
                ie.attrs.push_back(
                    {"IP address set outside the project", i.hasIpSettings ? yesNo(i.ipAssignedElsewhere) : std::string(), 0});
                ie.attrs.push_back({"bus address", i.hasBusAddress ? std::to_string(i.busAddress) : std::string(), 0});
                ie.attrs.push_back({"PROFINET name", i.profinetNameAuto ? std::string() : i.profinetName, 0});
                ie.attrs.push_back({"PROFINET name generated automatically", yesNo(i.profinetNameAuto), 0});
                ie.attrs.push_back({"MAC address", i.configuredMac, 0});
                ie.attrs.push_back({"subnet", i.subnet, kRef});
                snap.add(std::move(ie));
            }
        }
    }

    for (const auto& s : inv.ioSystems) {
        Entity e;
        e.key = key("io", s.id);
        e.kind = "IO system";
        e.label = path({s.controllerDevice, s.name});
        e.description = s.kind;
        e.attrs.push_back({"name", s.name, 0});
        e.attrs.push_back({"number", s.hasNumber ? std::to_string(s.number) : std::string(), 0});
        e.attrs.push_back({"controller", path({s.controllerDevice, s.controller}), kRef});
        e.attrs.push_back({"subnet", s.subnet, kRef});
        snap.add(std::move(e));
    }

    for (const auto& l : inv.portLinks) {
        Entity e;
        const uint64_t lo = std::min(l.a.id, l.b.id), hi = std::max(l.a.id, l.b.id);
        e.key = "link:" + std::to_string(lo) + "-" + std::to_string(hi);
        e.kind = "port connection";
        e.label = path({l.a.device, l.a.module, l.a.port}) + " <-> " + path({l.b.device, l.b.module, l.b.port});
        snap.add(std::move(e));
    }

    for (const auto& c : inv.connections) {
        Entity e;
        e.key = key("conn", c.id);
        e.kind = "connection";
        e.label = path({c.local.device, c.name});
        const std::string partner = endText(c.partner).empty() ? c.partnerAddress : endText(c.partner);
        e.description = c.kind + (partner.empty() ? std::string() : " to " + partner);
        e.attrs.push_back({"name", c.name, 0});
        e.attrs.push_back({"type", c.kind, 0});
        e.attrs.push_back({"local end", endText(c.local), kRef});
        e.attrs.push_back({"partner", endText(c.partner), kRef});
        e.attrs.push_back({"partner address", c.partnerAddress, 0});
        e.attrs.push_back({"local ID", c.hasLocalId ? std::to_string(c.localId) : std::string(), 0});
        e.attrs.push_back({"partner ID", c.hasPartnerId ? std::to_string(c.partnerId) : std::string(), 0});
        e.attrs.push_back({"configured on both sides", yesNo(c.bothSides), 0});
        e.attrs.push_back({"over TCP/IP", setting(c.overTcpIp), 0});
        e.attrs.push_back({"one-way", setting(c.oneWay), 0});
        e.attrs.push_back({"active connection establishment", setting(c.activeEstablishment), 0});
        snap.add(std::move(e));
    }

    auto plcKey = [&](const std::string& plc) {
        auto it = plcKeys.find(plc);
        return it == plcKeys.end() ? std::string() : it->second;
    };

    for (const auto& b : prog.blockList) {
        Entity e;
        e.key = key("blk", b.id);
        e.parent = plcKey(b.plc);
        e.kind = b.type == "UDT" || b.type == "SDT" ? "data type" : "block";
        const std::string address = b.hasNumber ? b.type + std::to_string(b.number) : b.type;
        e.label = path({b.plc, b.name + " [" + address + "]"});
        e.description = commaList([&] {
            std::vector<std::string> parts;
            if (!b.kind.empty()) parts.push_back(b.kind);
            if (!b.language.empty()) parts.push_back(b.language);
            if (!b.protection.empty()) parts.push_back(b.protection + " protected");
            return parts;
        }());
        e.attrs.push_back({"name", b.name, 0});
        e.attrs.push_back({"type", b.type, 0});
        e.attrs.push_back({"number", b.hasNumber ? std::to_string(b.number) : std::string(), 0});
        e.attrs.push_back({"kind", b.kind, 0});
        e.attrs.push_back({"instance of", b.instanceOf, kRef});
        e.attrs.push_back({"language", b.language.empty() ? b.languageStored : b.language, 0});
        e.attrs.push_back({"protection", b.protection, 0});
        e.attrs.push_back({"copy protection", b.copyProtection, 0});
        e.attrs.push_back({"folder", b.folder, 0});
        e.attrs.push_back({"title", b.title, 0});
        e.attrs.push_back({"comment", b.comment, 0});
        e.attrs.push_back({"author", b.author, 0});
        e.attrs.push_back({"family", b.family, 0});
        e.attrs.push_back({"user-defined ID", b.userId, 0});
        e.attrs.push_back({"version", b.version, 0});
        e.attrs.push_back({"optimized block access", b.hasAccess ? yesNo(b.symbolicAccessOnly) : std::string(), 0});
        e.attrs.push_back({"networks", b.hasNetworks ? std::to_string(b.networks) : std::string(), 0});
        e.attrs.push_back({"write-protected in the device", flag(b.writeProtectedInDevice), 0});
        e.attrs.push_back({"only stored in load memory", flag(b.onlyInLoadMemory), 0});
        e.attrs.push_back({"accessible from OPC UA", flag(b.accessibleFromOpcUa), 0});
        e.attrs.push_back({"accessible via web server", flag(b.accessibleFromWebServer), 0});
        e.attrs.push_back({"load memory", b.hasLoadMemory ? std::to_string(b.loadMemory) : std::string(), kResult});
        e.attrs.push_back({"work memory", b.hasWorkMemory ? std::to_string(b.workMemory) : std::string(), kResult});
        e.attrs.push_back({"code changed", b.codeModified, kTime});
        e.attrs.push_back({"interface changed", b.interfaceModified, kTime});
        e.attrs.push_back({"compiled", b.compiled, kTime});
        e.attrs.push_back({"downloaded", b.downloaded, kTime});
        e.attrs.push_back({"needs compiling", b.compileNeeded.empty() || b.compileNeeded == "UpToDate" ? std::string()
                                                                                                     : b.compileNeeded,
                           0});
        e.attrs.push_back({"modified", b.modified, kTime | kMarker});
        snap.add(std::move(e));
    }

    // The networks of the code blocks. A block whose code is not read has
    // none here, in any save.
    for (const BlockCode& b : code.blocks) {
        const std::string blockKey = key("blk", b.blockId);
        auto it = snap.index.find(blockKey);
        if (it == snap.index.end()) continue;
        if (b.isProtected) {
            snap.list[it->second].codeUnread = true;
            continue;
        }
        const std::string blockLabel = snap.list[it->second].label;
        for (const Network& n : b.networks) {
            Entity e;
            e.key = key("nw", n.id);
            e.parent = blockKey;
            e.kind = "network";
            e.label = blockLabel + " / network " + std::to_string(n.number);
            e.description = commaList([&] {
                std::vector<std::string> parts;
                if (!n.title.empty()) parts.push_back(n.title);
                if (!n.language.empty()) parts.push_back(n.language);
                if (n.content == "empty") parts.push_back("empty");
                return parts;
            }());
            e.attrs.push_back({"title", n.title, 0});
            e.attrs.push_back({"comment", n.comment, 0});
            e.attrs.push_back({"language", n.language.empty() ? n.languageStored : n.language, 0});
            e.code = n.lines;
            snap.add(std::move(e));
        }
    }

    for (const auto& db : prog.blocks) {
        const std::string blockKey = key("blk", db.id);
        auto it = snap.index.find(blockKey);
        const std::string label = it != snap.index.end() ? snap.list[it->second].label : path({db.plc, db.name});
        addMembers(snap, blockKey, label, db.members, std::string(), 0);
    }

    for (const auto& t : prog.tags) {
        Entity e;
        e.key = key("tag", t.id);
        e.parent = plcKey(t.plc);
        e.kind = "tag";
        e.label = path({t.plc, t.table, t.name});
        e.description = commaList([&] {
            std::vector<std::string> parts;
            if (!t.dataType.empty()) parts.push_back(t.dataType);
            if (!t.address.empty()) parts.push_back(t.address);
            return parts;
        }());
        e.attrs.push_back({"name", t.name, 0});
        e.attrs.push_back({"tag table", t.table, 0});
        e.attrs.push_back({"data type", t.dataType, 0});
        e.attrs.push_back({"address", t.address, 0});
        e.attrs.push_back({"comment", t.comment, 0});
        snap.add(std::move(e));
    }

    for (const auto& t : prog.hmiTags) {
        Entity e;
        e.key = key("hmitag", t.id);
        auto dev = deviceKeys.find(t.hmi);
        if (dev != deviceKeys.end()) e.parent = dev->second;
        e.kind = "HMI tag";
        e.label = path({t.hmi, t.table, t.name});
        e.description = commaList([&] {
            std::vector<std::string> parts;
            if (!t.dataType.empty()) parts.push_back(t.dataType);
            if (t.access == "internal") parts.push_back("internal");
            else if (!t.plcTag.empty()) parts.push_back("PLC tag " + t.plcTag);
            else if (!t.address.empty()) parts.push_back(t.address);
            return parts;
        }());
        e.attrs.push_back({"name", t.name, 0});
        e.attrs.push_back({"tag table", t.table, 0});
        e.attrs.push_back({"data type", t.dataType, 0});
        e.attrs.push_back({"connection", t.connection, kRef});
        e.attrs.push_back({"PLC", t.plc, kRef});
        e.attrs.push_back({"PLC tag", t.plcTag, 0});
        e.attrs.push_back({"link to the PLC tag", t.access == "symbolic" && !t.plcTagLinked ? "broken" : "", 0});
        e.attrs.push_back({"address", t.address, 0});
        e.attrs.push_back({"acquisition cycle", t.acquisitionCycle, 0});
        e.attrs.push_back({"acquisition mode", acquisitionModeName(t.acquisitionMode), 0});
        e.attrs.push_back({"comment", t.comment, 0});
        e.attrs.push_back({"start value", t.startValue, 0});
        snap.add(std::move(e));
    }

    for (const auto& c : prog.constants) {
        // The process image partitions are the same 34 constants in every PLC.
        if (c.kind == "pip") continue;
        Entity e;
        e.key = key("const", c.id);
        e.parent = plcKey(c.plc);
        e.kind = c.kind == "user" ? "user constant" : (c.kind == "hardware" ? "hardware identifier" : "system constant");
        e.label = path({c.plc, c.table, c.name});
        e.description = c.dataType + (c.value.empty() ? std::string() : " = " + c.value);
        e.attrs.push_back({"name", c.name, 0});
        e.attrs.push_back({"tag table", c.table, 0});
        e.attrs.push_back({"data type", c.dataType, 0});
        e.attrs.push_back({"value", c.value, 0});
        e.attrs.push_back({"comment", c.comment, 0});
        e.attrs.push_back({"stands for", path({c.standsForDevice, c.standsFor}), kRef});
        snap.add(std::move(e));
    }
    return snap;
}

// Replaces the parts of a " / " path that are old names by the new ones.
std::string renamed(const std::string& value, const std::map<std::string, std::string>& renames) {
    std::string out;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t end = value.find(" / ", pos);
        if (end == std::string::npos) end = value.size();
        std::string part = value.substr(pos, end - pos);
        auto it = renames.find(part);
        if (!out.empty() || pos) out += " / ";
        out += it == renames.end() ? part : it->second;
        pos = end + 3;
    }
    return out;
}

// The code is the same but for names that changed in this save: a renamed
// tag or block appears with its new name wherever it is used.
bool renamedCode(const std::vector<std::string>& before, const std::vector<std::string>& after,
                 const std::map<std::string, std::string>& renames) {
    if (renames.empty() || before.size() != after.size()) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        std::string line = before[i];
        for (const auto& r : renames) {
            const std::string from = "\"" + r.first + "\"", to = "\"" + r.second + "\"";
            for (size_t pos = line.find(from); pos != std::string::npos; pos = line.find(from, pos + to.size()))
                line.replace(pos, from.size(), to);
        }
        if (line != after[i]) return false;
    }
    return true;
}

// An item that was removed and added again under the same name in one save
// (TIA Portal generates an instance data block anew when its function block
// changes) is one item that changed, not two. Returns `before` with the keys
// of such items, and of their parts, replaced by the new ones.
Snapshot rekey(const Snapshot& before, const Snapshot& after, std::map<std::string, std::string>& recreated) {
    using Name = std::pair<std::string, std::string>;  // kind, label
    std::map<Name, std::vector<const Entity*>> gone, come;
    // Only what belongs to a PLC and is named within it: a station deleted
    // and another added under the same default name are two stations.
    auto eligible = [](const Entity& e) {
        return e.kind == "block" || e.kind == "data type" || e.kind == "tag" || e.kind == "HMI tag" ||
               e.kind == "user constant" ||
               e.kind == "hardware identifier" || e.kind == "system constant";
    };
    for (const auto& e : before.list)
        if (eligible(e) && !after.index.count(e.key)) gone[{e.kind, e.label}].push_back(&e);
    for (const auto& e : after.list)
        if (eligible(e) && !before.index.count(e.key)) come[{e.kind, e.label}].push_back(&e);
    for (const auto& kv : gone) {
        auto it = come.find(kv.first);
        if (kv.second.size() != 1 || it == come.end() || it->second.size() != 1) continue;
        if (kv.second[0]->parent != it->second[0]->parent) continue;
        recreated[kv.second[0]->key] = it->second[0]->key;
    }
    if (recreated.empty()) return before;
    Snapshot out;
    out.hasProject = before.hasProject;
    out.modified = before.modified;
    out.by = before.by;
    out.events = before.events;
    for (Entity e : before.list) {
        auto self = recreated.find(e.key);
        auto parent = recreated.find(e.parent);
        if (self != recreated.end()) {
            e.key = self->second;
        } else if (parent != recreated.end()) {
            const std::string prefix = "mem:" + e.parent + ":";
            if (e.key.compare(0, prefix.size(), prefix) == 0) e.key = "mem:" + parent->second + ":" + e.key.substr(prefix.size());
            e.parent = parent->second;
        }
        out.add(std::move(e));
    }
    return out;
}

void diff(const Snapshot& original, const Snapshot& after, std::vector<HistoryChange>& out) {
    std::map<std::string, std::string> recreated;  // old key -> new key
    const Snapshot before = rekey(original, after, recreated);
    std::map<std::string, std::string> recreatedFrom;
    for (const auto& kv : recreated) recreatedFrom[kv.second] = kv.first;
    auto added = [&](const Entity& e) { return before.index.find(e.key) == before.index.end(); };
    auto removed = [&](const Entity& e) { return after.index.find(e.key) == after.index.end(); };

    // Names that changed in this save. A value that differs only by such a
    // name (the partner of a connection after its PLC was renamed) is not a
    // change of its own.
    std::map<std::string, std::string> renames;
    for (const auto& e : after.list) {
        auto it = before.index.find(e.key);
        if (it == before.index.end()) continue;
        const Entity& old = before.list[it->second];
        if (!e.attrs.empty() && !old.attrs.empty() && e.attrs[0].name == "name" && old.attrs[0].name == "name" &&
            e.attrs[0].value != old.attrs[0].value && !old.attrs[0].value.empty())
            renames.emplace(old.attrs[0].value, e.attrs[0].value);
    }

    // The networks of a block whose code is not read, before or after, say
    // nothing: they are not compared.
    auto unreadBlock = [&](const Entity& e) {
        if (e.kind != "network") return false;
        for (const Snapshot* s : {&before, &after}) {
            auto p = s->index.find(e.parent);
            if (p != s->index.end() && s->list[p->second].codeUnread) return true;
        }
        return false;
    };

    for (const auto& e : after.list) {
        if (unreadBlock(e)) continue;
        auto it = before.index.find(e.key);
        if (it == before.index.end()) {
            HistoryChange c;
            c.change = "added";
            c.kind = e.kind;
            c.item = e.label;
            c.description = e.description;
            c.to = joinLines(e.code);
            c.key = e.key;
            c.parentKey = e.parent;
            if (!e.parent.empty()) {
                auto p = after.index.find(e.parent);
                if (p != after.index.end() && added(after.list[p->second])) c.partOf = after.list[p->second].label;
            }
            out.push_back(std::move(c));
            continue;
        }
        const Entity& old = before.list[it->second];
        auto again = recreatedFrom.find(e.key);
        if (again != recreatedFrom.end()) {
            HistoryChange c;
            c.change = "changed";
            c.kind = e.kind;
            c.item = e.label;
            c.attribute = "created anew";
            c.from = "object " + again->second.substr(again->second.find(':') + 1);
            c.to = "object " + e.key.substr(e.key.find(':') + 1);
            c.isTime = true;
            c.key = e.key;
            c.parentKey = e.parent;
            out.push_back(std::move(c));
        }
        std::map<std::string, const Attr*> oldAttrs;
        for (const auto& a : old.attrs) oldAttrs[a.name] = &a;
        for (const auto& a : e.attrs) {
            auto o = oldAttrs.find(a.name);
            const std::string from = o == oldAttrs.end() ? std::string() : o->second->value;
            if (from == a.value) continue;
            if ((a.flags & kRef) && !renames.empty() && renamed(from, renames) == a.value) continue;
            HistoryChange c;
            c.change = "changed";
            c.kind = e.kind;
            c.item = e.label;
            c.attribute = a.name;
            c.from = from;
            c.to = a.value;
            c.isTime = (a.flags & kTime) != 0;
            c.isMarker = (a.flags & kMarker) != 0;
            c.isResult = (a.flags & kResult) != 0;
            c.key = e.key;
            c.parentKey = e.parent;
            out.push_back(std::move(c));
        }
        if (e.code != old.code && !renamedCode(old.code, e.code, renames)) {
            // the lines that differ, without what is the same before and after them
            size_t head = 0;
            while (head < e.code.size() && head < old.code.size() && e.code[head] == old.code[head]) ++head;
            size_t tail = 0;
            while (tail < e.code.size() - head && tail < old.code.size() - head &&
                   e.code[e.code.size() - 1 - tail] == old.code[old.code.size() - 1 - tail])
                ++tail;
            HistoryChange c;
            c.change = "changed";
            c.kind = e.kind;
            c.item = e.label;
            c.attribute = "code";
            c.from = joinLines(std::vector<std::string>(old.code.begin() + static_cast<long>(head),
                                                        old.code.end() - static_cast<long>(tail)));
            c.to = joinLines(std::vector<std::string>(e.code.begin() + static_cast<long>(head),
                                                      e.code.end() - static_cast<long>(tail)));
            c.key = e.key;
            c.parentKey = e.parent;
            out.push_back(std::move(c));
        }
    }
    for (const auto& e : before.list) {
        if (!removed(e) || unreadBlock(e)) continue;
        HistoryChange c;
        c.change = "removed";
        c.kind = e.kind;
        c.item = e.label;
        c.description = e.description;
        c.from = joinLines(e.code);
        c.key = e.key;
        c.parentKey = e.parent;
        if (!e.parent.empty()) {
            auto p = before.index.find(e.parent);
            if (p != before.index.end() && removed(before.list[p->second])) c.partOf = before.list[p->second].label;
        }
        out.push_back(std::move(c));
    }
}

// For two files that do not share the identities of their objects: an id
// in one says nothing about the other (new projects number their objects
// alike), so items are paired by kind and name alone, where each side has
// one item of that kind and name. The others get keys of their own.
Snapshot matchByName(const Snapshot& before, const Snapshot& after, size_t& matched) {
    using Name = std::pair<std::string, std::string>;
    std::map<Name, std::vector<const Entity*>> mine, theirs;
    for (const auto& e : before.list) mine[{e.kind, e.label}].push_back(&e);
    for (const auto& e : after.list) theirs[{e.kind, e.label}].push_back(&e);
    std::map<std::string, std::string> to;
    for (const auto& e : before.list) {
        if (e.kind == "project") {  // there is one on each side
            to[e.key] = e.key;
            continue;
        }
        const Name n{e.kind, e.label};
        auto it = theirs.find(n);
        if (mine[n].size() == 1 && it != theirs.end() && it->second.size() == 1) to[e.key] = it->second[0]->key;
        else to[e.key] = "old:" + e.key;
    }
    matched = 0;
    for (const auto& kv : to)
        if (kv.second.compare(0, 4, "old:") != 0 && kv.first != "project") ++matched;
    Snapshot out;
    out.hasProject = before.hasProject;
    out.created = before.created;
    out.modified = before.modified;
    out.by = before.by;
    out.events = before.events;
    for (Entity e : before.list) {
        e.key = to[e.key];
        auto parent = to.find(e.parent);
        if (parent != to.end()) e.parent = parent->second;
        out.add(std::move(e));
    }
    return out;
}

}  // namespace

ProjectDiff diffProjects(const Project& oldProject, const ProtectedVersions& oldProtected, const Project& newProject,
                         const ProtectedVersions& newProtected) {
    ProjectDiff d;
    const Snapshot before = takeSnapshot(oldProject, oldProtected);
    const Snapshot after = takeSnapshot(newProject, newProtected);
    d.itemsOld = before.list.size();
    d.itemsNew = after.list.size();
    // Versions of one project, "Save as" included, keep the time the
    // project was created, and the ids of their objects. Without that time,
    // most keys in common say the same.
    if (!before.created.empty() && !after.created.empty()) {
        d.sameLineage = before.created == after.created;
    } else {
        size_t shared = 0, counted = 0;
        for (const auto& e : before.list) {
            if (e.kind == "project" || e.kind == "project event") continue;
            ++counted;
            if (after.index.count(e.key)) ++shared;
        }
        d.sameLineage = counted == 0 || shared * 2 >= counted;
    }
    std::set<std::string> unread;
    for (const Snapshot* snap : {&before, &after})
        for (const auto& e : snap->list)
            if (e.codeUnread) unread.insert(e.label);
    d.unreadBlocks = unread.size();
    if (d.sameLineage) {
        diff(before, after, d.changes);
    } else {
        const Snapshot paired = matchByName(before, after, d.matchedByName);
        diff(paired, after, d.changes);
    }
    return d;
}

bool substantialChange(const HistoryChange& c) {
    if (c.change != "changed") return true;
    return !c.isTime && !c.isMarker && !c.isResult && c.attribute != "needs compiling";
}

History buildHistory(std::shared_ptr<const std::vector<uint8_t>> data, size_t throughSave,
                     const std::function<void(size_t, size_t)>& progress) {
    History h;
    // The whole file once, for the list of blocks and where each save ends.
    const Container whole = Container::parse(data);
    h.savesInFile = whole.saveCount();
    const size_t last = throughSave && throughSave < h.savesInFile ? throughSave : h.savesInFile;

    std::vector<std::string> commitTimes;
    for (const auto& m : whole.markers())
        if (m.kind == "commit") commitTimes.push_back(formatTicks(m.ticks));

    // A block is not read from the versions the file keeps from before it
    // was know-how protected.
    ProtectedVersions protectedUpTo;
    try {
        const Project latest(Container::parse(data));
        if (!latest.meta().empty()) protectedUpTo = protectedVersions(latest);
    } catch (const ParseError&) {
    }

    Snapshot before;
    bool seenProject = false;
    // Objects written after the last save marker. Seen in project files that
    // were written in one go (one marker after the type model, then the whole
    // project): the present state is then not that of the last save.
    const bool unclosed = !throughSave && whole.objectsAfterLastSave() > 0;
    for (size_t n = 1; n <= last + (unclosed ? 1 : 0); ++n) {
        if (progress) progress(n, last + (unclosed ? 1 : 0));
        HistorySave s;
        s.number = n;
        s.afterLastSave = n > last;
        Snapshot after;
        const MetaModel* meta = nullptr;
        std::unique_ptr<Project> project;
        try {
            ContainerOptions opt;
            opt.throughSave = s.afterLastSave ? 0 : n;
            project.reset(new Project(Container::parse(data, opt)));
            meta = &project->meta();
            after = takeSnapshot(*project, protectedUpTo);
        } catch (const ParseError& e) {
            s.problem = e.what();
        }

        // What this save wrote: the blocks between the end of the previous
        // save and the end of this one, the last version of each object.
        bool projectWritten = false;
        std::string latestChange, changedBy, changedByTime;
        {
            const size_t begin = n >= 2 ? whole.saveEnds()[n - 2] : 0;
            const size_t end = s.afterLastSave ? whole.blocks().size() : whole.saveEnds()[n - 1];
            std::map<std::pair<uint32_t, uint64_t>, bool> written;  // object -> deleted
            for (size_t i = begin; i < end && i < whole.blocks().size(); ++i) {
                const Block& b = whole.blocks()[i];
                if (whole.isSystem(b)) continue;
                written[{b.type, b.id}] = b.deleted();
            }
            std::map<std::string, HistorySave::TypeCount> byType;
            for (const auto& kv : written) {
                ++s.objectsWritten;
                if (kv.second) ++s.objectsDeleted;
                std::string name;
                const TypeDef* t = meta ? meta->findById(kv.first.first) : nullptr;
                if (t) name = t->shortName();
                if (name.empty()) {
                    char buf[24];
                    std::snprintf(buf, sizeof buf, "type 0x%x", static_cast<unsigned>(kv.first.first));
                    name = buf;
                }
                auto& tc = byType[name];
                tc.type = name;
                ++tc.written;
                if (kv.second) ++tc.deleted;

                const bool isProject = t && meta->derivesFromShort(t->name, "ProjectData");
                if (isProject && !kv.second) projectWritten = true;
                if (!t || kv.second || !project) continue;
                // when the object was last changed, and by whom
                const Block* b = project->live(kv.first.first, kv.first.second);
                if (!b) continue;
                try {
                    Object o;
                    if (!project->decode(*b, o)) continue;
                    const std::string modified = o.attrString("ICoreAttributes", "ModifiedTime");
                    if (modified.empty()) continue;
                    if (modified > latestChange) latestChange = modified;
                    const std::string by = o.attrString("ICoreAttributes", "LastModifiedBy");
                    if (!by.empty() && modified > changedByTime) {
                        changedByTime = modified;
                        changedBy = by;
                    }
                } catch (const ParseError&) {
                }
            }
            for (auto& kv : byType) s.objectTypes.push_back(std::move(kv.second));
            std::stable_sort(s.objectTypes.begin(), s.objectTypes.end(),
                             [](const HistorySave::TypeCount& a, const HistorySave::TypeCount& b) {
                                 return a.written > b.written;
                             });
        }

        if (!s.problem.empty()) {
            h.saves.push_back(std::move(s));
            continue;  // compare the next save with the last one that could be read
        }

        s.hasProject = after.hasProject;
        s.by = projectWritten && !after.by.empty() ? after.by : changedBy;
        if (whole.layout() == Layout::V11 && !s.afterLastSave) {
            if (n - 1 < commitTimes.size()) {
                s.time = commitTimes[n - 1];
                s.timeSource = "save";
            }
        } else if (!latestChange.empty()) {
            s.time = latestChange;
            s.timeSource = "latest_change";
        }

        if (!seenProject) {
            if (after.hasProject || !after.list.empty()) {
                seenProject = true;
                s.firstState = true;
                std::vector<std::string> order;
                std::map<std::string, size_t> counts;
                for (const auto& e : after.list) {
                    if (e.kind == "project" || e.kind == "project event") continue;
                    if (!counts.count(e.kind)) order.push_back(e.kind);
                    ++counts[e.kind];
                }
                for (const auto& k : order) s.contents.emplace_back(k, counts[k]);
            }
        } else {
            diff(before, after, s.changes);
        }
        s.beforeProject = !seenProject;
        h.events = after.events;
        before = std::move(after);
        h.saves.push_back(std::move(s));
    }

    if (h.savesInFile == 0) h.notes.push_back("the file records no saves");
    if (unclosed)
        h.notes.push_back("the file goes on after its last save marker: what was written there is listed as \"after save " +
                          std::to_string(last) + "\", and is the present state of the project");
    if (throughSave && throughSave < h.savesInFile)
        h.notes.push_back("covers saves 1 to " + std::to_string(last) + " of " + std::to_string(h.savesInFile));
    // The project is already there in full in the first save that holds it:
    // what was saved before is not in the file.
    for (const auto& s : h.saves)
        if (s.firstState) {
            if (!s.contents.empty())
                h.notes.push_back(std::string("the history starts ") +
                                  (s.afterLastSave ? "after save " + std::to_string(s.number - 1)
                                                   : "at save " + std::to_string(s.number)) +
                                  " with a project that already has contents; what was saved before is not in the "
                                  "file (TIA Portal sometimes writes the whole file anew, which drops the earlier saves)");
            break;
        }
    return h;
}

}  // namespace tia
