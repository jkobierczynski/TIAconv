// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PLC data: tags and data blocks with their members.
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

struct ProgramStats {
    size_t tagsOutsideProject = 0;
    size_t blocksOutsideProject = 0;
    size_t blocksWithoutInterface = 0;
};

struct ProgramData {
    std::vector<Tag> tags;
    std::vector<DataBlock> blocks;
    ProgramStats stats;
    std::vector<std::string> warnings;
};

ProgramData buildProgramData(const Project& project);

// "12.3" for a bit, "12" for anything else.
std::string formatOffset(const BlockMember& m);

}  // namespace tia
