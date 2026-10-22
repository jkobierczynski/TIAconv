// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "programjson.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include "blocksource.hpp"

namespace tia {
namespace {

// A JSON writer: objects and arrays one entry per line, indented by two;
// a compact object or array (and everything in it) on one line.
class Json {
public:
    explicit Json(std::ostream& out) : out_(out) {}

    Json& key(const char* k) {
        separate();
        str(k);
        out_ << ": ";
        afterKey_ = true;
        return *this;
    }
    void beginObject(bool compact = false) { begin('{', compact); }
    void endObject() { end('}'); }
    void beginArray(bool compact = false) { begin('[', compact); }
    void endArray() { end(']'); }

    void value(const std::string& s) {
        before();
        str(s);
    }
    void value(const char* s) { value(std::string(s)); }
    // a text that may be absent: null when empty
    void text(const std::string& s) {
        if (s.empty()) null();
        else value(s);
    }
    void value(bool b) {
        before();
        out_ << (b ? "true" : "false");
    }
    void value(int64_t n) {
        before();
        out_ << n;
    }
    void value(uint64_t n) {
        before();
        out_ << n;
    }
    void value(int n) { value(static_cast<int64_t>(n)); }
    void null() {
        before();
        out_ << "null";
    }
    void strings(const std::vector<std::string>& v) {
        beginArray(true);
        for (const auto& s : v) value(s);
        endArray();
    }
    void finish() { out_ << "\n"; }

private:
    struct Level {
        bool compact;
        bool empty = true;
    };
    std::ostream& out_;
    std::vector<Level> stack_;
    bool afterKey_ = false;

    bool compact() const { return !stack_.empty() && stack_.back().compact; }

    void newline() {
        out_ << "\n";
        for (size_t i = 0; i < stack_.size(); ++i) out_ << "  ";
    }
    // before a key or an array entry: the comma and the line break
    void separate() {
        if (stack_.empty()) return;
        Level& l = stack_.back();
        if (!l.empty) out_ << ",";
        if (l.compact) {
            if (!l.empty) out_ << " ";
        } else {
            newline();
        }
        l.empty = false;
    }
    void before() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        separate();
    }
    void begin(char c, bool compactHere) {
        before();
        out_ << c;
        stack_.push_back(Level{compactHere || compact()});
    }
    void end(char c) {
        const Level l = stack_.back();
        stack_.pop_back();
        if (!l.empty && !l.compact) newline();
        out_ << c;
    }
    void str(const std::string& s) {
        out_ << '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out_ << "\\\""; break;
                case '\\': out_ << "\\\\"; break;
                case '\n': out_ << "\\n"; break;
                case '\r': out_ << "\\r"; break;
                case '\t': out_ << "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out_ << buf;
                    } else {
                        out_ << static_cast<char>(c);
                    }
            }
        }
        out_ << '"';
    }
};

void members(Json& j, const std::vector<BlockMember>& list, const MemberAccess& outer = MemberAccess()) {
    j.beginArray();
    for (const BlockMember& m : list) {
        j.beginObject(m.members.empty());
        j.key("name").value(m.name);
        j.key("data_type").text(m.dataType);
        j.key("section").text(m.section);
        j.key("offset");
        if (m.hasOffset) j.value(formatOffset(m));
        else j.null();
        j.key("offset_bits");
        if (m.hasOffset) j.value(static_cast<uint64_t>(m.offsetBits));
        else j.null();
        j.key("start_value");
        if (m.hasStartValue) j.value(m.startValue);
        else j.null();
        j.key("retain").value(isRetain(m));
        const MemberAccess a = memberAccess(m, outer);
        j.key("accessible_from_hmi").value(a.accessible);
        j.key("writable_from_hmi").value(a.writable);
        j.key("setpoint").value(a.setpoint);
        j.key("unresolved").value(m.unresolved);
        j.key("comment").text(m.comment);
        j.key("members");
        members(j, m.members, a);
        j.endObject();
    }
    j.endArray();
}

bool isFailSafe(const BlockInfo& b) {
    for (const std::string* s : {&b.language, &b.languageStored})
        if (s->compare(0, 2, "F_") == 0 || s->compare(0, 6, "Safety") == 0) return true;
    return false;
}

bool isGraph(const BlockInfo& b) {
    return b.language.find("GRAPH") != std::string::npos || b.languageStored.find("GRAPH") != std::string::npos;
}

struct Reason {
    std::string block, type, reason;
};

// What keeps a block from being simulated from the file.
std::vector<std::string> reasonsFor(const BlockInfo& info, const BlockCode* code) {
    std::vector<std::string> out;
    if (code && code->isProtected) {
        if (info.protection == "system") out.push_back("system protected: an instruction from a Siemens library");
        else if (code->protectedLater) out.push_back("know-how protected later; this version is not read");
        else out.push_back("know-how protected");
        return out;
    }
    if (isGraph(info)) out.push_back("GRAPH");
    if (isFailSafe(info)) out.push_back("fail-safe");
    if (!code) {
        out.push_back("code not found");
        return out;
    }
    if (!info.declarationsRead) out.push_back("interface not read");
    for (const Network& n : code->networks)
        if (n.content == "unread")
            out.push_back("network " + std::to_string(n.number) + " not read" +
                          (n.notes.empty() ? std::string() : ": " + n.notes.front()));
    for (const std::string& note : code->notes) out.push_back(note);
    return out;
}

void element(Json& j, const NetworkElement& e) {
    j.beginObject();
    j.key("uid").value(static_cast<uint64_t>(e.uid));
    j.key("kind").value(e.kind);
    j.key("name").value(e.name);
    j.key("ref_id");
    if (e.refId) j.value(e.refId);
    else j.null();
    j.key("instance").text(e.instance);
    j.key("instance_ref_id");
    if (e.instanceRefId) j.value(e.instanceRefId);
    else j.null();
    j.key("data_type").text(e.dataType);
    j.key("options");
    j.beginArray(true);
    for (const auto& o : e.options) {
        j.beginObject(true);
        j.key("name").value(o.first);
        j.key("value").value(o.second);
        j.endObject();
    }
    j.endArray();
    j.key("pins");
    j.beginArray();
    for (const auto& p : e.pins) {
        j.beginObject(p.operands.empty());
        j.key("name").value(p.name);
        j.key("direction").value(p.output ? "out" : "in");
        j.key("connected").strings(p.connected);
        j.key("operands");
        j.beginArray();
        for (const auto& o : p.operands) {
            j.beginObject(true);
            j.key("text").value(o.text);
            j.key("ref_id");
            if (o.refId) j.value(o.refId);
            else j.null();
            j.key("linked").value(o.linked);
            j.key("kind").text(o.kind);
            j.key("data_type").text(o.dataType);
            j.key("data_type_from_pin").value(o.dataTypeFromPin);
            j.key("data_block").text(o.dataBlock);
            j.key("address").text(o.address);
            j.key("pin_type").text(o.pinType);
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();
    j.endObject();
}

void blockCode(Json& j, const BlockCode& c) {
    j.beginObject();
    j.key("networks");
    j.beginArray();
    for (const Network& n : c.networks) {
        j.beginObject();
        j.key("number").value(static_cast<uint64_t>(n.number));
        j.key("network_id").value(n.networkId);
        j.key("title").text(n.title);
        j.key("comment").text(n.comment);
        j.key("language").text(n.language);
        j.key("content").value(n.content);
        j.key("lines");
        j.beginArray();
        for (const auto& l : n.lines) j.value(l);
        j.endArray();
        j.key("elements");
        j.beginArray();
        for (const auto& e : n.elements) element(j, e);
        j.endArray();
        j.key("notes").strings(n.notes);
        j.endObject();
    }
    j.endArray();
    j.key("references");
    j.beginArray();
    for (const CodeReference& r : c.references) {
        if (r.kind == "call interface" || r.kind == "expression") continue;
        j.beginObject(true);
        j.key("ref_id").value(r.refId);
        j.key("kind").value(r.kind);
        j.key("kind_stored").text(r.kindStored);
        j.key("text").value(r.text);
        j.key("data_type").text(r.dataType);
        j.key("data_block").text(r.container);
        j.key("declared_as").text(r.declaredAs);
        j.endObject();
    }
    j.endArray();
    j.endObject();
}

}  // namespace

std::vector<std::string> programPlcs(const ProgramData& prog) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& p) {
        if (!p.empty() && seen.insert(p).second) out.push_back(p);
    };
    for (const auto& b : prog.blockList) add(b.plc);
    for (const auto& t : prog.tags) add(t.plc);
    for (const auto& d : prog.blocks) add(d.plc);
    return out;
}

void writeProgramJson(std::ostream& out, const std::string& plc, const Inventory& inv, const ProgramData& prog,
                      const CodeData& code, const ProgramJsonContext& ctx) {
    Json j(out);
    j.beginObject();
    j.key("format").value("tiaconv-program");
    j.key("schema_version").value(kProgramSchemaVersion);
    j.key("tool");
    j.beginObject(true);
    j.key("name").value("tiaconv");
    j.key("version").value(ctx.toolVersion);
    j.endObject();
    j.key("source");
    j.beginObject(true);
    j.key("file").value(ctx.source);
    j.key("project").text(inv.project.name);
    j.key("save");
    if (ctx.shownSave) j.value(static_cast<uint64_t>(ctx.shownSave));
    else j.null();
    j.endObject();

    // the CPU, and the modules of its station and of the IO devices it controls
    const Device* station = nullptr;
    const Module* cpu = nullptr;
    for (const Device& d : inv.devices)
        for (const Module& m : d.modules)
            if (!cpu && m.kind == "controller" && m.name == plc) {
                station = &d;
                cpu = &m;
            }
    j.key("plc");
    j.beginObject();
    j.key("name").value(plc);
    j.key("station").text(station ? station->name : std::string());
    j.key("type").text(cpu ? cpu->typeName : std::string());
    j.key("order_number").text(cpu ? cpu->orderNumber : std::string());
    j.key("firmware").text(cpu ? cpu->firmware : std::string());
    j.endObject();
    j.key("io");
    j.beginArray();
    for (const Device& d : inv.devices) {
        bool controlled = &d == station;
        for (const Module& m : d.modules)
            if (m.ioController == plc) controlled = true;
        if (!controlled) continue;
        for (const Module& m : d.modules)
            for (const IoAddress& a : m.ioAddresses) {
                j.beginObject(true);
                j.key("station").value(d.name);
                j.key("module").value(m.name);
                j.key("item").text(a.item);
                j.key("area").value(a.input ? "I" : "Q");
                j.key("start_bit").value(a.startBit);
                j.key("length_bits").value(a.lengthBits);
                j.key("process_image").value(a.processImage);
                j.key("text").value(ioAddressText(a));
                j.endObject();
            }
    }
    j.endArray();

    j.key("tags");
    j.beginArray();
    for (const Tag& t : prog.tags) {
        if (t.plc != plc) continue;
        j.beginObject(true);
        j.key("name").value(t.name);
        j.key("table").text(t.table);
        j.key("data_type").text(t.dataType);
        j.key("address").text(t.address);
        j.key("comment").text(t.comment);
        j.endObject();
    }
    j.endArray();

    j.key("constants");
    j.beginArray();
    for (const Constant& c : prog.constants) {
        if (c.plc != plc) continue;
        j.beginObject(true);
        j.key("name").value(c.name);
        j.key("table").text(c.table);
        j.key("data_type").text(c.dataType);
        j.key("value").text(c.value);
        j.key("kind").text(c.kind);
        j.key("system").value(c.system);
        j.key("comment").text(c.comment);
        j.endObject();
    }
    j.endArray();

    std::vector<Reason> reasons;
    j.key("data_blocks");
    j.beginArray();
    for (const DataBlock& d : prog.blocks) {
        if (d.plc != plc) continue;
        j.beginObject();
        j.key("name").value(d.name);
        j.key("number");
        if (d.hasNumber) j.value(d.number);
        else j.null();
        j.key("kind").text(d.kind);
        j.key("instance_of").text(d.instanceOf);
        j.key("optimized");
        if (d.hasAccess) j.value(d.symbolicAccessOnly);
        else j.null();
        j.key("notes").strings(d.notes);
        j.key("members");
        members(j, d.members);
        j.endObject();
        if (d.members.empty() && !d.notes.empty())
            reasons.push_back({d.name, "DB", "members not read: " + d.notes.front()});
    }
    j.endArray();

    std::map<uint64_t, const BlockCode*> codeOf;
    for (const BlockCode& c : code.blocks) codeOf[c.blockId] = &c;

    j.key("organization_blocks");
    j.beginArray();
    for (const BlockInfo& b : prog.blockList) {
        if (b.plc != plc || b.type != "OB") continue;
        j.beginObject(true);
        j.key("name").value(b.name);
        j.key("number");
        if (b.hasNumber) j.value(b.number);
        else j.null();
        j.key("event").text(b.kind);
        j.key("priority");
        if (b.hasPriority) j.value(b.priority);
        else j.null();
        j.key("cycle_us");
        if (b.hasCycle) j.value(b.cycleMicroseconds);
        else j.null();
        j.key("phase_shift_us");
        if (b.hasCycle) j.value(b.phaseShiftMicroseconds);
        else j.null();
        j.endObject();
    }
    j.endArray();

    std::map<std::string, std::string> used;  // instructions: name -> version
    const InstructionVersions versions = instructionVersionsOf(prog, plc);
    j.key("blocks");
    j.beginArray();
    for (const BlockInfo& b : prog.blockList) {
        if (b.plc != plc || (b.type != "OB" && b.type != "FB" && b.type != "FC")) continue;
        auto ci = codeOf.find(b.id);
        const BlockCode* c = ci == codeOf.end() ? nullptr : ci->second;
        const std::vector<std::string> why = reasonsFor(b, c);
        for (const auto& r : why) reasons.push_back({b.name, b.type, r});
        const bool open = c && !c->isProtected;
        j.beginObject();
        j.key("type").value(b.type);
        j.key("name").value(b.name);
        j.key("number");
        if (b.hasNumber) j.value(b.number);
        else j.null();
        j.key("language").text(b.language);
        j.key("optimized");
        if (b.hasAccess) j.value(b.symbolicAccessOnly);
        else j.null();
        j.key("protection").text(b.protection);
        j.key("simulatable").value(why.empty());
        j.key("interface");
        if (open && b.declarationsRead) members(j, b.declarations);
        else j.null();
        j.key("code");
        if (open) blockCode(j, *c);
        else j.null();
        j.endObject();
        if (open)
            for (const CodeReference& r : c->references)
                if (r.kind == "instruction") {
                    const std::string name = r.name.empty() ? r.text : r.name;
                    auto v = versions.find(name);
                    std::string& slot = used[name];
                    if (v != versions.end()) slot = v->second;
                }
    }
    j.endArray();

    j.key("instructions");
    j.beginArray();
    for (const auto& u : used) {
        j.beginObject(true);
        j.key("name").value(u.first);
        j.key("version").text(u.second);
        j.endObject();
    }
    j.endArray();

    j.key("not_simulatable");
    j.beginArray();
    for (const Reason& r : reasons) {
        j.beginObject(true);
        j.key("block").value(r.block);
        j.key("type").value(r.type);
        j.key("reason").value(r.reason);
        j.endObject();
    }
    j.endArray();
    j.endObject();
    j.finish();
}

std::vector<std::string> writeProgramJsonFiles(const std::string& dir, const Inventory& inv, const ProgramData& prog,
                                               const CodeData& code, const ProgramJsonContext& ctx) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    if (ec) throw std::runtime_error("cannot create " + dir + ": " + ec.message());
    std::vector<std::string> written;
    std::set<std::string> names;
    for (const std::string& plc : programPlcs(prog)) {
        // two PLCs whose names differ only in case get two files on Windows too
        std::string name = safeFileName(plc);
        auto fold = [](std::string x) {
            for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return x;
        };
        while (!names.insert(fold(name)).second) name += "_";
        const fs::path p = fs::u8path(dir) / fs::u8path(name + ".json");
        std::ofstream f(p, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + p.u8string());
        writeProgramJson(f, plc, inv, prog, code, ctx);
        if (!f) throw std::runtime_error("cannot write " + p.u8string());
        written.push_back(p.u8string());
    }
    return written;
}

}  // namespace tia
