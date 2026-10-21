// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A code block as a source file: the header, the declarations of its
// interface and its code, in the form TIA Portal's "Generate source from
// blocks" writes for SCL (.scl) and STL (.awl). LAD and FBD have no source
// form in TIA Portal; they get the same header and declarations and
// tiaconv's listing of the networks (.txt). Know-how protected blocks get a
// file that says so, without interface or code.
#pragma once

#include <string>
#include <vector>

#include "code.hpp"
#include "program.hpp"

namespace tia {

struct BlockSource {
    std::string fileName;  // "ZZSCL.scl"; characters not allowed in file names replaced by "_"
    // "scl", "stl" (TIA Portal's source form), "listing" (LAD, FBD: tiaconv's
    // notation), "protected" (no code read), "unread" (some network could
    // not be read; the file says which)
    std::string form;
    std::vector<std::string> lines;  // without line ends
};

// The declaration part: "   VAR_INPUT ", "      start : Bool;   // ...",
// "   END_VAR", "" for each section, as TIA Portal writes it.
std::vector<std::string> declarationLines(const BlockInfo& info);

BlockSource blockSource(const BlockInfo& info, const BlockCode& code);

// A name as a source writes it: as it is when it is a plain identifier,
// otherwise in double quotes ("666").
std::string sourceName(const std::string& name);

// Writes one file per code block of `code` into `dir` (created when
// missing), with CR LF line ends and a UTF-8 byte-order mark unless
// `bom` is false; returns the number of files written. Errors throw
// std::runtime_error.
size_t writeBlockSources(const std::string& dir, const ProgramData& prog, const CodeData& code, bool bom);

}  // namespace tia
