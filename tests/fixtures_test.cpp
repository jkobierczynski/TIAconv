// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads the V21 test projects in tests/fixtures. Each project differs from the
// previous one by one known change (see tests/fixtures/README.md), so every
// value checked here was typed into TIA Portal by hand.
#include <cstdio>
#include <string>

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
    tia::Layout layout = tia::Layout::V11;
};

Loaded load(const std::string& name) {
    current = name;
    tia::LoadedSource src = tia::loadProjectData(std::string(TIACONV_FIXTURES) + "/" + name);
    tia::ContainerOptions opt;
    opt.verifyHashes = true;
    tia::Project p(tia::Container::parse(std::move(src.data), opt));
    Loaded l;
    l.inv = tia::buildInventory(p);
    l.prog = tia::buildProgramData(p);
    l.hashErrors = p.container().hashErrors();
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
    std::printf("%d fixture checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
