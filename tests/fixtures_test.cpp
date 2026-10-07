// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the V21 test projects in tests/fixtures. Each project differs from the
// previous one by one known change (see tests/fixtures/README.md), so every
// value checked here was typed into TIA Portal by hand.
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "container.hpp"
#include "history.hpp"
#include "inventory.hpp"
#include "program.hpp"
#include "project.hpp"
#include "source.hpp"

#ifndef TIACONV_FIXTURES
#error "TIACONV_FIXTURES must point at tests/fixtures"
#endif

namespace {

int failures = 0;
int checks = 0;
std::string current;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++checks;                                                                      \
        if (!(cond)) {                                                                 \
            ++failures;                                                                \
            std::printf("FAIL %s (%s:%d)  %s\n", current.c_str(), __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

struct Loaded {
    tia::Inventory inv;
    tia::ProgramData prog;
    size_t hashErrors = 0;
    size_t saves = 0;
    tia::Layout layout = tia::Layout::V11;
};

Loaded load(const std::string& name, size_t throughSave = 0) {
    current = throughSave ? name + " after save " + std::to_string(throughSave) : name;
    tia::LoadedSource src = tia::loadProjectData(std::string(TIACONV_FIXTURES) + "/" + name);
    tia::ContainerOptions opt;
    opt.verifyHashes = true;
    opt.throughSave = throughSave;
    tia::Project p(tia::Container::parse(std::move(src.data), opt));
    Loaded l;
    l.inv = tia::buildInventory(p);
    l.prog = tia::buildProgramData(p);
    l.hashErrors = p.container().hashErrors();
    l.saves = p.container().saveCount();
    l.layout = p.container().layout();
    return l;
}

const tia::Module* module(const tia::Inventory& inv, const std::string& name) {
    for (const auto& d : inv.devices)
        for (const auto& m : d.modules)
            if (m.name == name) return &m;
    return nullptr;
}

const tia::Interface* firstInterface(const tia::Inventory& inv, const std::string& moduleName) {
    const tia::Module* m = module(inv, moduleName);
    return (m && !m->interfaces.empty()) ? &m->interfaces.front() : nullptr;
}

const tia::BlockInfo* blockInfo(const tia::ProgramData& p, const std::string& plc, const std::string& name) {
    for (const auto& b : p.blockList)
        if (b.plc == plc && b.name == name) return &b;
    return nullptr;
}

void common(const Loaded& l, const std::string& projectName) {
    CHECK(l.layout == tia::Layout::V14);
    CHECK(l.hashErrors == 0);
    CHECK(l.inv.warnings.empty());
    CHECK(l.inv.stats.objectsWithProblems == 0);
    CHECK(l.inv.stats.unattachedItems == 0);
    CHECK(l.inv.project.found && l.inv.project.name == projectName);
}

// ---- save history ----
//
// What the history has to show for a save, written as
//   +|kind|item|description      added
//   -|kind|item|description      removed
//   ~|kind|item|attribute|from|to
// Left out, because they follow from something else: the parts of an item
// that was added or removed as a whole, and what compiling a block changes.
struct Step {
    size_t save;
    const char* change;
};

// tests/fixtures/README.md says what was done in TIA Portal before each of
// these saves; every line here was compared with it. Saves 17 to 27 built
// the program of s08_program and have no step of their own there.
const Step kStepsBlocksA[] = {
    {3, "+|device|S7-1200 station_1|S71200.Device"},
    {4, "~|project|s01_cpu|name|s00_empty|s01_cpu"},
    {5, "~|module|S7-1200 station_1 / ZZALPHA|name|PLC_1|ZZALPHA"},
    {6, "~|project|s02_name|name|s01_cpu|s02_name"},
    {7, "+|subnet|PN/IE_1|"},
    {7, "~|interface|S7-1200 station_1 / ZZALPHA / X1 : PN(LAN)|IP address|192.168.0.1|192.168.77.11"},
    {7, "~|interface|S7-1200 station_1 / ZZALPHA / X1 : PN(LAN)|subnet||PN/IE_1"},
    {8, "~|project|s03_ip|name|s02_name|s03_ip"},
    {9, "~|interface|S7-1200 station_1 / ZZALPHA / X1 : PN(LAN)|router||192.168.77.1"},
    {10, "~|project|s04_router|name|s03_ip|s04_router"},
    {11, "~|interface|S7-1200 station_1 / ZZALPHA / X1 : PN(LAN)|PROFINET name||zzalpha-pn"},
    {11, "~|interface|S7-1200 station_1 / ZZALPHA / X1 : PN(LAN)|PROFINET name generated automatically|yes|no"},
    {12, "~|project|s05_pnname|name|s04_router|s05_pnname"},
    {13, "+|module|S7-1200 station_1 / DI 8x24VDC_1|SM 1221 DI8 x 24VDC, 6ES7 221-1BF30-0XB0, V1.0"},
    {13, "+|hardware identifier|ZZALPHA / Default tag table / Local~DI_8x24VDC_1|Hw_SubModule = 267"},
    {14, "~|project|s06_module|name|s05_pnname|s06_module"},
    {15, "+|device|S7-1500/ET200MP station_1|S71500.Device"},
    {16, "~|project|s07_second|name|s06_module|s07_second"},
    {28, "~|project|s09_security|name|s08|s09_security"},
    {29, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|PUT/GET access||yes"},
    {30, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|access level||Read access"},
    {31, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|access level|Read access|HMI access"},
    {32, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|access level|HMI access|No access (complete protection)"},
    {32, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|PUT/GET access|yes|no"},
    {33, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|access level|No access (complete protection)|Full access (no protection)"},
    {34, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|access level|Full access (no protection)|HMI access"},
    {34, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|PUT/GET access|no|yes"},
    {35, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server access on||X1"},
    {36, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server access on|X1|"},
    {37, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server||yes"},
    {37, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server access on||X1"},
    {38, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server access on|X1|"},
    {39, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server access on||X1"},
    {40, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|web server HTTPS only||yes"},
    {41, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|time synchronisation||NTP"},
    {41, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|NTP servers||192.168.77.50"},
    {42, "~|module|S7-1500/ET200MP station_1 / ZZBRAVO|display protection||yes"},
    {43, "~|module|S7-1200 station_1 / ZZALPHA|access level||Write protection"},
    {44, "~|module|S7-1200 station_1 / ZZALPHA|web server||yes"},
    {45, "~|module|S7-1200 station_1 / ZZALPHA|access level|Write protection|Write/read protection"},
    {46, "+|device|S7-1500/ET200MP station_2|S71500.Device"},
    {47, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|OPC UA server||yes"},
    {48, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|PG/PC and HMI communication||legacy communication permitted"},
    {49, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|protection of confidential configuration data|yes|no"},
    {50, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|access protection|users_and_roles|none"},
    {50, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|access control||no"},
    {51, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|access protection|none|users_and_roles_and_access_levels"},
    {51, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|access control|no|yes"},
    {51, "~|module|S7-1500/ET200MP station_2 / ZZCHARLIE|access control via access levels||yes"},
    {53, "~|project|s10_connections|name|s09_security|s10_connections"},
    {54, "~|interface|S7-1500/ET200MP station_1 / ZZBRAVO / X1|subnet||PN/IE_1"},
    {55, "~|interface|S7-1500/ET200MP station_2 / ZZCHARLIE / X1|IP address|192.168.0.1|192.168.77.13"},
    {55, "~|interface|S7-1500/ET200MP station_2 / ZZCHARLIE / X1|subnet||PN/IE_1"},
    {56, "+|device|ZZIO1|ET200SP.Device"},
    {57, "~|module|ZZIO1 / IO device_1|IO controller||ZZBRAVO"},
    {57, "~|module|ZZIO1 / IO device_1|IO system||PROFINET IO-System"},
    {57, "~|interface|ZZIO1 / IO device_1 / IE1|IP address|192.168.0.1|192.168.77.1"},
    {57, "~|interface|ZZIO1 / IO device_1 / IE1|subnet||PN/IE_1"},
    {57, "+|IO system|S7-1500/ET200MP station_1 / PROFINET IO-System|IOSystem_PROFINET"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / Local~PROFINET_IO-System|Hw_IoSystem = 257"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~Head|Hw_SubModule = 258"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~PROFINET_interface|Hw_Interface = 259"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~PROFINET_interface~Port_1|Hw_Interface = 260"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~PROFINET_interface~Port_2|Hw_Interface = 261"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~IODevice|Hw_Device = 262"},
    {57, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_1~Proxy|Hw_SubModule = 264"},
    {58, "+|device|ZZI02|ET200SP.Device"},
    {58, "+|IO system|S7-1500/ET200MP station_2 / PROFINET IO-System|IOSystem_PROFINET"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / Local~PROFINET_IO-System|Hw_IoSystem = 257"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~IODevice|Hw_Device = 265"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~Proxy|Hw_SubModule = 267"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~Head|Hw_SubModule = 268"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface|Hw_Interface = 269"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface~Port_1|Hw_Interface = 270"},
    {58, "+|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface~Port_2|Hw_Interface = 271"},
    {59, "~|module|ZZI02 / IO device_2|IO controller|ZZCHARLIE|ZZBRAVO"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~IODevice|Hw_Device = 265"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~Proxy|Hw_SubModule = 267"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~Head|Hw_SubModule = 268"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~PROFINET_interface|Hw_Interface = 269"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~PROFINET_interface~Port_1|Hw_Interface = 270"},
    {59, "+|hardware identifier|ZZBRAVO / Default tag table / IO_device_2~PROFINET_interface~Port_2|Hw_Interface = 271"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~IODevice|Hw_Device = 265"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~Proxy|Hw_SubModule = 267"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~Head|Hw_SubModule = 268"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface|Hw_Interface = 269"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface~Port_1|Hw_Interface = 270"},
    {59, "-|hardware identifier|ZZCHARLIE / Default tag table / IO_device_2~PROFINET_interface~Port_2|Hw_Interface = 271"},
    {60, "+|port connection|S7-1500/ET200MP station_1 / ZZBRAVO / X1 P1 <-> ZZIO1 / IO device_1 / X1 P1R|"},
    {61, "+|port connection|ZZIO1 / IO device_1 / X1 P2R <-> ZZI02 / IO device_2 / X1 P1R|"},
    {62, "+|connection|S7-1500/ET200MP station_1 / S7_Connection_1|S7 to S7-1500/ET200MP station_2 / ZZCHARLIE / X1"},
    {63, "+|connection|S7-1500/ET200MP station_1 / S7_Connection_2|S7 to 192.168.77.99"},
    {64, "-|connection|S7-1500/ET200MP station_1 / S7_Connection_1|S7 to S7-1500/ET200MP station_2 / ZZCHARLIE / X1"},
    {65, "+|device|S7-1200 station_2|S71200.Device"},
    {66, "~|module|S7-1200 station_2 / ZZDELTA|name|PLC_1|ZZDELTA"},
    {66, "~|module|S7-1200 station_2 / ZZDELTA|access protection|users_and_roles|users_and_roles_and_access_levels"},
    {66, "~|module|S7-1200 station_2 / ZZDELTA|access control via access levels||yes"},
    {66, "~|hardware identifier|ZZDELTA / Default tag table / Local|stands for|S7-1200 station_2 / CPU proxy|S7-1200 station_2 /  ZZDELTA"},
    {69, "~|module|S7-1200 station_2 / ZZDELTA|access level|No access (complete protection)|Read access"},
    {70, "~|module|S7-1200 station_2 / ZZDELTA|access level|Read access|HMI access"},
    {71, "~|module|S7-1200 station_2 / ZZDELTA|access level|HMI access|Full access (no protection)"},
    {73, "~|project|s11_blocks|name|s10_connections|s11_blocks"},
    {74, "+|block|ZZBRAVO / ZZFC [FB3]|SCL"},
    {75, "+|block|ZZBRAVO / ZZCYCLIC [OB30]|CyclicInterrupt, LAD"},
    {75, "+|system constant|ZZBRAVO / Default tag table / OB_ZZCYCLIC|OB_Cyclic = 30"},
    {76, "~|block|ZZBRAVO / ZZFC [FB3]|folder|Program blocks|Program blocks/Group_1"},
    {77, "~|block|ZZBRAVO / ZZFC [FB77]|number|3|77"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|title||ZZTITLE"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|comment||ZZCOMMENT"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|author||ZZAUTH"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|family||ZZFAM"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|user-defined ID||ZZID"},
    {78, "~|block|ZZBRAVO / ZZFC [FB77]|version|0.1|1.2"},
    {79, "~|block|ZZBRAVO / DB_Standard [DB1]|title||ZZDBTITLE"},
    {79, "~|block|ZZBRAVO / DB_Standard [DB1]|comment||ZZDBCOMMENT"},
    {80, "~|block|ZZBRAVO / DB_Standard [DB1]|write-protected in the device|no|yes"},
};

// s12_constants: saves 3 to 9 are those of s11_blocks, 11 to 17 the constants.
const Step kStepsConstants[] = {
    {3, "~|block|ZZBRAVO / Block_2 [FB2]|copy protection||cpu"},
    {4, "~|block|ZZBRAVO / Block_1 [FB1]|protection|know-how|"},
    {5, "-|block|ZZBRAVO / ZZCYCLIC [OB30]|CyclicInterrupt, LAD"},
    {5, "-|system constant|ZZBRAVO / Default tag table / OB_ZZCYCLIC|OB_Cyclic = 30"},
    {6, "+|block|ZZBRAVO / ZZDB [DB5]|global, DB"},
    {7, "+|block|ZZCHARLIE / ZZDB2 [DB1]|global, DB"},
    {8, "~|block|ZZCHARLIE / ZZDB2 [DB1]|accessible from OPC UA||no"},
    {9, "~|block|ZZCHARLIE / ZZDB2 [DB1]|accessible via web server||no"},
    {11, "~|project|s12_constants|name|s11_blocks|s12_constants"},
    {12, "+|user constant|ZZBRAVO / Default tag table / ZZCONST_INT|Int = 42"},
    {13, "+|user constant|ZZBRAVO / Default tag table / ZZCONST_REAL|Real = 3.5"},
    {14, "+|user constant|ZZBRAVO / Tag MyTagTable / ZZCONST_TIME|Time = T#5s"},
    {15, "+|user constant|ZZBRAVO / Tag MyTagTable / ZZCONST_STR|String = 'Hello'"},
    {16, "~|user constant|ZZBRAVO / Default tag table / ZZCONST_INT|value|42|43"},
    {17, "-|user constant|ZZBRAVO / Default tag table / ZZCONST_REAL|Real = 3.5"},
};

std::string stepText(const tia::HistoryChange& c) {
    if (c.change == "changed") return "~|" + c.kind + "|" + c.item + "|" + c.attribute + "|" + c.from + "|" + c.to;
    return std::string(c.change == "added" ? "+" : "-") + "|" + c.kind + "|" + c.item + "|" + c.description;
}

bool followsFromSomethingElse(const tia::HistoryChange& c) {
    if (c.change != "changed") return !c.partOf.empty();
    return c.isTime || c.isMarker || c.isResult || c.attribute == "needs compiling";
}

tia::History history(const std::string& name, size_t throughSave = 0) {
    current = name + " history";
    tia::LoadedSource src = tia::loadProjectData(std::string(TIACONV_FIXTURES) + "/" + name);
    return tia::buildHistory(std::make_shared<const std::vector<uint8_t>>(std::move(src.data)), throughSave);
}

// The saves from `first` on show exactly the listed changes, no more.
template <size_t N>
void checkSteps(const tia::History& h, const Step (&steps)[N], size_t first, const std::set<size_t>& undocumented) {
    for (const auto& s : h.saves) {
        if (s.number < first || undocumented.count(s.number)) continue;
        std::multiset<std::string> expected, found;
        for (const Step& st : steps)
            if (st.save == s.number) expected.insert(st.change);
        for (const auto& c : s.changes)
            if (!followsFromSomethingElse(c)) found.insert(stepText(c));
        ++checks;
        if (expected != found) {
            ++failures;
            std::printf("FAIL %s: save %zu\n", current.c_str(), s.number);
            for (const auto& e : expected)
                if (!found.count(e)) std::printf("    missing: %s\n", e.c_str());
            for (const auto& f : found)
                if (!expected.count(f)) std::printf("    not expected: %s\n", f.c_str());
        }
    }
}

// First state plus everything added minus everything removed is what the
// project holds at the end, kind by kind.
void checkTotals(const tia::History& h, const Loaded& end) {
    std::map<std::string, long> n;
    for (const auto& s : h.saves) {
        for (const auto& kv : s.contents) n[kv.first] += static_cast<long>(kv.second);
        for (const auto& c : s.changes) {
            if (c.change == "added") ++n[c.kind];
            if (c.change == "removed") --n[c.kind];
        }
    }
    long devices = 0, modules = 0, interfaces = 0, blocks = 0, types = 0, user = 0, hardware = 0;
    for (const auto& d : end.inv.devices) {
        if (!d.inProject) continue;
        ++devices;
        modules += static_cast<long>(d.modules.size());
        for (const auto& m : d.modules) interfaces += static_cast<long>(m.interfaces.size());
    }
    for (const auto& b : end.prog.blockList) ++(b.type == "UDT" || b.type == "SDT" ? types : blocks);
    for (const auto& c : end.prog.constants) {
        if (c.kind == "user") ++user;
        if (c.kind == "hardware") ++hardware;
    }
    CHECK(n["device"] == devices);
    CHECK(n["module"] == modules);
    CHECK(n["interface"] == interfaces);
    CHECK(n["subnet"] == static_cast<long>(end.inv.subnets.size()));
    CHECK(n["IO system"] == static_cast<long>(end.inv.ioSystems.size()));
    CHECK(n["port connection"] == static_cast<long>(end.inv.portLinks.size()));
    CHECK(n["connection"] == static_cast<long>(end.inv.connections.size()));
    CHECK(n["block"] == blocks);
    CHECK(n["data type"] == types);
    CHECK(n["tag"] == static_cast<long>(end.prog.tags.size()));
    CHECK(n["user constant"] == user);
    CHECK(n["hardware identifier"] == hardware);
}

void testHistory() {
    {
        const tia::History h = history("s11_blocks_a");
        CHECK(h.savesInFile == 80 && h.saves.size() == 80);
        CHECK(h.notes.empty());
        // TIA Portal's own list: the project was created, nothing since
        CHECK(h.events.size() == 1);
        if (h.events.size() == 1) {
            CHECK(h.events[0].event == "ProjectHistoryUserCreated" && h.events[0].version == "V21");
            CHECK(h.events[0].date == "2026-10-05T13:00:10.135Z");
            CHECK(tia::projectEventText(h.events[0]) == "Project created with TIA Portal V21");
        }
        if (h.saves.size() == 80) {
            auto save = [&](size_t n) -> const tia::HistorySave& { return h.saves[n - 1]; };
            // a new project: a save with the type model only, then the empty project
            CHECK(save(1).beforeProject && save(1).objectsWritten == 0 && save(1).time.empty());
            CHECK(save(2).firstState && save(2).contents.empty() && save(2).objectsWritten == 344);
            CHECK(save(2).time == "2026-10-05T13:00:10.683Z" && save(2).timeSource == "latest_change");
            CHECK(save(2).by == "PC");
            // the PLC was renamed: the save did not write the project object,
            // the time and the user are those of the objects it did write
            CHECK(save(5).time == "2026-10-05T13:03:42.678Z" && save(5).by == "PC");
            CHECK(save(5).objectsWritten == 9 && save(5).objectsDeleted == 0);
            // a router address: the three objects written name no user
            CHECK(save(9).time == "2026-10-05T13:10:05.707Z" && save(9).by.empty());
            // the tags of save 17 were entered over six minutes; the project
            // object in the same save has an earlier time than the last of them
            CHECK(save(17).time == "2026-10-05T17:57:32.278Z" && save(17).by == "PC");
            // saved without a change
            for (size_t n : {27u, 52u, 72u}) {
                CHECK(save(n).changes.empty() && save(n).objectsWritten == 1 && save(n).time.empty());
                CHECK(save(n).objectTypes.size() == 1 && save(n).objectTypes[0].type == "CorePersistenceInfo");
            }
            // a password entered, a role created: written, and nothing to report
            CHECK(save(67).changes.empty() && save(67).objectsWritten == 3);
            CHECK(save(68).changes.empty() && save(68).objectsWritten == 6);
            bool role = false;
            for (const auto& t : save(68).objectTypes)
                if (t.type == "CustomRole" && t.written == 1) role = true;
            CHECK(role);
            // a station comes with its parts
            size_t parts = 0;
            for (const auto& c : save(3).changes)
                if (c.change == "added" && c.partOf == "S7-1200 station_1") ++parts;
            CHECK(parts == 2);  // rack and CPU; the interface is part of the CPU
            CHECK(save(64).objectsDeleted == 2);  // the two halves of the S7 connection
            // every save that changed something has a time, and they are in order
            std::string last;
            bool ordered = true, complete = true;
            for (const auto& s : h.saves) {
                if (s.time.empty()) {
                    if (!s.changes.empty()) complete = false;
                    continue;
                }
                if (s.time <= last) ordered = false;
                last = s.time;
            }
            CHECK(ordered && complete && save(80).time == "2026-10-06T20:47:12.487Z");
        }
        checkSteps(h, kStepsBlocksA, 3, {17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27});
        checkTotals(h, load("s11_blocks_a"));

        // the history up to a save is the start of the whole history
        const tia::History part = history("s11_blocks_a", 51);
        CHECK(part.savesInFile == 80 && part.saves.size() == 51 && part.notes.size() == 1);
        bool same = part.saves.size() <= h.saves.size();
        for (size_t i = 0; same && i < part.saves.size(); ++i) {
            same = part.saves[i].changes.size() == h.saves[i].changes.size() && part.saves[i].time == h.saves[i].time;
            for (size_t k = 0; same && k < part.saves[i].changes.size(); ++k)
                same = stepText(part.saves[i].changes[k]) == stepText(h.saves[i].changes[k]);
        }
        CHECK(same);
    }
    {
        // TIA Portal wrote this file anew: its first save holds the type
        // model, the second the whole project as it was then
        const tia::History h = history("s12_constants");
        CHECK(h.savesInFile == 17 && h.saves.size() == 17);
        CHECK(h.notes.size() == 1 && h.notes[0].find("starts at save 2") != std::string::npos);
        if (h.saves.size() == 17) {
            CHECK(h.saves[0].beforeProject);
            CHECK(h.saves[1].firstState && h.saves[1].objectsWritten == 1430);
            std::map<std::string, size_t> contents(h.saves[1].contents.begin(), h.saves[1].contents.end());
            CHECK(contents["device"] == 6 && contents["block"] == 12 && contents["tag"] == 12);
            CHECK(h.saves[9].changes.empty() && h.saves[9].objectsWritten == 1);  // saved without a change
            // the file on the PC was last written at 20:44:27.140 UTC
            CHECK(h.saves[16].time == "2026-10-07T20:44:27.109Z" && h.saves[16].by == "PC");
            CHECK(h.saves[16].objectsWritten == 7 && h.saves[16].objectsDeleted == 1);
        }
        checkSteps(h, kStepsConstants, 3, {});
        checkTotals(h, load("s12_constants"));
    }
    for (const char* name : {"s00_empty", "s07_second", "s08_program", "s09_security", "s10_connections", "s11_blocks"}) {
        const tia::History h = history(name);
        CHECK(h.saves.size() == h.savesInFile && !h.saves.empty());
        bool problems = false;
        for (const auto& s : h.saves)
            if (!s.problem.empty() || s.afterLastSave) problems = true;
        CHECK(!problems);
        checkTotals(h, load(name));
    }
}

}  // namespace

int main() {
    {
        Loaded l = load("s00_empty");
        common(l, "s00_empty");
        CHECK(l.inv.devices.empty());
        CHECK(l.inv.subnets.empty());
    }
    {
        Loaded l = load("s01_cpu");
        common(l, "s01_cpu");
        CHECK(l.inv.devices.size() == 1 && l.inv.devices[0].inProject);
        const tia::Module* cpu = module(l.inv, "PLC_1");
        CHECK(cpu != nullptr);
        if (cpu) {
            CHECK(cpu->kind == "controller");
            CHECK(cpu->typeName == "CPU 1212C AC/DC/Rly");
            CHECK(cpu->orderNumber == "6ES7 212-1BD30-0XB0");
            CHECK(cpu->firmware == "V2.2");
            CHECK(cpu->hasPosition && cpu->position == 1);
            CHECK(cpu->container == "Rack_0");
        }
        const tia::Interface* i = firstInterface(l.inv, "PLC_1");
        CHECK(i != nullptr);
        if (i) {
            CHECK(i->ip == "192.168.0.1" && i->mask == "255.255.255.0");
            CHECK(i->hasIpSetByUser && !i->ipSetByUser);  // still TIA's default
            CHECK(!i->ipAssignedElsewhere);
            CHECK(i->router.empty());
            CHECK(i->profinetNameAuto && i->profinetName.empty());
            CHECK(i->subnet.empty());
        }
    }
    {
        Loaded l = load("s02_name");
        common(l, "s02_name");
        CHECK(module(l.inv, "ZZALPHA") != nullptr);
        CHECK(module(l.inv, "PLC_1") == nullptr);
        // The stored PROFINET name does not follow the rename while it is automatic.
        const tia::Interface* i = firstInterface(l.inv, "ZZALPHA");
        CHECK(i && i->profinetNameAuto && i->profinetName.empty() && i->profinetNameStored == "plc_1");
    }
    {
        Loaded l = load("s03_ip");
        common(l, "s03_ip");
        const tia::Interface* i = firstInterface(l.inv, "ZZALPHA");
        CHECK(i != nullptr);
        if (i) {
            CHECK(i->ip == "192.168.77.11" && i->mask == "255.255.255.0");
            CHECK(i->hasIpSetByUser && i->ipSetByUser);
            CHECK(i->router.empty());
            CHECK(i->subnet == "PN/IE_1");
        }
        CHECK(l.inv.subnets.size() == 1);
        if (l.inv.subnets.size() == 1) {
            CHECK(l.inv.subnets[0].name == "PN/IE_1" && l.inv.subnets[0].netType == 3);
            CHECK(l.inv.subnets[0].members.size() == 1 && l.inv.subnets[0].members[0].ip == "192.168.77.11");
        }
    }
    {
        Loaded l = load("s04_router");
        common(l, "s04_router");
        const tia::Interface* i = firstInterface(l.inv, "ZZALPHA");
        CHECK(i && i->ip == "192.168.77.11" && i->router == "192.168.77.1");
    }
    {
        Loaded l = load("s05_pnname");
        common(l, "s05_pnname");
        const tia::Interface* i = firstInterface(l.inv, "ZZALPHA");
        CHECK(i && !i->profinetNameAuto && i->profinetName == "zzalpha-pn");
    }
    {
        Loaded l = load("s06_module");
        common(l, "s06_module");
        CHECK(l.inv.stats.deletedObjects == 4);
        const tia::Module* sm = module(l.inv, "DI 8x24VDC_1");
        CHECK(sm != nullptr);
        if (sm) {
            CHECK(sm->kind == "module");
            CHECK(sm->orderNumber == "6ES7 221-1BF30-0XB0");
            CHECK(sm->firmware == "V1.0");
            CHECK(sm->hasPosition && sm->position == 2);
            CHECK(sm->container == "Rack_0");
        }
    }
    {
        Loaded l = load("s07_second");
        common(l, "s07_second");
        CHECK(l.inv.devices.size() == 2);
        const tia::Module* a = module(l.inv, "ZZALPHA");
        const tia::Module* b = module(l.inv, "ZZBRAVO");
        CHECK(a != nullptr && b != nullptr);
        if (b) {
            CHECK(b->typeName == "CPU 1511-1 PN");
            CHECK(b->orderNumber == "6ES7 511-1AK00-0AB0");
            CHECK(b->firmware == "V1.8");
        }
        const tia::Interface* ia = firstInterface(l.inv, "ZZALPHA");
        const tia::Interface* ib = firstInterface(l.inv, "ZZBRAVO");
        CHECK(ia && ia->ip == "192.168.77.11" && ia->router == "192.168.77.1" && ia->profinetName == "zzalpha-pn" &&
              ia->subnet == "PN/IE_1");
        CHECK(ib && ib->ip == "192.168.77.12" && ib->mask == "255.255.255.0" && ib->router.empty());
        CHECK(ib && ib->subnet.empty());  // ZZBRAVO was not attached to the subnet
        CHECK(l.inv.subnets.size() == 1 && l.inv.subnets[0].members.size() == 1);
    }
    {
        // s08_program: tags, data blocks, function blocks. What is checked
        // against the tag table export and the block source that TIA Portal
        // wrote for this project (tests/fixtures/s08_program/exports) is
        // marked "export"; "screenshot" is what TIA Portal displayed: the
        // project tree, the block calls in OB1, and the editors of DB1 and
        // DB2 with the Offset and Start value columns.
        Loaded l = load("s08_program");
        common(l, "s08");
        CHECK(l.inv.devices.size() == 2);
        const tia::ProgramData& p = l.prog;

        // The list of blocks. Screenshot of the project tree: Main [OB1],
        // Block_1 [FB1], Block_2 [FB2], DB_Standard [DB1], DB_Optimized [DB2],
        // Block_1_DB [DB3], Block_2_DB [DB4], and one PLC data type. Block_2
        // is the block TIA Portal generated a statement list source for.
        {
            current = "s08: list of blocks";
            const struct { const char* plc; const char* type; int64_t number; const char* name; const char* kind;
                           const char* instanceOf; const char* language; const char* folder; } want[] = {
                {"ZZALPHA", "OB", 1, "Main", "ProgramCycle", "", "LAD", "Program blocks"},
                {"ZZBRAVO", "OB", 1, "Main", "ProgramCycle", "", "LAD", "Program blocks"},
                {"ZZBRAVO", "FB", 1, "Block_1", "", "", "FBD", "Program blocks"},
                {"ZZBRAVO", "FB", 2, "Block_2", "", "", "STL", "Program blocks"},
                {"ZZBRAVO", "DB", 1, "DB_Standard", "global", "", "DB", "Program blocks"},
                {"ZZBRAVO", "DB", 2, "DB_Optimized", "global", "", "DB", "Program blocks"},
                {"ZZBRAVO", "DB", 3, "Block_1_DB", "instance", "Block_1", "DB", "Program blocks"},
                {"ZZBRAVO", "DB", 4, "Block_2_DB", "instance", "Block_2", "DB", "Program blocks"},
                {"ZZBRAVO", "UDT", 1, "User_data_type_1", "", "", "UDT", "PLC data types"}};
            const size_t n = sizeof want / sizeof want[0];
            CHECK(p.blockList.size() == n);
            for (size_t k = 0; k < n && k < p.blockList.size(); ++k) {
                const tia::BlockInfo& b = p.blockList[k];
                current = std::string("s08: list of blocks, ") + want[k].plc + " " + want[k].name;
                CHECK(b.plc == want[k].plc && b.type == want[k].type && b.hasNumber && b.number == want[k].number);
                CHECK(b.name == want[k].name && b.kind == want[k].kind && b.instanceOf == want[k].instanceOf);
                CHECK(b.language == want[k].language && b.folder == want[k].folder && !b.system);
                // nothing is protected, nothing was ever downloaded
                CHECK(b.protection.empty() && b.protectionStored == "NoProtection" && b.copyProtection.empty());
                CHECK(b.downloaded.empty() && b.downloads.empty());
                CHECK(b.created.size() == 24 && b.created.compare(0, 11, "2026-10-05T") == 0 && !b.modified.empty());
            }
            // the generated source of Block_2: VERSION : 0.1, optimized access
            if (p.blockList.size() == n) {
                const tia::BlockInfo& b2 = p.blockList[3];
                CHECK(b2.version == "0.1" && b2.author.empty() && b2.hasAccess && b2.symbolicAccessOnly);
                CHECK(p.blockList[4].hasAccess && !p.blockList[4].symbolicAccessOnly);
            }
        }

        // export: PLCTags.xlsx, all eight rows of "Tag MyTagTable"
        const struct { const char* name; const char* type; const char* address; const char* comment; } tags[] = {
            {"Mybool", "Bool", "%I0.0", "This is a bolean"}, {"Mysecondbool", "Bool", "%Q0.1", ""},
            {"MyByte", "Byte", "%MB10", ""},                 {"MyInt", "Word", "%MW20", ""},
            {"MyReal", "Real", "%MD22", ""},                 {"MyDWord", "DWord", "%MD26", ""},
            {"MyTime", "Time", "%MD30", ""},                 {"MyChar", "Char", "%MB34", "This is a char"}};
        size_t inTable = 0, inDefault = 0;
        for (const auto& t : p.tags) {
            CHECK(t.plc == "ZZBRAVO");
            if (t.table == "Tag MyTagTable") ++inTable;
            if (t.table == "Default tag table") ++inDefault;
        }
        CHECK(inTable == 8);  // screenshot: "Tag MyTagTable [8]"
        // screenshot: "Default tag table [49]" = these 4 tags + 45 system constants
        CHECK(inDefault == 4 && p.tags.size() == 12);
        {
            current = "s08: constants";
            size_t bravo = 0, hardware = 0, user = 0;
            for (const auto& c : p.constants) {
                if (c.plc != "ZZBRAVO") continue;
                ++bravo;
                CHECK(c.system && c.table == "Default tag table");
                if (c.kind == "hardware") ++hardware;
                if (c.kind == "user") ++user;
            }
            CHECK(bravo == 45 && hardware == 10 && user == 0);
        }
        for (const auto& want : tags) {
            const tia::Tag* found = nullptr;
            for (const auto& t : p.tags)
                if (t.name == want.name) found = &t;
            CHECK(found != nullptr);
            if (found)
                CHECK(found->table == "Tag MyTagTable" && found->dataType == want.type &&
                      found->address == want.address && found->comment == want.comment);
        }

        // screenshot: block numbers in the project tree
        CHECK(p.blocks.size() == 4);
        if (p.blocks.size() == 4) {
            const tia::DataBlock& std1 = p.blocks[0];
            const tia::DataBlock& opt = p.blocks[1];
            const tia::DataBlock& i1 = p.blocks[2];
            const tia::DataBlock& i2 = p.blocks[3];
            CHECK(std1.name == "DB_Standard" && std1.number == 1 && std1.kind == "global");
            CHECK(opt.name == "DB_Optimized" && opt.number == 2 && opt.kind == "global");
            CHECK(i1.name == "Block_1_DB" && i1.number == 3 && i1.kind == "instance" && i1.instanceOf == "Block_1");
            CHECK(i2.name == "Block_2_DB" && i2.number == 4 && i2.kind == "instance" && i2.instanceOf == "Block_2");
            // the two global blocks were made to differ in this one setting
            CHECK(std1.hasAccess && !std1.symbolicAccessOnly);
            CHECK(opt.hasAccess && opt.symbolicAccessOnly);

            // export: source_from_blocks.awl, FUNCTION_BLOCK "Block_2" and its
            // instance block ({ S7_Optimized_Access := 'TRUE' })
            CHECK(i2.symbolicAccessOnly && i2.members.size() == 4);
            if (i2.members.size() == 4) {
                const struct { const char* name; const char* type; const char* section; const char* value; const char* comment; } m[] = {
                    {"MB12", "Byte", "Input", "16#3", "My input byte"},
                    {"MB14", "Byte", "Output", "16#4", "My output byte"},
                    {"MB15", "Byte", "InOut", "", "My In/Out byte"},
                    {"666", "DInt", "Static", "666", "My Static"}};
                for (size_t k = 0; k < 4; ++k) {
                    const tia::BlockMember& g = i2.members[k];
                    CHECK(g.name == m[k].name && g.dataType == m[k].type && g.section == m[k].section);
                    CHECK(g.startValue == m[k].value && g.hasStartValue == (m[k].value[0] != 0));
                    CHECK(g.comment == m[k].comment && !g.hasOffset);
                }
            }
            // screenshot: the call of Block_1 in OB1 shows its parameters and
            // the default 16#7 of my_inout
            CHECK(i1.members.size() == 4);
            if (i1.members.size() == 4) {
                CHECK(i1.members[0].name == "M12.1" && i1.members[0].section == "Input");
                CHECK(i1.members[1].name == "M13.1" && i1.members[1].section == "Output");
                CHECK(i1.members[2].name == "my_inout" && i1.members[2].section == "InOut" &&
                      i1.members[2].startValue == "16#7");
            }

            // screenshot: the editor of DB_Standard, every row. TIA Portal
            // shows offsets as byte.bit for all types (2.0, 258.0); tiaconv
            // prints the bit only for Bool. The members of the PLC data type
            // show its defaults; memberBolean has none of its own.
            const struct { const char* name; const char* type; uint64_t byte; const char* value; } rows[] = {
                {"my_data_type", "\"User_data_type_1\"", 0, ""},
                {"my_bool", "Bool", 262, "true"},
                {"my_byte", "Byte", 263, "16#4"},
                {"my_int", "Int", 264, "5"},
                {"my_dint", "DInt", 266, "6"},
                {"my_real", "Real", 270, "3.141593"},
                {"my_stringof10", "Array[0..10] of String", 274, ""},
                {"my_array_of_int", "Array[0..3] of Int", 3090, ""}};
            const struct { const char* name; const char* type; uint64_t byte; const char* value; } udt[] = {
                {"memberDate", "Date", 0, "D#1990-01-31"},
                {"memberString", "String", 2, "'Hello world'"},
                {"memberInt", "Int", 258, "7"},
                {"memberBolean", "Bool", 260, ""}};
            CHECK(std1.members.size() == 8 && std1.memberCount == 12);
            CHECK(opt.members.size() == 8 && opt.memberCount == 12);
            for (const tia::DataBlock* b : {&std1, &opt}) {
                if (b->members.size() != 8) continue;
                const bool standard = b == &std1;
                for (size_t k = 0; k < 8; ++k) {
                    const tia::BlockMember& g = b->members[k];
                    // screenshot of DB_Optimized: same members, one of them
                    // spelled differently, no offset column
                    const std::string name = !standard && k == 6 ? "my_sitringof10" : rows[k].name;
                    CHECK(g.name == name && g.dataType == rows[k].type && g.startValue == rows[k].value);
                    CHECK(g.hasOffset == standard);
                    if (standard) CHECK(g.offsetBits == rows[k].byte * 8);
                }
                const auto& inner = b->members[0].members;
                CHECK(inner.size() == 4);
                for (size_t k = 0; k < inner.size() && k < 4; ++k) {
                    CHECK(inner[k].name == udt[k].name && inner[k].dataType == udt[k].type);
                    CHECK(inner[k].startValue == udt[k].value && inner[k].hasOffset == standard);
                    if (standard) CHECK(inner[k].offsetBits == udt[k].byte * 8);
                }
            }
        }
    }
    {
        // s09_security: one project, one security setting changed per save
        // (see tests/fixtures/README.md). Every row is the state after one
        // save, i.e. after one known action in TIA Portal V21.
        using S = tia::Security;
        struct Step {
            size_t save;
            const char* plc;
            const char* what;
            bool (*holds)(const S&);
        };
        const Step steps[] = {
            {28, "ZZBRAVO", "before any change: nothing stored",
             [](const S& s) { return s.empty(); }},
            {29, "ZZBRAVO", "PUT/GET ticked",
             [](const S& s) { return s.putGet.stored && s.putGet.on && !s.hasAccessLevel; }},
            {30, "ZZBRAVO", "Read access",
             [](const S& s) { return s.accessLevel == 2 && s.accessLevelName == "Read access" && s.putGet.on; }},
            {31, "ZZBRAVO", "HMI access",
             [](const S& s) { return s.accessLevel == 3 && s.accessLevelName == "HMI access"; }},
            {32, "ZZBRAVO", "No access; TIA Portal switched PUT/GET off with it",
             [](const S& s) {
                 return s.accessLevel == 4 && s.accessLevelName == "No access (complete protection)" &&
                        s.putGet.stored && !s.putGet.on;
             }},
            {33, "ZZBRAVO", "back to Full access",
             [](const S& s) { return s.accessLevel == 1 && s.accessLevelName == "Full access (no protection)"; }},
            {34, "ZZBRAVO", "HMI access again, PUT/GET on again",
             [](const S& s) { return s.accessLevel == 3 && s.putGet.stored && s.putGet.on; }},
            {35, "ZZBRAVO", "web server access ticked on interface X1 only",
             [](const S& s) { return s.webServerInterfaces == std::vector<std::string>{"X1"} && !s.webServer.stored; }},
            {36, "ZZBRAVO", "... and unticked",
             [](const S& s) { return s.webServerInterfaces.empty() && !s.webServer.stored; }},
            {37, "ZZBRAVO", "web server activated on the module; TIA Portal ticked the interface with it",
             [](const S& s) { return s.webServer.stored && s.webServer.on && s.webServerInterfaces.size() == 1; }},
            {38, "ZZBRAVO", "interface unticked",
             [](const S& s) { return s.webServer.on && s.webServerInterfaces.empty(); }},
            {39, "ZZBRAVO", "interface ticked",
             [](const S& s) { return s.webServer.on && s.webServerInterfaces.size() == 1 && !s.webServerHttpsOnly.stored; }},
            {40, "ZZBRAVO", "HTTPS only",
             [](const S& s) { return s.webServerHttpsOnly.stored && s.webServerHttpsOnly.on && !s.hasTimeSyncRole; }},
            {41, "ZZBRAVO", "NTP with server 192.168.77.50",
             [](const S& s) {
                 return s.timeSyncRole == 2 && s.ntpServers == std::vector<std::string>{"192.168.77.50"} &&
                        !s.displayProtection.stored;
             }},
            {42, "ZZBRAVO", "display protection",
             [](const S& s) { return s.displayProtection.stored && s.displayProtection.on && !s.opcUaServer.stored; }},
            {42, "ZZALPHA", "before any change: nothing stored",
             [](const S& s) { return s.empty(); }},
            {43, "ZZALPHA", "Write protection",
             [](const S& s) { return s.accessLevel == 2 && s.accessLevelName == "Write protection" && !s.webServer.stored; }},
            {44, "ZZALPHA", "web server activated",
             [](const S& s) { return s.webServer.stored && s.webServer.on && s.webServerInterfaces.empty(); }},
            {45, "ZZALPHA", "Write/read protection",
             [](const S& s) { return s.accessLevel == 3 && s.accessLevelName == "Write/read protection"; }},
            // ZZCHARLIE, firmware V4.1, as the security wizard left it: its
            // overview page said protection of confidential data enabled,
            // legacy access protection "No access", and listed secure
            // communication and access control, which are defaults and so
            // not stored.
            {46, "ZZCHARLIE", "added with the wizard's defaults",
             [](const S& s) {
                 return s.accessLevel == 4 && s.accessLevelName == "No access (complete protection)" &&
                        s.putGet.stored && !s.putGet.on && s.configDataProtection.stored && s.configDataProtection.on &&
                        !s.accessControl.stored && !s.hasCommunicationMode && !s.opcUaServer.stored;
             }},
            {47, "ZZCHARLIE", "OPC UA server activated",
             [](const S& s) { return s.opcUaServer.stored && s.opcUaServer.on && !s.hasCommunicationMode; }},
            {48, "ZZCHARLIE", "\"only secure communication\" unticked",
             [](const S& s) { return s.hasCommunicationMode && s.communicationMode == 0 && s.configDataProtection.on; }},
            {49, "ZZCHARLIE", "protection of confidential configuration data unticked",
             [](const S& s) { return s.configDataProtection.stored && !s.configDataProtection.on && !s.accessControl.stored; }},
            {50, "ZZCHARLIE", "access control disabled",
             [](const S& s) { return s.accessControl.stored && !s.accessControl.on; }},
            {51, "ZZCHARLIE", "access control enabled, via access levels",
             [](const S& s) {
                 return s.accessControl.stored && s.accessControl.on && s.accessControlViaAccessLevels.stored &&
                        s.accessControlViaAccessLevels.on && s.accessLevel == 4;
             }},
        };
        size_t loadedSave = 0;
        Loaded l;
        for (const Step& st : steps) {
            if (st.save != loadedSave) {
                l = load("s09_security", st.save);
                loadedSave = st.save;
                CHECK(l.hashErrors == 0 && l.inv.stats.objectsWithProblems == 0);
            }
            current = "s09_security after save " + std::to_string(st.save) + ", " + st.plc + ": " + st.what;
            const tia::Module* m = module(l.inv, st.plc);
            CHECK(m != nullptr && m->kind == "controller");
            if (m) CHECK(st.holds(m->security));
        }
        // ZZCHARLIE does not exist before the save that added it
        l = load("s09_security", 45);
        CHECK(module(l.inv, "ZZCHARLIE") == nullptr && l.inv.devices.size() == 2);
        // the whole file: 51 saves, three stations, and the new CPU's internal
        // interface without an address is not listed
        l = load("s09_security");
        CHECK(l.saves == 51 && l.inv.devices.size() == 3);
        const tia::Module* c = module(l.inv, "ZZCHARLIE");
        CHECK(c && c->firmware == "V4.1" && c->orderNumber == "6ES7 511-1AL03-0AB0" && c->interfaces.size() == 1);
        CHECK(module(l.inv, "Virtual CP interface port_1") == nullptr);

        // What decides about access. Only the newest CPU has user management;
        // on it the access level is not what protects the CPU.
        current = "s09_security: what decides about access";
        auto protection = [&](size_t save, const char* plc) {
            Loaded at = load("s09_security", save);
            const tia::Module* m = module(at.inv, plc);
            return m ? tia::accessProtection(m->security) : std::string("?");
        };
        CHECK(protection(51, "ZZALPHA") == "access_levels" && protection(51, "ZZBRAVO") == "access_levels");
        CHECK(protection(28, "ZZBRAVO") == "access_levels");  // before anything was set
        CHECK(protection(46, "ZZCHARLIE") == "users_and_roles");
        CHECK(protection(50, "ZZCHARLIE") == "none");
        CHECK(protection(51, "ZZCHARLIE") == "users_and_roles_and_access_levels");
        const tia::Module* a = module(l.inv, "ZZALPHA");
        const tia::Module* b = module(l.inv, "ZZBRAVO");
        CHECK(a && b && c && !a->security.userManagement && !b->security.userManagement && c->security.userManagement);
        CHECK(c && c->security.functionRightSet == "PlcDeviceFunctionRights.UmacFunctionRights_S71500V41");
    }
    {
        // s10_connections: network, IO systems, port cabling and connections,
        // again one change per save (see tests/fixtures/README.md).
        using I = tia::Inventory;
        auto subnetMember = [](const I& v, const std::string& module) {
            for (const auto& s : v.subnets)
                for (const auto& m : s.members)
                    if (m.module == module) return true;
            return false;
        };
        auto ioOf = [](const I& v, const std::string& controller) -> const tia::IoSystem* {
            for (const auto& s : v.ioSystems)
                if (s.controller == controller) return &s;
            return nullptr;
        };
        auto conn = [](const I& v, const std::string& name) -> const tia::Connection* {
            for (const auto& c : v.connections)
                if (c.name == name) return &c;
            return nullptr;
        };
        struct Step {
            size_t save;
            const char* what;
            bool (*holds)(const I&, decltype(subnetMember)&, decltype(ioOf)&, decltype(conn)&);
        };
        const Step steps[] = {
            {53, "start: only ZZALPHA on the subnet, nothing else configured",
             [](const I& v, auto& onNet, auto&, auto&) {
                 return v.devices.size() == 3 && onNet(v, "ZZALPHA") && !onNet(v, "ZZBRAVO") && !onNet(v, "ZZCHARLIE") &&
                        v.ioSystems.empty() && v.portLinks.empty() && v.connections.empty();
             }},
            {54, "ZZBRAVO attached to PN/IE_1",
             [](const I& v, auto& onNet, auto&, auto&) { return onNet(v, "ZZBRAVO") && !onNet(v, "ZZCHARLIE"); }},
            {55, "ZZCHARLIE: address 192.168.77.13, attached to PN/IE_1",
             [](const I& v, auto& onNet, auto&, auto&) {
                 const tia::Interface* i = firstInterface(v, "ZZCHARLIE");
                 return onNet(v, "ZZCHARLIE") && i && i->ip == "192.168.77.13" && v.devices.size() == 3;
             }},
            {56, "ET 200SP station ZZIO1 added, not assigned: listed as a device of the project, on no subnet",
             [](const I& v, auto& onNet, auto&, auto&) {
                 const tia::Module* m = module(v, "IO device_1");
                 return v.devices.size() == 4 && v.devices[3].name == "ZZIO1" && v.devices[3].inProject && m &&
                        m->orderNumber == "6ES7 155-6AU02-0BN0" && m->firmware == "V6.4" && m->ioController.empty() &&
                        !onNet(v, "IO device_1") && v.ioSystems.empty();
             }},
            {57, "ZZIO1 assigned to ZZBRAVO: an IO system with one device; TIA Portal gave it 192.168.77.1",
             [](const I& v, auto& onNet, auto& io, auto&) {
                 const tia::IoSystem* s = io(v, "ZZBRAVO");
                 const tia::Module* m = module(v, "IO device_1");
                 return v.ioSystems.size() == 1 && s && s->name == "PROFINET IO-System" && s->hasNumber &&
                        s->number == 100 && s->subnet == "PN/IE_1" && s->devices.size() == 1 &&
                        s->devices[0].device == "ZZIO1" && s->devices[0].module == "IO device_1" &&
                        s->devices[0].ip == "192.168.77.1" && onNet(v, "IO device_1") && m && m->ioController == "ZZBRAVO";
             }},
            {58, "second station added and assigned to ZZCHARLIE: a second IO system",
             [](const I& v, auto&, auto& io, auto&) {
                 const tia::IoSystem* b = io(v, "ZZBRAVO");
                 const tia::IoSystem* c = io(v, "ZZCHARLIE");
                 return v.devices.size() == 5 && v.ioSystems.size() == 2 && b && b->devices.size() == 1 && c &&
                        c->number == 100 && c->devices.size() == 1 && c->devices[0].module == "IO device_2" &&
                        c->devices[0].ip == "192.168.77.2";
             }},
            {59, "second station reassigned to ZZBRAVO: ZZCHARLIE's IO system stays, empty",
             [](const I& v, auto&, auto& io, auto&) {
                 const tia::IoSystem* b = io(v, "ZZBRAVO");
                 const tia::IoSystem* c = io(v, "ZZCHARLIE");
                 const tia::Module* m = module(v, "IO device_2");
                 return b && b->devices.size() == 2 && b->devices[1].module == "IO device_2" && c && c->devices.empty() &&
                        m && m->ioController == "ZZBRAVO" && v.portLinks.empty();
             }},
            {60, "cable ZZBRAVO port 1 - IO device_1 port 1",
             [](const I& v, auto&, auto&, auto&) {
                 if (v.portLinks.size() != 1) return false;
                 const tia::PortLink& l = v.portLinks[0];
                 return l.a.module == "ZZBRAVO" && l.a.port == "X1 P1" && l.b.device == "ZZIO1" &&
                        l.b.module == "IO device_1" && l.b.port == "X1 P1R";
             }},
            {61, "cable IO device_1 port 2 - IO device_2 port 1",
             [](const I& v, auto&, auto&, auto&) {
                 if (v.portLinks.size() != 2) return false;
                 const tia::PortLink& l = v.portLinks[1];
                 return l.a.module == "IO device_1" && l.a.port == "X1 P2R" && l.b.module == "IO device_2" &&
                        l.b.port == "X1 P1R" && v.connections.empty();
             }},
            // The Connections table of TIA Portal showed for this one: local
            // end point ZZBRAVO, local ID 100 (hex), partner ID 100, partner
            // ZZCHARLIE, and the same from ZZCHARLIE's side.
            {62, "S7 connection ZZBRAVO - ZZCHARLIE: two halves, reported once",
             [](const I& v, auto&, auto&, auto& cn) {
                 const tia::Connection* c = cn(v, "S7_Connection_1");
                 return v.connections.size() == 1 && c && c->kind == "S7" && c->bothSides &&
                        c->local.module == "ZZBRAVO" && c->local.interface == "X1" && c->local.ip == "192.168.77.12" &&
                        c->partner.module == "ZZCHARLIE" && c->partner.ip == "192.168.77.13" && c->localId == 0x100 &&
                        c->hasPartnerId && c->partnerId == 0x100 && c->oneWay.stored && !c->oneWay.on &&
                        c->partnerAddress.empty();
             }},
            // ... and for this one: local ID 101 (hex), partner "Unspecified".
            {63, "S7 connection from ZZBRAVO to a partner outside the project, 192.168.77.99",
             [](const I& v, auto&, auto&, auto& cn) {
                 const tia::Connection* c = cn(v, "S7_Connection_2");
                 return v.connections.size() == 2 && c && c->kind == "S7" && !c->bothSides &&
                        c->local.module == "ZZBRAVO" && c->partner.device.empty() && c->partner.module.empty() &&
                        c->partnerAddress == "192.168.77.99" && c->localId == 0x101 && c->oneWay.on &&
                        c->activeEstablishment.on;
             }},
            {64, "S7_Connection_1 deleted",
             [](const I& v, auto&, auto&, auto& cn) {
                 return v.connections.size() == 1 && !cn(v, "S7_Connection_1") && cn(v, "S7_Connection_2") &&
                        v.ioSystems.size() == 2 && v.portLinks.size() == 2;
             }},
            // An S7-1200 with firmware V4.7. TIA Portal showed "Enable access
            // control" selected, "Use access control via access levels" not
            // ticked, and the access level greyed out at "No access
            // (complete protection)".
            {65, "S7-1200 with firmware V4.7 added: user management, access level 4 shown as No access",
             [](const I& v, auto& onNet, auto&, auto&) {
                 const tia::Module* m = module(v, "PLC_1");
                 return v.devices.size() == 6 && m && m->kind == "controller" && m->firmware == "V4.7" &&
                        m->orderNumber == "6ES7 214-1BG40-0XB0" && !onNet(v, "PLC_1") && m->security.userManagement &&
                        m->security.functionRightSet == "PlcDeviceFunctionRights.S71200V4_7_1" &&
                        !m->security.accessControl.stored && tia::accessProtection(m->security) == "users_and_roles" &&
                        m->security.accessLevel == 4 &&
                        m->security.accessLevelName == "No access (complete protection)";
             }},
            {66, "renamed to ZZDELTA; \"Use access control via access levels\" ticked",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 return !module(v, "PLC_1") && m && m->security.accessControlViaAccessLevels.on &&
                        tia::accessProtection(m->security) == "users_and_roles_and_access_levels" &&
                        m->security.accessLevel == 4;
             }},
            // The level cannot be chosen on this CPU: it follows from the
            // rights of the Anonymous user. Entering passwords changes
            // nothing that tiaconv reads.
            {67, "a password entered on the access control page: nothing else changes",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 return m && m->security.accessLevel == 4 && !m->security.accessControl.stored &&
                        tia::accessProtection(m->security) == "users_and_roles_and_access_levels" &&
                        v.connections.size() == 1 && v.ioSystems.size() == 2;
             }},
            // Users and roles: a role with the right "Read access" on
            // ZZDELTA, then the role given to the Anonymous user. Only then
            // did the greyed-out access level in TIA Portal move to "Read
            // access", and the stored level with it.
            {68, "role with the right Read access on ZZDELTA created: nothing changes yet",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 return m && m->security.accessLevel == 4;
             }},
            {69, "role assigned to the Anonymous user: access without login is Read access",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 return m && m->security.accessLevel == 2 && m->security.accessLevelName == "Read access";
             }},
            {70, "right of the role changed to HMI access",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 return m && m->security.accessLevel == 3 && m->security.accessLevelName == "HMI access";
             }},
            {71, "right of the role changed to Full access",
             [](const I& v, auto&, auto&, auto&) {
                 const tia::Module* m = module(v, "ZZDELTA");
                 const tia::Module* c = module(v, "ZZCHARLIE");
                 return m && m->security.accessLevel == 1 && m->security.accessLevelName == "Full access (no protection)" &&
                        tia::accessProtection(m->security) == "users_and_roles_and_access_levels" && c &&
                        c->security.accessLevel == 4;
             }},
        };
        for (const Step& st : steps) {
            Loaded l = load("s10_connections", st.save);
            CHECK(l.hashErrors == 0 && l.inv.stats.objectsWithProblems == 0);
            current = "s10_connections after save " + std::to_string(st.save) + ": " + st.what;
            CHECK(st.holds(l.inv, subnetMember, ioOf, conn));
        }
        current = "s10_connections, whole file";
        CHECK(load("s10_connections").saves == 71);
    }
    {
        // s11_blocks_a: the list of blocks, one action per save, all on
        // ZZBRAVO (see tests/fixtures/README.md). This is the project file as
        // it was after 80 saves; TIA Portal rewrote it a few saves later.
        using P = tia::ProgramData;
        struct Step {
            size_t save;
            const char* what;
            bool (*holds)(const P&);
        };
        const Step steps[] = {
            {73, "start: the blocks of s08_program, nothing protected",
             [](const P& p) {
                 const tia::BlockInfo* m = blockInfo(p, "ZZBRAVO", "Main");
                 return p.blockList.size() == 11 && m && m->type == "OB" && m->number == 1 &&
                        !blockInfo(p, "ZZBRAVO", "ZZFC");
             }},
            {74, "function block ZZFC added, SCL, number given by TIA Portal",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZFC");
                 return p.blockList.size() == 12 && b && b->type == "FB" && b->hasNumber && b->number == 3 &&
                        b->language == "SCL" && b->folder == "Program blocks" && b->version == "0.1" &&
                        b->title.empty() && b->comment.empty() && b->hasNetworks && b->networks == 1;
             }},
            {75, "cyclic interrupt OB added: OB30",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZCYCLIC");
                 return b && b->type == "OB" && b->number == 30 && b->kind == "CyclicInterrupt" &&
                        b->language == "LAD" && b->languageStored == "LAD_CLASSIC";
             }},
            {76, "group added, ZZFC moved into it",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZFC");
                 const tia::BlockInfo* m = blockInfo(p, "ZZBRAVO", "Main");
                 return b && b->folder == "Program blocks/Group_1" && !b->system && m && m->folder == "Program blocks";
             }},
            {77, "number of ZZFC set by hand: 77",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZFC");
                 return b && b->number == 77 && b->author.empty();
             }},
            {78, "title, comment, author, family, version and user-defined ID of ZZFC",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZFC");
                 return b && b->title == "ZZTITLE" && b->comment == "ZZCOMMENT" && b->author == "ZZAUTH" &&
                        b->family == "ZZFAM" && b->version == "1.2" && b->userId == "ZZID";
             }},
            {79, "title and comment of DB_Standard",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "DB_Standard");
                 const tia::DataBlock* d = nullptr;
                 for (const auto& db : p.blocks)
                     if (db.name == "DB_Standard") d = &db;
                 return b && b->title == "ZZDBTITLE" && b->comment == "ZZDBCOMMENT" && d && d->title == "ZZDBTITLE" &&
                        d->comment == "ZZDBCOMMENT" && b->writeProtectedInDevice.stored &&
                        !b->writeProtectedInDevice.on && b->compileNeeded == "UpToDate";
             }},
            {80, "DB_Standard write-protected in the device: has to be compiled again",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "DB_Standard");
                 const tia::BlockInfo* o = blockInfo(p, "ZZBRAVO", "DB_Optimized");
                 return b && b->writeProtectedInDevice.on && b->compileNeeded == "Binary" && o &&
                        !o->writeProtectedInDevice.on && o->compileNeeded == "UpToDate" && b->protection.empty();
             }},
        };
        for (const Step& st : steps) {
            Loaded l = load("s11_blocks_a", st.save);
            CHECK(l.hashErrors == 0);
            current = "s11_blocks_a after save " + std::to_string(st.save) + ": " + st.what;
            CHECK(st.holds(l.prog));
        }
        current = "s11_blocks_a, whole file";
        CHECK(load("s11_blocks_a").saves == 80);
    }
    {
        // s11_blocks: the same project a few saves later. TIA Portal had
        // rewritten the file by then: the first save marker follows the type
        // model, the second closes the state after four more actions (code
        // of ZZFC edited, program compiled, Block_1 know-how protected, ZZFC
        // write-protected), and the saves after that were appended.
        using P = tia::ProgramData;
        struct Step {
            size_t save;
            const char* what;
            bool (*holds)(const P&);
        };
        const Step steps[] = {
            // Screenshot of Block_1's Protection page at this point: "The
            // block is protected", no write protection, copy protection "No
            // binding". Screenshot of the project tree: Main [OB1], ZZCYCLIC
            // [OB30], Block_1 [FB1], Block_2 [FB2], DB1 to DB4, and
            // ZZFC [FB77] in Group_1.
            {2, "compiled; Block_1 know-how protected, ZZFC write-protected",
             [](const P& p) {
                 const tia::BlockInfo* b1 = blockInfo(p, "ZZBRAVO", "Block_1");
                 const tia::BlockInfo* b2 = blockInfo(p, "ZZBRAVO", "Block_2");
                 const tia::BlockInfo* fc = blockInfo(p, "ZZBRAVO", "ZZFC");
                 const tia::BlockInfo* db = blockInfo(p, "ZZBRAVO", "DB_Standard");
                 if (!b1 || !b2 || !fc || !db || p.blockList.size() != 13) return false;
                 for (const auto& b : p.blockList)
                     if (b.plc == "ZZBRAVO" && b.compileNeeded != "UpToDate") return false;
                 return b1->protection == "know-how" && b1->protectionStored == "KnowHowProtection" &&
                        !b1->writeProtection.stored && b1->copyProtection.empty() && fc->protection == "write" &&
                        fc->protectionStored == "NoProtection" && fc->writeProtection.on && fc->number == 77 &&
                        fc->folder == "Program blocks/Group_1" && b2->protection.empty() &&
                        b1->compiled.compare(0, 16, "2026-10-06T20:48") == 0 && b1->hasLoadMemory &&
                        b1->loadMemory > 0 && b1->hasWorkMemory && db->writeProtectedInDevice.on &&
                        b1->downloaded.empty();
             }},
            {3, "Block_2 bound to the serial number of the CPU, entered by hand",
             [](const P& p) {
                 const tia::BlockInfo* b2 = blockInfo(p, "ZZBRAVO", "Block_2");
                 const tia::BlockInfo* b1 = blockInfo(p, "ZZBRAVO", "Block_1");
                 return b2 && b2->copyProtection == "cpu" && b2->copyProtectionStored == "BindToPLC" &&
                        b2->copyProtectionSerial == "S C-ZZ99887766" && b2->protection.empty() &&
                        b2->compileNeeded == "Binary" && b1 && b1->protection == "know-how";
             }},
            {4, "know-how protection of Block_1 removed",
             [](const P& p) {
                 const tia::BlockInfo* b1 = blockInfo(p, "ZZBRAVO", "Block_1");
                 const tia::BlockInfo* fc = blockInfo(p, "ZZBRAVO", "ZZFC");
                 return b1 && b1->protection.empty() && b1->protectionStored == "NoProtection" && fc &&
                        fc->protection == "write";
             }},
            {5, "ZZCYCLIC deleted",
             [](const P& p) { return p.blockList.size() == 12 && !blockInfo(p, "ZZBRAVO", "ZZCYCLIC"); }},
            // Screenshot of its Attributes page: optimized access ticked, not
            // write-protected, not "only in load memory", the two
            // "accessible" boxes ticked but greyed out.
            {6, "data block ZZDB added on ZZBRAVO",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZBRAVO", "ZZDB");
                 const tia::BlockInfo* m = blockInfo(p, "ZZCHARLIE", "Main");
                 return b && b->type == "DB" && b->number == 5 && b->kind == "global" &&
                        !b->accessibleFromOpcUa.stored && !b->accessibleFromWebServer.stored && b->hasAccess &&
                        b->symbolicAccessOnly && !b->writeProtectedInDevice.on && !b->onlyInLoadMemory.on &&
                        // counted in TIA Portal: OB1 of ZZCHARLIE has one network
                        m && m->hasNetworks && m->networks == 1;
             }},
            {7, "data block ZZDB2 added on ZZCHARLIE: both access options at their default, nothing stored",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZCHARLIE", "ZZDB2");
                 return b && b->type == "DB" && b->number == 1 && !b->accessibleFromOpcUa.stored &&
                        !b->accessibleFromWebServer.stored;
             }},
            {8, "\"Data block accessible from OPC UA\" unticked",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZCHARLIE", "ZZDB2");
                 return b && b->accessibleFromOpcUa.stored && !b->accessibleFromOpcUa.on &&
                        !b->accessibleFromWebServer.stored;
             }},
            {9, "\"Data block accessible via Web server\" unticked",
             [](const P& p) {
                 const tia::BlockInfo* b = blockInfo(p, "ZZCHARLIE", "ZZDB2");
                 return b && !b->accessibleFromOpcUa.on && b->accessibleFromWebServer.stored &&
                        !b->accessibleFromWebServer.on;
             }},
        };
        for (const Step& st : steps) {
            Loaded l = load("s11_blocks", st.save);
            CHECK(l.hashErrors == 0);
            current = "s11_blocks after save " + std::to_string(st.save) + ": " + st.what;
            CHECK(st.holds(l.prog));
        }
        // Three screenshots taken in TIA Portal at the end, on ZZBRAVO:
        // Program info > Resources (load memory and work memory per block,
        // "?" for the two blocks that have to be compiled again), the Time
        // stamps page of ZZFC (shown in local time, UTC+2), and OB1 with its
        // three networks and its title in quotes.
        {
            Loaded l = load("s11_blocks");
            current = "s11_blocks: Program info > Resources";
            const struct { const char* name; int64_t load; int64_t work; } res[] = {
                {"Main", 4374, 173},         {"Block_1", 3436, 82},     {"ZZFC", 2906, 82},
                {"DB_Standard", 5824, 3166}, {"DB_Optimized", 5622, 3268},
                {"Block_1_DB", 1659, 180},   {"Block_2_DB", 1645, 180}};
            for (const auto& r : res) {
                const tia::BlockInfo* b = blockInfo(l.prog, "ZZBRAVO", r.name);
                current = std::string("s11_blocks: Program info > Resources, ") + r.name;
                CHECK(b && b->hasLoadMemory && b->loadMemory == r.load && b->hasWorkMemory && b->workMemory == r.work);
            }
            current = "s11_blocks: Program info > Resources, the rest";
            const tia::BlockInfo* udt = blockInfo(l.prog, "ZZBRAVO", "User_data_type_1");
            CHECK(udt && udt->hasLoadMemory && udt->loadMemory == 912 && !udt->hasWorkMemory);
            for (const char* name : {"Block_2", "ZZDB"}) {
                const tia::BlockInfo* b = blockInfo(l.prog, "ZZBRAVO", name);
                CHECK(b && !b->hasLoadMemory && !b->hasWorkMemory && b->compileNeeded != "UpToDate");
            }
            current = "s11_blocks: time stamps of ZZFC";
            const tia::BlockInfo* fc = blockInfo(l.prog, "ZZBRAVO", "ZZFC");
            CHECK(fc && fc->created.compare(0, 19, "2026-10-06T20:35:50") == 0);            // 10:35:50 PM
            CHECK(fc && fc->modified.compare(0, 19, "2026-10-06T20:50:22") == 0);           // 10:50:22 PM
            CHECK(fc && fc->interfaceModified.compare(0, 19, "2026-10-06T20:39:56") == 0);  // 10:39:56 PM
            CHECK(fc && fc->codeModified.compare(0, 19, "2026-10-06T20:48:10") == 0);       // 10:48:10 PM
            current = "s11_blocks: OB1";
            const tia::BlockInfo* ob = blockInfo(l.prog, "ZZBRAVO", "Main");
            CHECK(ob && ob->hasNetworks && ob->networks == 3 && ob->title == "\"Main Program Sweep (Cycle)\"");
        }
        // the first marker of a rewritten file closes nothing but the type model
        Loaded first = load("s11_blocks", 1);
        current = "s11_blocks, save 1 and whole file";
        CHECK(first.prog.blockList.empty() && first.inv.devices.empty());
        Loaded whole = load("s11_blocks");
        CHECK(whole.saves == 9 && whole.inv.devices.size() == 6);
    }
    {
        // s12_constants: user constants, one action per save, on ZZBRAVO
        // (see tests/fixtures/README.md), and the hardware identifiers.
        using P = tia::ProgramData;
        auto constant = [](const P& p, const std::string& name) -> const tia::Constant* {
            for (const auto& c : p.constants)
                if (c.kind == "user" && c.name == name) return &c;
            return nullptr;
        };
        auto users = [](const P& p) {
            size_t n = 0;
            for (const auto& c : p.constants) n += c.kind == "user";
            return n;
        };
        struct Step {
            size_t save;
            const char* what;
            bool (*holds)(const P&, decltype(constant)&, decltype(users)&);
        };
        const Step steps[] = {
            {11, "start: no user constants", [](const P& p, auto&, auto& n) { return n(p) == 0; }},
            {12, "ZZCONST_INT, Int, 42, with a comment, in the default tag table",
             [](const P& p, auto& c, auto& n) {
                 const tia::Constant* k = c(p, "ZZCONST_INT");
                 return n(p) == 1 && k && k->plc == "ZZBRAVO" && k->table == "Default tag table" &&
                        k->dataType == "Int" && k->value == "42" && k->comment == "My int constant" && !k->system &&
                        k->standsFor.empty();
             }},
            {13, "ZZCONST_REAL, Real, 3.5",
             [](const P& p, auto& c, auto& n) {
                 const tia::Constant* k = c(p, "ZZCONST_REAL");
                 return n(p) == 2 && k && k->dataType == "Real" && k->value == "3.5" && k->comment.empty();
             }},
            {14, "ZZCONST_TIME, Time, T#5s, in another tag table",
             [](const P& p, auto& c, auto& n) {
                 const tia::Constant* k = c(p, "ZZCONST_TIME");
                 return n(p) == 3 && k && k->table == "Tag MyTagTable" && k->dataType == "Time" && k->value == "T#5s";
             }},
            {15, "ZZCONST_STR, String, 'Hello'",
             [](const P& p, auto& c, auto& n) {
                 const tia::Constant* k = c(p, "ZZCONST_STR");
                 return n(p) == 4 && k && k->table == "Tag MyTagTable" && k->dataType == "String" &&
                        k->value == "'Hello'";
             }},
            {16, "value of ZZCONST_INT changed to 43",
             [](const P& p, auto& c, auto& n) {
                 const tia::Constant* k = c(p, "ZZCONST_INT");
                 return n(p) == 4 && k && k->value == "43" && k->comment == "My int constant";
             }},
            {17, "ZZCONST_REAL deleted",
             [](const P& p, auto& c, auto& n) { return n(p) == 3 && !c(p, "ZZCONST_REAL") && c(p, "ZZCONST_STR"); }},
        };
        for (const Step& st : steps) {
            Loaded l = load("s12_constants", st.save);
            CHECK(l.hashErrors == 0);
            current = "s12_constants after save " + std::to_string(st.save) + ": " + st.what;
            CHECK(st.holds(l.prog, constant, users));
        }
        Loaded l = load("s12_constants");
        current = "s12_constants, whole file";
        CHECK(l.saves == 17);
        // user constants keep the order in which they were entered; tags are untouched
        std::vector<std::string> names;
        for (const auto& c : l.prog.constants)
            if (c.kind == "user") names.push_back(c.name);
        CHECK(names.size() == 3 && names[0] == "ZZCONST_INT" && names[1] == "ZZCONST_TIME" && names[2] == "ZZCONST_STR");
        CHECK(l.prog.tags.size() == 12);
        // TIA Portal's own export of the tags at the end
        // (exports/PLCTags.xlsx), sheet "Constants": name, path, data type,
        // value, comment. Same rows, same order.
        current = "s12_constants: export of the constants";
        const struct { const char* name; const char* path; const char* type; const char* value; const char* comment; }
        exported[] = {{"ZZCONST_INT", "Default tag table", "Int", "43", "My int constant"},
                      {"ZZCONST_TIME", "Tag MyTagTable", "Time", "T#5s", ""},
                      {"ZZCONST_STR", "Tag MyTagTable", "String", "'Hello'", ""}};
        size_t row = 0;
        for (const auto& c : l.prog.constants) {
            if (c.kind != "user") continue;
            CHECK(row < 3);
            if (row < 3)
                CHECK(c.name == exported[row].name && c.table == exported[row].path &&
                      c.dataType == exported[row].type && c.value == exported[row].value &&
                      c.comment == exported[row].comment);
            ++row;
        }
        CHECK(row == 3);
        // Screenshot at the end: the project tree shows "Default tag table
        // [63]" and "Tag MyTagTable [10]". TIA Portal counts tags and
        // constants: 4 tags + 1 user constant + 58 system constants (23
        // hardware identifiers, among them those of the two IO devices, the
        // OB constant and 34 process image partitions), and 8 tags + 2 user
        // constants.
        current = "s12_constants: numbers TIA Portal shows next to the tag tables";
        std::map<std::string, size_t> perTable, kinds;
        for (const auto& t : l.prog.tags)
            if (t.plc == "ZZBRAVO") ++perTable[t.table];
        for (const auto& c : l.prog.constants) {
            if (c.plc != "ZZBRAVO") continue;
            ++perTable[c.table];
            ++kinds[c.kind];
        }
        CHECK(perTable["Default tag table"] == 63 && perTable["Tag MyTagTable"] == 10);
        CHECK(kinds["hardware"] == 23 && kinds["ob"] == 1 && kinds["pip"] == 34 && kinds["user"] == 3);
        // Screenshot of the "System constants" tab, rows 1 to 34: None
        // 65535, Automatic update 0, PIP 1 .. PIP 31, PIP OB Servo 32768,
        // all of data type Pip.
        current = "s12_constants: process image constants";
        std::map<std::string, std::string> pip;
        for (const auto& c : l.prog.constants)
            if (c.plc == "ZZBRAVO" && c.kind == "pip") {
                CHECK(c.dataType == "Pip" && c.system);
                pip[c.name] = c.value;
            }
        CHECK(pip.size() == 34 && pip["None"] == "65535" && pip["Automatic update"] == "0" &&
              pip["PIP OB Servo"] == "32768");
        for (int i = 1; i <= 31; ++i) CHECK(pip["PIP " + std::to_string(i)] == std::to_string(i));
        // Second screenshot of that tab, rows 35 to 58: the hardware
        // identifiers and the OB constant. (TIA Portal cut off the names of
        // the four IO device ports; their values are in the screenshot.) What
        // each one stands for is not shown there; it is checked against the
        // name.
        current = "s12_constants: hardware identifiers";
        const struct { const char* name; const char* type; const char* value; const char* device; const char* item; }
        shown[] = {
            {"Local~MC", "Hw_SubModule", "51", "S7-1500/ET200MP station_1", "Card reader/writer_1"},
            {"Local~Common", "Hw_SubModule", "50", "S7-1500/ET200MP station_1", "ZZBRAVO"},
            {"Local~Device", "Hw_Device", "32", "S7-1500/ET200MP station_1", "ZZBRAVO"},
            {"Local~Configuration", "Hw_SubModule", "33", "S7-1500/ET200MP station_1", "ZZBRAVO"},
            {"Local~Display", "Hw_SubModule", "54", "S7-1500/ET200MP station_1", "CPU display_1"},
            {"Local~Exec", "Hw_SubModule", "52", "S7-1500/ET200MP station_1", "CPU exec unit_1"},
            {"Local", "Hw_SubModule", "49", "S7-1500/ET200MP station_1", " ZZBRAVO"},
            {"Local~PROFINET_interface_1", "Hw_Interface", "64", "S7-1500/ET200MP station_1", "PROFINET interface_1"},
            {"Local~PROFINET_interface_1~Port_1", "Hw_Interface", "65", "S7-1500/ET200MP station_1", "Port_1"},
            {"Local~PROFINET_interface_1~Port_2", "Hw_Interface", "66", "S7-1500/ET200MP station_1", "Port_2"},
            {"OB_Main", "OB_PCYCLE", "1", "", "Main"},
            {"Local~PROFINET_IO-System", "Hw_IoSystem", "257", "S7-1500/ET200MP station_1", "IOController_PROFINET"},
            {"IO_device_1~Proxy", "Hw_SubModule", "264", "ZZIO1", "IO device_1"},
            {"IO_device_1~IODevice", "Hw_Device", "262", "ZZIO1", "IO device_1"},
            {"IO_device_1~Head", "Hw_SubModule", "258", "ZZIO1", "_1"},
            {"IO_device_1~PROFINET_interface", "Hw_Interface", "259", "ZZIO1", "PROFINET interface"},
            {"IO_device_1~PROFINET_interface~Port_1", "Hw_Interface", "260", "ZZIO1", "Port_1"},
            {"IO_device_1~PROFINET_interface~Port_2", "Hw_Interface", "261", "ZZIO1", "Port_2"},
            {"IO_device_2~Proxy", "Hw_SubModule", "267", "ZZI02", "IO device_2"},
            {"IO_device_2~IODevice", "Hw_Device", "265", "ZZI02", "IO device_2"},
            {"IO_device_2~Head", "Hw_SubModule", "268", "ZZI02", "_1"},
            {"IO_device_2~PROFINET_interface", "Hw_Interface", "269", "ZZI02", "PROFINET interface"},
            {"IO_device_2~PROFINET_interface~Port_1", "Hw_Interface", "270", "ZZI02", "Port_1"},
            {"IO_device_2~PROFINET_interface~Port_2", "Hw_Interface", "271", "ZZI02", "Port_2"}};
        size_t matched = 0;
        for (const auto& want : shown) {
            const tia::Constant* found = nullptr;
            for (const auto& c : l.prog.constants)
                if (c.plc == "ZZBRAVO" && c.system && c.name == want.name) found = &c;
            current = std::string("s12_constants: hardware identifiers, ") + want.name;
            CHECK(found && found->dataType == want.type && found->value == want.value &&
                  found->standsFor == want.item && found->standsForDevice == want.device);
            if (found) {
                ++matched;
                CHECK(found->kind == (std::string(want.type) == "OB_PCYCLE" ? "ob" : "hardware"));
            }
        }
        // 23 hardware identifiers and the OB constant: nothing else, nothing missing
        CHECK(matched == 24 && kinds["hardware"] + kinds["ob"] == 24);
    }
    testHistory();
    std::printf("%d fixture checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
