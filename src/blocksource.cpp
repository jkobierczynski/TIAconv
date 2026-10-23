// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "blocksource.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace tia {

namespace {

// The parameter types as a source writes them (the project stores ANY,
// POINTER in capitals)
std::string sourceTypeName(const std::string& t) {
    static const char* const names[] = {"Any", "Pointer", "Variant", "Block_FB", "Block_FC", "Block_DB", "Block_SDB"};
    for (const char* n : names) {
        const std::string name = n;
        if (t.size() != name.size()) continue;
        bool same = true;
        for (size_t k = 0; k < t.size() && same; ++k)
            same = std::toupper(static_cast<unsigned char>(t[k])) == std::toupper(static_cast<unsigned char>(name[k]));
        if (same) return name;
    }
    return t;
}


const char kIndent[] = "   ";  // one level, as TIA Portal's sources indent

bool plainIdentifier(const std::string& s) {
    if (s.empty()) return false;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(c0) || c0 == '_')) return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_')) return false;
    return true;
}

// TIA Portal's keyword for a section of a block interface.
std::string sectionKeyword(const std::string& section) {
    if (section == "Input") return "VAR_INPUT";
    if (section == "Output") return "VAR_OUTPUT";
    if (section == "InOut") return "VAR_IN_OUT";
    if (section == "Static") return "VAR";
    if (section == "Temp") return "VAR_TEMP";
    if (section == "Constant") return "VAR CONSTANT";
    return std::string();
}

// The settings of a member that differ from the defaults, as a source
// writes them before the colon: " { ExternalWritable := 'False'}".
// A multi-instance: a member whose own members have sections (Input,
// Output, ...), the instance of a function block or of an instruction.
bool isInstance(const BlockMember& m) {
    for (const auto& c : m.members)
        if (!c.section.empty()) return true;
    return false;
}

// The library version of an instruction: TON_TIME is the TON instruction
// for the type Time.
std::string instructionVersion(const std::string& type, const InstructionVersions* versions) {
    if (!versions) return std::string();
    auto it = versions->find(type);
    if (it != versions->end()) return it->second;
    const size_t us = type.rfind('_');
    if (us != std::string::npos) {
        it = versions->find(type.substr(0, us));
        if (it != versions->end()) return it->second;
    }
    return std::string();
}

std::string memberAttributes(const BlockMember& m, const MemberAccess& a, const InstructionVersions* versions) {
    std::vector<std::string> parts;
    // An instruction's instance: its type is an instruction of the project
    // (TON_TIME for TON), or its members have sections. The members of an
    // IEC timer come from a system data type and have none.
    const bool unquoted = !m.dataType.empty() && m.dataType[0] != '"';
    const std::string version = unquoted ? instructionVersion(m.dataType, versions) : std::string();
    const bool instruction = unquoted && (!version.empty() || isInstance(m));
    const bool instance = instruction || isInstance(m);
    if (instruction) {
        parts.push_back("InstructionName := '" + m.dataType + "'");
        if (!version.empty()) parts.push_back("LibVersion := '" + version + "'");
    }
    if (!a.accessible) parts.push_back("ExternalAccessible := 'False'");
    if (!a.visible) parts.push_back("ExternalVisible := 'False'");
    if (!a.writable) parts.push_back("ExternalWritable := 'False'");
    // a multi-instance always carries the setpoint setting
    if (instance) parts.push_back(std::string("S7_SetPoint := '") + (a.setpoint ? "True" : "False") + "'");
    else if (a.setpoint) parts.push_back("S7_SetPoint := 'True'");
    if (parts.empty()) return std::string();
    std::string out = instruction ? " {" : " { ";
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "; " : "") + parts[i];
    return out + "}";
}

void memberLines(const std::vector<BlockMember>& members, const std::string& indent, bool attributes,
                 const MemberAccess& outer, const InstructionVersions* versions, std::vector<std::string>& out) {
    for (const auto& m : members) {
        const MemberAccess a = memberAccess(m, outer);
        std::string line =
            indent + sourceName(m.name) + (attributes ? memberAttributes(m, a, versions) : std::string()) + " : ";
        // A structure lists its members; a multi-instance, a system type or
        // a PLC data type is named by its type alone.
        const bool structure = m.dataType == "Struct" && !m.members.empty();
        if (structure) {
            line += "Struct";
            if (!m.comment.empty()) line += "   // " + m.comment;
            out.push_back(line);
            memberLines(m.members, indent + kIndent, attributes, a, versions, out);
            out.push_back(indent + "END_STRUCT;");
            continue;
        }
        line += sourceTypeName(m.dataType);
        if (m.hasStartValue) line += " := " + m.startValue;
        line += ";";
        if (!m.comment.empty()) line += "   // " + m.comment;
        out.push_back(line);
    }
}

std::string blockKeyword(const std::string& type) {
    if (type == "FB") return "FUNCTION_BLOCK";
    if (type == "FC") return "FUNCTION";
    if (type == "OB") return "ORGANIZATION_BLOCK";
    return std::string();
}

// A comment over several lines, each as a // line.
void commentLines(const std::string& text, const std::string& indent, std::vector<std::string>& out) {
    if (text.empty()) return;
    size_t from = 0;
    while (from <= text.size()) {
        size_t nl = text.find('\n', from);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(from, nl - from);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(indent + "//" + line);
        from = nl + 1;
    }
}

// An STL statement as the editor shows it ("A     \"ZZA\"") as a source
// writes it ("      A \"ZZA\";").
std::string stlSourceLine(const std::string& line) {
    if (line.empty()) return line;
    if (line.compare(0, 2, "//") == 0) return "      " + line;
    std::string s = line;
    // a label stands at the start of the line: "M001:      L \"Sel\";"
    std::string label;
    const size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 0 && s.find_first_of(" \"'", 0) > colon) {
        label = s.substr(0, colon + 1);
        const size_t rest = s.find_first_not_of(' ', colon + 1);
        if (rest == std::string::npos) return label;
        s = s.substr(rest);
    }
    const size_t gap = s.find(' ');
    if (gap != std::string::npos) {
        const size_t next = s.find_first_not_of(' ', gap);
        s = next == std::string::npos ? s.substr(0, gap) : s.substr(0, gap) + " " + s.substr(next);
    }
    // an inline comment stays after the semicolon
    std::string comment;
    const size_t c = s.find(" //");
    if (c != std::string::npos) {
        comment = s.substr(c);
        s = s.substr(0, c);
    }
    // an indirect address as the source writes it: DBW[ AR1, P#2.0]
    if (s.find(" [") != std::string::npos) {
        auto replaceAll = [&](const std::string& from, const std::string& to) {
            for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
        };
        replaceAll(" [ ", "[ ");
        replaceAll(" [", "[ ");
        replaceAll(" , ", ", ");
        replaceAll(" ]", "]");
    }
    return (label.empty() ? std::string("      ") : label + "      ") + s + ";" + comment;
}


}  // namespace

std::string safeFileName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|')
            out += '_';
        else
            out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.back() = '_';
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

InstructionVersions instructionVersionsOf(const ProgramData& prog, const std::string& plc) {
    // those the project keeps without a PLC (V19, V21), then the PLC's own
    InstructionVersions versions;
    for (const std::string& key : {std::string(), plc}) {
        auto v = prog.instructionVersions.find(key);
        if (v != prog.instructionVersions.end())
            for (const auto& kv : v->second) versions[kv.first] = kv.second;
    }
    return versions;
}

std::string sourceName(const std::string& name) { return plainIdentifier(name) ? name : "\"" + name + "\""; }

std::vector<std::string> declarationLines(const BlockInfo& info, const InstructionVersions* versions) {
    std::vector<std::string> out;
    // sections in the order a source has them
    for (const char* section : {"Input", "Output", "InOut", "Static", "Temp", "Constant"}) {
        std::vector<BlockMember> in;
        for (const auto& m : info.declarations)
            if (m.section == section) in.push_back(m);
        if (in.empty()) continue;
        // members that keep their value (retain) in runs of their own, in
        // the order they are declared
        const bool attributes = std::string(section) != "Temp" && std::string(section) != "Constant";
        for (size_t i = 0; i < in.size();) {
            const bool retain = isRetain(in[i]);
            size_t j = i;
            while (j < in.size() && isRetain(in[j]) == retain) ++j;
            out.push_back(std::string(kIndent) + sectionKeyword(section) + (retain ? " RETAIN" : " "));
            memberLines(std::vector<BlockMember>(in.begin() + static_cast<long>(i), in.begin() + static_cast<long>(j)),
                        std::string(kIndent) + kIndent, attributes, MemberAccess(), versions, out);
            out.push_back(std::string(kIndent) + "END_VAR");
            i = j;
        }
        out.push_back(std::string());
    }
    return out;
}

BlockSource blockSource(const BlockInfo& info, const BlockCode& code, const InstructionVersions* versions) {
    BlockSource s;
    const std::string keyword = blockKeyword(info.type);
    bool scl = false, stl = false, graphic = false, unread = false;
    for (const auto& n : code.networks) {
        if (n.content == "scl") scl = true;
        else if (n.content == "stl") stl = true;
        else if (n.content == "graphic") graphic = true;
        else if (n.content == "unread") unread = true;
    }
    if (info.language == "SCL") scl = true;
    else if (info.language == "STL") stl = true;
    if (code.isProtected) s.form = "protected";
    else if (unread) s.form = "unread";
    else if (scl && !stl && !graphic) s.form = "scl";
    else if (stl && !scl && !graphic) s.form = "stl";
    else s.form = "listing";
    const std::string ext = s.form == "scl" ? ".scl" : s.form == "stl" ? ".awl" : ".txt";
    s.fileName = safeFileName(info.name) + ext;
    std::vector<std::string>& out = s.lines;

    if (s.form == "protected") {
        out.push_back("// " + info.type + " \"" + info.name + "\" is know-how protected" +
                      (code.protectedLater ? " later in this project file" : "") +
                      ": its interface and code are not read.");
        return s;
    }
    if (s.form == "listing" || s.form == "unread")
        out.push_back("// " + (info.language.empty() ? std::string() : info.language + " ") +
                      "block in tiaconv's notation (TIA Portal has no source form for it)");

    // the header
    std::string head = keyword + " \"" + info.name + "\"";
    if (info.type == "FC") {
        std::string ret = "Void";
        for (const auto& m : info.declarations)
            if (m.section == "Return" && !m.dataType.empty()) ret = m.dataType;
        head += " : " + ret;
    }
    out.push_back(head);
    if (!info.title.empty()) out.push_back("TITLE = " + info.title);
    out.push_back(std::string("{ S7_Optimized_Access := '") + (info.symbolicAccessOnly ? "TRUE" : "FALSE") + "' }");
    // a header value that is not a plain name in single quotes
    auto headerValue = [](const std::string& v) { return plainIdentifier(v) ? v : "'" + v + "'"; };
    if (!info.author.empty()) out.push_back("AUTHOR : " + headerValue(info.author));
    if (!info.family.empty()) out.push_back("FAMILY : " + headerValue(info.family));
    if (!info.userId.empty()) out.push_back("NAME : " + headerValue(info.userId));
    out.push_back("VERSION : " + (info.version.empty() ? std::string("0.1") : info.version));
    commentLines(info.comment, "", out);
    for (auto& l : declarationLines(info, versions)) out.push_back(std::move(l));
    out.push_back(std::string());
    out.push_back("BEGIN");

    if (s.form == "scl") {
        for (const auto& n : code.networks) {
            for (const auto& l : n.lines) out.push_back("\t" + l);
            for (size_t i = 0; i < n.trailingEmptyLines; ++i) out.push_back("\t");
        }
    } else if (s.form == "stl") {
        for (const auto& n : code.networks) {
            out.push_back("NETWORK");
            out.push_back("TITLE = " + n.title);
            commentLines(n.comment, "", out);
            for (const auto& l : n.lines) out.push_back(stlSourceLine(l));
            // the empty lines the network keeps after its statements
            for (size_t i = 0; i < n.trailingEmptyLines; ++i) out.push_back(std::string());
        }
    } else {
        for (const auto& n : code.networks) {
            out.push_back("NETWORK " + std::to_string(n.number) + (n.title.empty() ? "" : "  " + n.title) + "  (" +
                          n.language + ")");
            commentLines(n.comment, "", out);
            if (n.content == "unread")
                for (const auto& note : n.notes) out.push_back("// not read: " + note);
            for (const auto& l : n.lines) out.push_back("      " + l);
        }
        out.push_back(std::string());
    }
    out.push_back("END_" + keyword);
    out.push_back(std::string());
    return s;
}

size_t writeBlockSources(const std::string& dir, const ProgramData& prog, const CodeData& code, bool bom) {
    namespace fs = std::filesystem;
    std::map<uint64_t, const BlockInfo*> infos;
    for (const auto& b : prog.blockList) infos[b.id] = &b;
    size_t written = 0;
    std::map<std::string, int> used;  // the same file name twice in one folder
    for (const auto& bc : code.blocks) {
        auto it = infos.find(bc.blockId);
        if (it == infos.end()) continue;
        const BlockInfo& info = *it->second;
        const InstructionVersions versions = instructionVersionsOf(prog, info.plc);
        BlockSource s = blockSource(info, bc, &versions);
        fs::path p = fs::u8path(dir) / fs::u8path(safeFileName(info.plc.empty() ? "PLC" : info.plc));
        // the folders of the project tree
        size_t from = 0;
        while (from < info.folder.size()) {
            size_t slash = info.folder.find('/', from);
            if (slash == std::string::npos) slash = info.folder.size();
            const std::string part = info.folder.substr(from, slash - from);
            if (!part.empty()) p /= fs::u8path(safeFileName(part));
            from = slash + 1;
        }
        std::error_code ec;
        fs::create_directories(p, ec);
        if (ec) throw std::runtime_error("cannot create " + p.u8string() + ": " + ec.message());
        std::string name = s.fileName;
        const std::string key = (p / fs::u8path(name)).u8string();
        if (used[key]++) {
            const size_t dot = name.rfind('.');
            name = name.substr(0, dot) + "_" + info.type + std::to_string(info.number) + name.substr(dot);
        }
        std::ofstream f(p / fs::u8path(name), std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + (p / fs::u8path(name)).u8string());
        if (bom) f << "\xEF\xBB\xBF";
        for (const auto& l : s.lines) f << l << "\r\n";
        if (!f) throw std::runtime_error("cannot write " + (p / fs::u8path(name)).u8string());
        ++written;
    }
    return written;
}

}  // namespace tia
