// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The code of the blocks: the networks of every OB, FB and FC with their
// content as text, and what each block uses (tags, data block members, other
// blocks) with the kind of access.
//
// SCL and STL are stored as the tokens of the source text and come out as
// that text.
// LAD and FBD are stored as parts and the wires between them; they come out
// as a listing in a notation of tiaconv's own (see README). Blocks with
// know-how protection are not read.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "program.hpp"
#include "project.hpp"

namespace tia {

// One place where a block uses something.
struct CodeUse {
    // The network: its position in the block (1 = first) and the number the
    // project knows it by. Position 0: not in a network (the block interface).
    size_t network = 0;
    int64_t networkId = 0;
    uint64_t uid = 0;  // the element of the network
    // "read", "write", "read and write", "call", "single instance" (the instance
    // data block of a call), "multiple instance", "array limit", "" for none, or
    // the stored name for anything else (TIA Portal's words in its list of
    // cross-references)
    std::string access;
    std::string accessStored;  // as the project names it; empty when it names none
    bool hidden = false;       // marked as not shown in TIA Portal's cross-references
};

// Something a block refers to: one entry of the table the block keeps of
// the tags, members, constants and blocks its code names.
struct CodeReference {
    int64_t refId = 0;
    // "tag", "data block member", "local" (a parameter or local variable of
    // the block), "constant", "local constant", "block", "data block",
    // "instance data block", "multi-instance", "instruction", or the stored
    // name for anything else
    std::string kind;
    std::string kindStored;
    std::string text;      // as written in code: "DB".member, #local, "Tag", 5
    std::string dataType;
    std::string container;  // data block member: the data block
    std::vector<CodeUse> uses;
    // What `text` is made of. The name of the entry itself; for an access
    // the names along its path, each with the reference numbers of its
    // array indices; and the part of the variable that is meant ("b0").
    std::string name;
    std::string scope;  // as stored: Global, Local, Constant, ...
    struct Step {
        std::string name;
        std::vector<int64_t> index;
    };
    std::vector<Step> path;
    std::string modifier;
    // The name the object referred to has now, where the project links the
    // entry to it (tags, blocks, data blocks). The stored name can be older:
    // TIA Portal updates it when the block is opened or compiled.
    std::string currentName;
    int64_t dataBlockRef = 0;  // a data block member: the entry of its data block
    bool renamed = false;      // `text` uses a current name that differs from the stored one
    // Not from the code but from the block's interface: the member declared
    // with this block as its data type (a multi-instance, "inner"). Its use
    // has network 0.
    std::string declaredAs;
    // "call interface": the parameters of the called block, in order
    std::vector<std::string> parameters;
};

// A part of a LAD or FBD network: a contact, a coil, a box, a call.
struct NetworkElement {
    uint64_t uid = 0;
    // "gate" (contact, coil, box), "call" (of a block), "instruction" (one
    // that has an instance, a timer for example)
    std::string kind;
    std::string name;      // the gate as stored (Contact, Coil, Move), the block or the instruction
    std::string instance;  // call, instruction: the instance as written in code
    // What the part was set to: the data type chosen for a box, the number
    // of inputs, "DisableENO", and the pins that are negated ("negated").
    std::vector<std::pair<std::string, std::string>> options;
    // The entry of the block's reference table for the block called or the
    // instruction (0: none), and for the instance; the data type a box was
    // set to ("" when not set; with "AutomaticTyped" in `options` TIA Portal
    // takes it from the operands).
    int64_t refId = 0;
    int64_t instanceRefId = 0;
    std::string dataType;
    // An operand at a pin, linked to its entry in the block's reference
    // table and through it to what it names.
    struct Operand {
        std::string text;    // as written in code, as in `connected`
        int64_t refId = 0;   // the entry of the reference table; 0 when the part names none
        bool linked = false; // the entry was found
        std::string kind, kindStored;  // as CodeReference::kind, kindStored
        // The data type: the entry's; for a literal without one, the type of
        // the pin (`dataTypeFromPin`)
        std::string dataType;
        bool dataTypeFromPin = false;
        std::string dataBlock;  // a data block member: the data block
        // A tag: its address (%M1.0). A member of a data block with standard
        // access, of an elementary type of up to 4 bytes: %DB1.DBX2.0,
        // %DB1.DBW4, ... Empty otherwise.
        std::string address;
        // The type the pin takes, where tiaconv knows it: Bool for the
        // operand of a contact or coil, the box's type for its data pins,
        // the parameter of a called block, the pins of the IEC timers and
        // counters. Empty when not known.
        std::string pinType;
    };
    struct Pin {
        std::string name;
        bool output = false;
        // What is at the pin: an operand as written in code, "power rail",
        // or the pin of another part as "<uid>.<pin>".
        std::vector<std::string> connected;
        std::vector<Operand> operands;  // the operands among `connected`, in that order
    };
    std::vector<Pin> pins;
};

// The code as a tree, the same for SCL, LAD and FBD: statements whose
// expressions name operands by their entry in the block's reference table.
//
// An expression's `op` is one of
//   ref      an operand (`refId`)
//   const    a literal (`literal`, `literalType`; `refId` where the table has it)
//   member   `args[0]`.`name`; `refId`: the entry for the whole access where there is one
//   index    `args[0]`[`args[1]`, ...]; `refId` as for member
//   slice    a part of `args[0]`: `name` is %X3, %B1, ...
//   not neg  unary
//   add sub mul div mod pow and or xor eq ne lt le gt ge    binary, `args` left to right
//   call     a block or instruction: `name`, `refId` (the block or instruction),
//            `instanceRefId` (its instance); `args` are its parameters
//   in out inout   a parameter of a call: `name`, `args[0]` the operand
//   edge_pos edge_neg   the edge of `args[0]` with the edge memory `args[1]` (LAD, FBD)
//   wire     the value of a pin of another part of the network, `name` "<uid>.<pin>"
//            (LAD, FBD: set by an earlier statement of the same network)
//   range    `args[0]`..`args[1]` (labels of CASE)
//   builtin  `name` ENO: the block's own ENO (SCL)
//   missing  nothing entered;  unknown: `name` says what
// An interpreter evaluates every operand of and/or: an edge in one of them
// updates its memory bit.
// `type` is the data type of the result where the project gives it (SCL).
struct CodeExpr {
    std::string op;
    int64_t refId = 0;
    int64_t instanceRefId = 0;
    std::string name;
    std::string type;
    std::string literal, literalType;
    std::vector<CodeExpr> args;
};

// A statement's `op` is one of
//   assign   `exprs[0]` := `exprs[1]`
//   call     `exprs[0]` (a call)
//   if       `branches`: condition in `exprs[0]`, then `body`; `elseBody` when `hasElse`
//   case     `exprs[0]` the selector; `branches`: labels in `exprs`, `body`; ELSE as for if
//   for      `exprs`: variable, from, to, [by]; `body`
//   while    `exprs[0]` the condition; `body`
//   repeat   `body`, until `exprs[0]`
//   return exit continue empty
//   eval     `exprs[0]` evaluated for its edges (LAD, FBD: logic that leads nowhere)
//   label    `name`: an STL label
//   region   `name`, `body`
//   stl      one STL statement: the instruction in `name` ("A", "L", "JC",
//            "CALL"), its operands in `exprs` (a CALL's parameters as `in`)
//   unknown  `name` says what
struct CodeStmt {
    std::string op;
    std::string name;
    std::vector<CodeExpr> exprs;
    struct Branch {
        std::vector<CodeExpr> exprs;
        std::vector<CodeStmt> body;
    };
    std::vector<Branch> branches;
    std::vector<CodeStmt> body;
    bool hasElse = false;
    std::vector<CodeStmt> elseBody;
    // stl: the operand as the editor shows it ("DBW [AR1,P#2.0]", "T 5")
    std::string text;
};

struct Network {
    uint64_t id = 0;
    size_t number = 0;      // position in the block, 1 = first
    int64_t networkId = 0;  // the number the project knows it by; stays when networks are moved
    std::string title;
    std::string comment;
    std::string language;        // as TIA Portal names it
    std::string languageStored;
    // "scl", "stl": `lines` is the source text. "graphic": LAD or FBD,
    // `lines` is tiaconv's listing of `elements`. "empty": a network
    // without code. "unread": `notes` says why.
    std::string content;
    std::vector<std::string> lines;
    // SCL: empty lines at the end of the text, not in `lines`; a generated
    // source has them
    size_t trailingEmptyLines = 0;
    std::vector<NetworkElement> elements;
    // The code as a tree (`treeRead`): SCL, LAD, FBD, STL
    bool treeRead = false;
    std::vector<CodeStmt> statements;
    std::vector<std::string> treeNotes;  // what of the tree could not be read
    std::vector<std::string> notes;
};

struct BlockCode {
    uint64_t blockId = 0;
    std::string plc;
    std::string type;  // OB, FB, FC
    bool hasNumber = false;
    int64_t number = 0;
    std::string name;
    std::string language;
    // The block is know-how protected (or a protected block of a Siemens
    // library): its code is not read. `protection` as in the list of blocks.
    bool isProtected = false;
    std::string protection;
    // Not read because the block was know-how protected later: this is a
    // version the file keeps from before (an earlier save is shown).
    bool protectedLater = false;
    std::vector<Network> networks;
    std::vector<CodeReference> references;
    std::vector<std::string> notes;
};

struct CodeStats {
    size_t blocks = 0;           // code blocks looked at
    size_t protectedBlocks = 0;  // of those, not read because protected
    size_t networks = 0;
    size_t unreadNetworks = 0;   // networks whose content could not be read
    size_t references = 0;
};

struct CodeData {
    std::vector<BlockCode> blocks;  // the code blocks of ProgramData::blockList, in that order
    CodeStats stats;
};

// Where know-how protection ends in the file: for every code block that is
// protected in some version the file holds, the position (in the file's
// list of object blocks) of the last such version. Read from the whole
// file, not from an earlier save.
using ProtectedVersions = std::map<uint64_t, size_t>;
ProtectedVersions protectedVersions(const Project& wholeFile);

// `protectedUpTo`, for showing an earlier save: a block is not read when the
// version of it in this state of the project is the last protected one or
// older. A project file keeps earlier versions of its objects, so it can
// still hold a block as it was before it was protected; that is not a way
// around the protection here. Once the protection has been removed in the
// project, the versions from then on are read.
CodeData buildCode(const Project& project, const ProgramData& prog, const ProtectedVersions* protectedUpTo = nullptr);

// The ids of the blocks in `prog` whose code is not to be read.
std::set<uint64_t> protectedBlockIds(const ProgramData& prog);

// --- pieces, exposed for the tests ---

// The source text of an SCL network from its stored token document, one
// string per line. `names` gives the text for a reference number where the
// tokens do not carry it themselves; `comments` the text of a multi-language
// comment by its number. Returns false when the document is not SCL tokens.
struct SclContext {
    const std::vector<CodeReference>* references = nullptr;
    const std::vector<std::string>* comments = nullptr;  // index = number - 1
};
bool sclText(const std::string& xml, const SclContext& ctx, std::vector<std::string>& lines,
             std::vector<std::string>& notes, size_t* trailingEmptyLines = nullptr);

// The statements of an SCL network from its stored token document. Returns
// false when the document is not SCL tokens.
bool sclTree(const std::string& xml, const SclContext& ctx, std::vector<CodeStmt>& out,
             std::vector<std::string>& notes);

// The lines of an STL network from its stored document. Returns false when
// the document is not STL statements.
bool stlText(const std::string& xml, const std::vector<CodeReference>& references, std::vector<std::string>& lines,
             std::vector<std::string>& notes, std::vector<CodeStmt>* tree = nullptr);

// The parts of a LAD or FBD network from its stored document, and the
// listing. Returns false when the document is not such a network.
bool graphicNetwork(const std::string& xml, const std::vector<CodeReference>& references,
                    std::vector<NetworkElement>& elements, std::vector<std::string>& lines,
                    std::vector<std::string>& notes, std::vector<CodeStmt>* tree = nullptr,
                    std::vector<std::string>* treeNotes = nullptr);

// Completes the operands of the LAD and FBD networks of `code` with what
// `prog` knows: the address of a tag or data block member, the type of each
// pin, and the type of a literal from its pin. buildCode calls it.
void linkOperands(BlockCode& code, const ProgramData& prog);

// The entries of one stored part of a block's reference table.
void parseReferencePart(const std::string& xml, std::vector<CodeReference>& out);
// Where the table's entries link to objects: from the table's FilcMetaPayload
// document, the reference number of an entry, the kind of object (17 a tag,
// 4 an instruction, other numbers blocks and data blocks) and its position in
// the list of such links.
struct ReferenceLink {
    int64_t refId = 0;
    int64_t type = 0;
    size_t position = 0;
};
std::vector<ReferenceLink> parseReferenceLinks(const std::string& xml);
// Fills in CodeReference::text for the entries whose text depends on other
// entries (the index of an array element), and the network positions.
void finishReferences(std::vector<CodeReference>& refs, const std::vector<Network>& networks);

// "_x0031_0" -> "10": names and values the project stores with characters
// written as _xHHHH_.
std::string decodeXmlName(const std::string& s);

}  // namespace tia
