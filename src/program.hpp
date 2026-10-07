// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PLC program: the list of blocks, tags, and data blocks with their members.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project.hpp"

namespace tia {

struct Tag {
    uint64_t id = 0;
    std::string plc;
    std::string table;
    std::string name;
    std::string dataType;
    std::string address;  // e.g. "%M1.0"
    std::string comment;
};

// A constant of a PLC. TIA Portal creates the system constants itself: the
// hardware identifiers, the numbers of the organization blocks and the
// process image partitions. User constants are entered in a tag table.
struct Constant {
    uint64_t id = 0;
    std::string plc;
    std::string table;
    std::string name;
    std::string dataType;  // Hw_Interface, OB_PCYCLE, Pip, Int, ...
    std::string value;     // as written in the project
    std::string comment;
    bool system = false;   // created by TIA Portal
    // "hardware" (a hardware identifier), "ob", "pip", "user", or "system"
    // for any other constant TIA Portal created
    std::string kind;
    // What a system constant stands for: the module, interface or port with
    // the station it is in, or the block.
    std::string standsFor;
    std::string standsForDevice;
};

struct BlockMember {
    std::string name;
    std::string dataType;
    std::string section;     // Input / Output / InOut / Static, instance blocks only
    std::string comment;
    std::string startValue;  // as written in the project; empty when the type default applies
    bool hasStartValue = false;
    bool hasOffset = false;  // only for blocks with standard (not optimized) access
    uint64_t offsetBits = 0;
    bool isBit = false;
    bool unresolved = false;  // the nested type could not be followed
    std::vector<BlockMember> members;
};

struct DataBlock {
    uint64_t id = 0;
    std::string plc;
    std::string name;
    std::string address;  // e.g. "%DB1"
    std::string title;
    std::string comment;
    bool hasNumber = false;
    int64_t number = 0;
    std::string kind;        // global, instance, or the raw type name
    std::string instanceOf;  // block or type an instance block belongs to
    bool hasAccess = false;
    bool symbolicAccessOnly = false;  // "optimized block access"
    std::vector<BlockMember> members;
    size_t memberCount = 0;  // all levels
    std::vector<std::string> notes;
};

// A stored setting: whether the project has it at all, and its value.
struct Flag {
    bool stored = false;
    bool on = false;
};

// One block or data type of a PLC, as listed under "Program blocks" and
// "PLC data types": what it is, how it is protected and when it changed.
struct BlockInfo {
    uint64_t id = 0;
    std::string plc;
    std::string type;  // OB, FB, FC, DB, UDT; SFB, SFC, SDT for what the firmware provides
    bool hasNumber = false;
    int64_t number = 0;
    std::string name;
    std::string kind;        // OB: the event it handles; DB: global or instance; otherwise what the project says
    std::string instanceOf;  // DB: the block or type it is an instance of
    std::string language;        // as TIA Portal names it: LAD, FBD, STL, SCL, ...
    std::string languageStored;  // as the project names it: LAD_CLASSIC, ...
    // "", "know-how", "write", "know-how, write", or "system" (protected
    // block from a Siemens library)
    std::string protection;
    std::string protectionStored;  // NoProtection, KnowHowProtection, ...
    Flag writeProtection;          // the block's own write protection setting
    // Binding to the serial number of a CPU or memory card: "", "cpu",
    // "memory-card", or the stored name for anything else.
    std::string copyProtection;
    std::string copyProtectionStored;
    std::string copyProtectionSerial;  // the serial number the block is bound to, when entered in the project
    std::string folder;   // path in the project tree, "Program blocks/Pumps"
    bool system = false;  // kept under "System blocks" or "System data types"
    std::string title;
    std::string comment;
    std::string author, family, userId, version;  // the block header
    bool hasAccess = false;
    bool symbolicAccessOnly = false;  // "optimized block access"
    std::string created, modified, codeModified, interfaceModified;  // ISO 8601, empty when not stored
    std::string compiled;    // last compilation
    // Why the block has to be compiled again, as the project names it
    // ("Binary", "TypeInfo"); "UpToDate" when it does not; empty when not stored.
    std::string compileNeeded;
    std::string downloaded;  // last download to a device from this project
    std::vector<std::string> downloads;  // download history, newest first
    bool hasLoadMemory = false, hasWorkMemory = false;
    int64_t loadMemory = 0, workMemory = 0;  // bytes
    bool hasNetworks = false;
    size_t networks = 0;  // code blocks: networks (one for a block written as text)
    // data blocks
    Flag writeProtectedInDevice;  // "Data block write-protected in the device"
    Flag onlyInLoadMemory;        // "Only store in load memory"
    Flag accessibleFromOpcUa;     // "Data block accessible from OPC UA"
    Flag accessibleFromWebServer; // "Data block accessible via Web server"
};

// What TIA Portal calls a programming language ("LAD" for LAD_CLASSIC);
// names that have not been compared with TIA Portal are returned as stored.
std::string languageName(const std::string& stored);

// The times in a block's stored download history, newest first, as ISO 8601.
std::vector<std::string> parseDownloadHistory(const std::string& stored);

struct ProgramStats {
    size_t tagsOutsideProject = 0;
    size_t blocksOutsideProject = 0;
    size_t blocksWithoutInterface = 0;
    size_t listedBlocksOutsideProject = 0;
    size_t constantsOutsideProject = 0;
};

struct ProgramData {
    std::vector<Tag> tags;
    std::vector<Constant> constants;
    std::vector<DataBlock> blocks;
    std::vector<BlockInfo> blockList;
    ProgramStats stats;
    std::vector<std::string> warnings;
};

ProgramData buildProgramData(const Project& project);

// "12.3" for a bit, "12" for anything else.
std::string formatOffset(const BlockMember& m);

}  // namespace tia
