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
    struct Pin {
        std::string name;
        bool output = false;
        // What is at the pin: an operand as written in code, "power rail",
        // or the pin of another part as "<uid>.<pin>".
        std::vector<std::string> connected;
    };
    std::vector<Pin> pins;
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
    std::vector<NetworkElement> elements;
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
             std::vector<std::string>& notes);

// The lines of an STL network from its stored document. Returns false when
// the document is not STL statements.
bool stlText(const std::string& xml, const std::vector<CodeReference>& references, std::vector<std::string>& lines,
             std::vector<std::string>& notes);

// The parts of a LAD or FBD network from its stored document, and the
// listing. Returns false when the document is not such a network.
bool graphicNetwork(const std::string& xml, const std::vector<CodeReference>& references,
                    std::vector<NetworkElement>& elements, std::vector<std::string>& lines,
                    std::vector<std::string>& notes);

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
