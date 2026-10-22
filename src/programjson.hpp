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

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "code.hpp"
#include "inventory.hpp"
#include "program.hpp"

namespace tia {

constexpr int kProgramSchemaVersion = 2;

// A data type as TIA Portal writes it ("Array[0..9] of Int", "String[20]",
// "\"Motor\"", TON_TIME), taken apart. `kind` is "elementary" (with `bits`),
// "string" (with `length`, the maximum number of characters), "array"
// (`dims`, `element`), "struct", "parameter" (Variant, Any, Pointer, ...),
// "void", "named" (any other name; `quoted` when written in quotes; the
// program export says what it names), or "unknown".
struct TypeDescriptor {
    std::string kind;
    std::string name;
    bool quoted = false;
    int64_t bits = -1;    // elementary: the size; -1 when not known
    int64_t length = -1;  // string
    struct Bound {
        std::string text;  // as written: "0", "#MAX", "\"MAX\"", "*"
        bool known = false;
        int64_t value = 0;
    };
    struct Dim {
        Bound low, high;
    };
    std::vector<Dim> dims;
    std::shared_ptr<TypeDescriptor> element;
};

TypeDescriptor describeType(const std::string& dataType);

// A value as TIA Portal writes it (a start value, a constant, a literal in
// code), parsed. `kind` is "bool", "int" (also bit strings and characters
// as their code), "real", "duration" (`ns`), "date" (`days` since
// 1990-01-01), "time_of_day" (`ns` since midnight), "date_and_time" (`ns`
// since 1970-01-01), "string", "char" (`string` and its code in `integer`),
// or "unknown". The type, where known, settles what the text alone does
// not: "1" for a Real is 1.0, "5000" for a Time is 5 s, 'A' for a Char a
// character.
struct ParsedValue {
    std::string kind;
    std::string text;  // as written, without blanks around it
    bool boolean = false;
    // int: the value; when `unsignedBig` it is above the range of int64_t
    // and `unsignedValue` holds it (ULInt, LWord)
    int64_t integer = 0;
    bool unsignedBig = false;
    uint64_t unsignedValue = 0;
    double real = 0;
    int64_t ns = 0;
    int64_t days = 0;
    std::string string;
};

ParsedValue parseValue(const std::string& text, const TypeDescriptor* type = nullptr);

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
