// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ostream>
#include <string>

#include "code.hpp"
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
    const CodeData* code = nullptr;    // text and JSON: add the code of the blocks
};

void writeText(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx);
void writeJson(std::ostream& out, const Inventory& inv, const ProgramData& prog, const ReportContext& ctx);
void writeCsv(std::ostream& out, const Inventory& inv, const ReportContext& ctx);

// One row per tag.
void writeTagsCsv(std::ostream& out, const ProgramData& prog);
// One side of `tiaconv diff`.
struct DiffSide {
    std::string source;   // as given on the command line
    std::string project;  // its name
    std::string modified, by;
    size_t saves = 0;      // saves the file records
    size_t shownSave = 0;  // compared as it was after this save; 0: as it is now
};

void writeDiffText(std::ostream& out, const ProjectDiff& d, const DiffSide& oldSide, const DiffSide& newSide);
void writeDiffJson(std::ostream& out, const ProjectDiff& d, const DiffSide& oldSide, const DiffSide& newSide,
                   const std::string& toolVersion);
// One row per change.
void writeDiffCsv(std::ostream& out, const ProjectDiff& d);

// One row per use of a tag, data block member or block in the code of a
// block: the cross-reference.
void writeCrossReferenceCsv(std::ostream& out, const CodeData& code);
// One row per tag of an HMI device.
void writeHmiTagsCsv(std::ostream& out, const ProgramData& prog);
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
