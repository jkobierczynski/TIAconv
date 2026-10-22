// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The program of a PLC as one JSON file, complete enough to simulate it
// outside TIA Portal: the memory (tags, constants, data blocks with offsets,
// start values and retain), the I/O addresses of its hardware, the
// organization blocks with their events, every code block with its
// interface and its code (SCL or STL as text, LAD or FBD as the graph of
// parts and wires), the instructions it uses, and what cannot be simulated
// from the file. The format is described in docs/PROGRAM-JSON.md and
// docs/program.schema.json; kProgramSchemaVersion changes with it.
#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "code.hpp"
#include "inventory.hpp"
#include "program.hpp"

namespace tia {

constexpr int kProgramSchemaVersion = 1;

struct ProgramJsonContext {
    std::string toolVersion;
    std::string source;     // the file read, as given
    size_t shownSave = 0;   // the project as it was after this save; 0: as it is now
};

// The PLCs of the program, in the order of their blocks, tags and data
// blocks.
std::vector<std::string> programPlcs(const ProgramData& prog);

void writeProgramJson(std::ostream& out, const std::string& plc, const Inventory& inv, const ProgramData& prog,
                      const CodeData& code, const ProgramJsonContext& ctx);

// One file per PLC in `dir` (created when missing): "<PLC>.json", with
// characters not allowed in file names replaced by "_". Returns the files
// written. Errors throw std::runtime_error.
std::vector<std::string> writeProgramJsonFiles(const std::string& dir, const Inventory& inv, const ProgramData& prog,
                                               const CodeData& code, const ProgramJsonContext& ctx);

}  // namespace tia
