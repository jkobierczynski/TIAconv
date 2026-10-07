// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ostream>
#include <string>

#include "history.hpp"
#include "inventory.hpp"
#include "program.hpp"
#include "project.hpp"

namespace tia {

struct ReportContext {
    std::string toolVersion;
    std::string source;
    std::string layout;
    bool hashesVerified = false;
    size_t hashErrors = 0;
    bool allDevices = false;  // text and CSV: include devices outside the project tree
    bool members = false;     // text: print the members of every data block
    size_t shownSave = 0;     // the project is shown as it was after this save; 0: as it is now
    const History* history = nullptr;  // text and JSON: add the save history
};

void writeText(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx);
void writeJson(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx);
void writeCsv(std::ostream& out, const Inventory& inv, const ReportContext& ctx);

// One row per tag.
void writeTagsCsv(std::ostream& out, const ProgramData& prog);
// One row per constant of a PLC: hardware identifiers, user constants and
// the other system constants.
void writeConstantsCsv(std::ostream& out, const ProgramData& prog);
// One row per block or data type of a PLC.
void writeBlockListCsv(std::ostream& out, const ProgramData& prog);
// One row per data block member, nested members as dotted paths.
void writeBlocksCsv(std::ostream& out, const ProgramData& prog);

// One row per change, and one for a save without any.
void writeHistoryCsv(std::ostream& out, const History& history);

// One JSON object per line for every decodable object: attributes, expando
// attributes and relations with resolved names.
void writeObjects(std::ostream& out, const Project& project);

}  // namespace tia
