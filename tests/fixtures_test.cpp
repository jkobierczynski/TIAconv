// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the V21 test projects in tests/fixtures. Each project differs from the
// previous one by one known change (see tests/fixtures/README.md), so every
// value checked here was typed into TIA Portal by hand.
#include <cstdio>
#include <string>
#include <vector>

#include "container.hpp"
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

void common(const Loaded& l, const std::string& projectName) {
    CHECK(l.layout == tia::Layout::V14);
    CHECK(l.hashErrors == 0);
    CHECK(l.inv.warnings.empty());
    CHECK(l.inv.stats.objectsWithProblems == 0);
    CHECK(l.inv.stats.unattachedItems == 0);
    CHECK(l.inv.project.found && l.inv.project.name == projectName);
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
    std::printf("%d fixture checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
