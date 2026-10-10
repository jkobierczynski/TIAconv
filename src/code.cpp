// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "code.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

#include "bytes.hpp"
#include "xml.hpp"

namespace tia {
namespace {

using Key = std::pair<uint32_t, uint64_t>;

constexpr int kMaxDepth = 64;
constexpr size_t kMaxLineWidth = 110;

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
        const bool digit = c >= '0' && c <= '9';
        if (!(letter || (digit && i > 0))) return false;
    }
    return true;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// A name inside an access path: as it is when it is a plain name, in quotes
// otherwise.
std::string memberName(const std::string& s) { return isIdentifier(s) ? s : quoted(s); }

bool parseInt(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

int64_t intAttr(const XmlNode& n, const char* name, int64_t fallback = 0) {
    const std::string* v = n.attr(name);
    int64_t out = 0;
    return v && parseInt(*v, out) ? out : fallback;
}

// The text of a stored document: the blob unpacked, without the byte-order
// mark. Empty when there is none or it cannot be unpacked.
std::string documentText(const Value* v) {
    if (!v || v->type != Value::Type::Bytes) return std::string();
    std::string out;
    if (!decodeBlob(v->s, out)) return std::string();
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return out;
}

std::string accessName(const std::string& stored, bool statesKind) {
    if (stored.empty()) return statesKind ? "" : "read";
    if (stored == "Read") return "read";
    if (stored == "Write") return "write";
    if (stored == "RW" || stored == "ReadWrite") return "read and write";
    if (stored == "Call") return "call";
    if (stored == "InstanceDB") return "single instance";
    if (stored == "Multiinstance") return "multiple instance";
    if (stored == "ArrayBoundary") return "array limit";
    if (stored == "None") return "";
    if (stored == "Jump") return "jump";
    if (stored == "Definition") return "definition";
    return stored;
}

std::string kindName(const std::string& stored) {
    if (stored == "GlobalAccess") return "data block member";
    if (stored == "InterfaceAccess") return "local";
    if (stored == "SimpleAccess") return "tag";
    if (stored == "LiteralConstant" || stored == "Constant") return "constant";
    if (stored == "LocalConstant") return "local constant";
    if (stored == "FBBlock" || stored == "FCBlock" || stored == "OBBlock") return "block";
    if (stored == "AufDBBlock") return "instance data block";
    if (stored == "DepDBBlock") return "data block";
    if (stored == "MultInstAccess") return "multi-instance";
    if (stored == "Instruction") return "instruction";
    if (stored == "BlockInterfaceInfo") return "call interface";
    if (stored == "Expression") return "expression";
    if (stored == "Label") return "label";
    if (stored == "Ident") return "undefined name";  // a name typed in the code that the project does not know
    return stored;
}

// The data type from "Bool:33554433:Bool" (kind, number, name).
std::string typeName(const std::string& stored) {
    size_t a = stored.find(':');
    if (a == std::string::npos) return stored;
    size_t b = stored.find(':', a + 1);
    if (b == std::string::npos) return stored.substr(0, a);
    std::string name = stored.substr(b + 1);
    return name.empty() && stored.compare(0, a, "Undef") != 0 ? stored.substr(0, a) : name;
}

std::vector<int64_t> intList(const std::string& s) {
    std::vector<int64_t> out;
    std::string cur;
    auto flush = [&] {
        int64_t v = 0;
        if (parseInt(cur, v)) out.push_back(v);
        cur.clear();
    };
    for (char c : s) {
        if (c == ',' || c == ' ' || c == ';') flush();
        else cur += c;
    }
    flush();
    return out;
}

// ---- the reference table ----

const CodeReference* findRef(const std::map<int64_t, size_t>& index, const std::vector<CodeReference>& refs,
                             int64_t refId) {
    auto it = index.find(refId);
    return it == index.end() ? nullptr : &refs[it->second];
}

std::string refText(const std::map<int64_t, size_t>& index, const std::vector<CodeReference>& refs,
                    const CodeReference& r, int depth);

std::string pathText(const std::map<int64_t, size_t>& index, const std::vector<CodeReference>& refs,
                     const CodeReference& r, const char* prefix, bool quoteFirst, int depth) {
    std::string out = prefix;
    for (size_t i = 0; i < r.path.size(); ++i) {
        const auto& step = r.path[i];
        if (i > 0) out += '.';
        out += i == 0 && quoteFirst ? quoted(step.name) : memberName(step.name);
        if (!step.index.empty()) {
            out += '[';
            for (size_t k = 0; k < step.index.size(); ++k) {
                if (k) out += ", ";
                const CodeReference* ix = findRef(index, refs, step.index[k]);
                out += ix && depth < 8 ? refText(index, refs, *ix, depth + 1) : "?";
            }
            out += ']';
        }
    }
    return out;
}

// How the entry is written in code.
std::string refText(const std::map<int64_t, size_t>& index, const std::vector<CodeReference>& refs,
                    const CodeReference& r, int depth) {
    std::string out;
    const std::string& k = r.kindStored;
    if (k == "GlobalAccess") {
        out = r.path.empty() ? quoted(r.name) : pathText(index, refs, r, "", true, depth);
    } else if (k == "InterfaceAccess" || k == "MultInstAccess") {
        out = r.path.empty() ? "#" + r.name : pathText(index, refs, r, "#", false, depth);
    } else if (k == "LiteralConstant" || k == "Constant") {
        out = r.name;
    } else if (k == "LocalConstant") {
        out = "#" + r.name;
    } else if (k == "Instruction" || k == "Expression" || k == "BlockInterfaceInfo" || k == "Label") {
        out = r.name;
    } else if (!r.name.empty()) {
        // a tag, a block, anything else with a name of its own
        out = r.scope == "Local" ? "#" + r.name : quoted(r.name);
        if (!r.path.empty() && r.scope != "Local" && k != "SimpleAccess") {
            // an access that starts at a named object the table does not know as a kind
            out = pathText(index, refs, r, "", true, depth);
        }
    } else if (!r.path.empty()) {
        out = pathText(index, refs, r, r.scope == "Local" ? "#" : "", r.scope != "Local", depth);
    }
    // part of a tag or variable: "b0" is byte 0 of it
    if (!r.modifier.empty() && r.modifier != "undef" && r.modifier != "None") {
        std::string m = r.modifier;
        for (char& c : m)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out += ".%" + m;
    }
    return out;
}

// ---- SCL ----

struct SclSymbol {
    bool hasRef = false;
    int64_t refId = 0;
    std::string name;
};

const char* sclToken(const std::string& element) {
    static const std::pair<const char*, const char*> table[] = {
        {"OpAs", ":="},   {"OpPa", "=>"},      {"FiSt", ";"},          {"BracO", "("},
        {"BracC", ")"},   {"BoxO", "["},       {"BoxC", "]"},          {"Dot", "."},
        {"Comma", ","},   {"Colon", ":"},      {"OpPl", "+"},          {"OpMi", "-"},
        {"OpMu", "*"},    {"OpDi", "/"},       {"OpG", ">"},           {"OpL", "<"},
        {"OpE", "="},     {"OpU", "<>"},       {"OpLE", "<="},         {"OpGE", ">="},
        {"OpAND", "AND"}, {"OpOR", "OR"},      {"OpNOT", "NOT"},       {"KwTHEN", "THEN"},
        {"KwELSE", "ELSE"}, {"KwELSIF", "ELSIF"}, {"KwENDIF", "END_IF"}, {"KwEndRegion", "END_REGION"},
        {"KwTO", "TO"},   {"KwDO", "DO"},      {"KwENDFOR", "END_FOR"}, {"KwOF", "OF"},
        {"KwENDC", "END_CASE"}, {"KwBY", "BY"}, {"BC", "(*"},          {"BCE", "*)"},
        {"KwENDW", "END_WHILE"}, {"KwUNTIL", "UNTIL"}, {"KwENDR", "END_REPEAT"}, {"LDots", ".."},
    };
    for (const auto& t : table)
        if (element == t.first) return t.second;
    return nullptr;
}

// Elements that only group other elements.
bool sclStructure(const std::string& element) {
    static const char* const names[] = {"RootStatements", "Statement", "Statements", "Expression", "Fold",
                                        "FctCa",          "InstCa",    "Param",      "CaseElem",   "CaseRange",
                                        "CaseSRange"};
    for (const char* n : names)
        if (element == n) return true;
    return false;
}

class SclWriter {
public:
    SclWriter(const SclContext& ctx, std::vector<std::string>& notes) : ctx_(ctx), notes_(notes) {
        if (ctx_.references)
            for (size_t i = 0; i < ctx_.references->size(); ++i) index_[(*ctx_.references)[i].refId] = i;
    }

    void symbols(const XmlNode& table) {
        for (const auto& s : table.children) {
            const std::string* id = s->attr("SymID");
            int64_t n = 0;
            if (!id || !parseInt(*id, n)) continue;
            SclSymbol sym;
            if (const std::string* r = s->attr("RefId")) sym.hasRef = parseInt(*r, sym.refId);
            if (const std::string* name = s->attr("Name")) sym.name = *name;
            else if (const std::string* bi = s->attr("BIName")) sym.name = *bi;
            else if (const std::string* lib = s->attr("BILibName")) sym.name = *lib;
            symbols_[n] = std::move(sym);
        }
    }

    void walk(const XmlNode& e, int depth) {
        if (depth > 200) {
            if (!tooDeep_) notes_.push_back("statements nested too deeply, text incomplete");
            tooDeep_ = true;
            return;
        }
        const std::string& t = e.name;
        if (t == "BL") {
            int64_t n = intAttr(e, "NumBLs", 1);
            if (n < 0) n = 0;
            if (n > 4096) n = 4096;
            text_.append(static_cast<size_t>(n), ' ');
            return;
        }
        if (t == "NL") {
            text_ += '\n';
            return;
        }
        if (t == "LC") {
            text_ += "//" + e.attrOr("TE", "");
            // older projects keep the line end inside the comment
            for (const auto& c : e.children) walk(*c, depth + 1);
            return;
        }
        if (t == "MLC") {
            multiLanguageComment(e);
            return;
        }
        const bool afterDot = afterDot_;
        afterDot_ = t == "Dot";
        if (const char* tok = sclToken(t)) {
            text_ += tok;
        } else if (t == "SymPa") {
            // the name of a parameter in a call; kept but not shown where the
            // source leaves it out
            if (const std::string* shown = e.attr("ODN")) text_ += *shown;
            else if (!e.attrIs("V", "0")) text_ += e.attrOr("FormalName", "");
        } else if (t == "SymVa" || t == "Sub" || t == "SymDB" || (!e.attr("TE") && e.attr("SyId"))) {
            text_ += symbolText(e, afterDot);
        } else if (const std::string* te = e.attr("TE")) {
            text_ += *te;
        } else if (!sclStructure(t)) {
            text_ += "{?" + t + "}";
            if (unknown_.insert(t).second) notes_.push_back("token of unknown kind '" + t + "', shown as {?" + t + "}");
        }
        for (const auto& c : e.children) walk(*c, depth + 1);
    }

    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        size_t from = 0;
        for (;;) {
            size_t nl = text_.find('\n', from);
            if (nl == std::string::npos) {
                out.push_back(text_.substr(from));
                break;
            }
            out.push_back(text_.substr(from, nl - from));
            from = nl + 1;
        }
        for (std::string& l : out)
            if (!l.empty() && l.back() == '\r') l.pop_back();
        while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    }

private:
    const SclContext& ctx_;
    std::vector<std::string>& notes_;
    std::map<int64_t, size_t> index_;
    std::map<int64_t, SclSymbol> symbols_;
    std::set<std::string> unknown_;
    std::string text_;
    bool tooDeep_ = false;
    bool unresolved_ = false;
    bool afterDot_ = false;

    std::string symbolText(const XmlNode& e, bool afterDot) {
        // Newer projects keep the name as it was written, which is older
        // than the current one when something was renamed since.
        if (const std::string* shown = e.attr("ODN")) {
            if (!afterDot)
                if (const CodeReference* r = symbolRef(e))
                    if (r->renamed && !r->text.empty()) return r->text;
            return *shown;
        }
        // Older ones only point to the reference table. An array element has
        // an entry of its own there, after the tokens that spell it out.
        if (e.attrIs("SI", "VarElem")) return std::string();
        int64_t id = 0;
        const std::string* sy = e.attr("SyId");
        if (sy && parseInt(*sy, id)) {
            auto it = symbols_.find(id);
            if (it != symbols_.end()) {
                if (it->second.hasRef && ctx_.references) {
                    if (const CodeReference* r = findRef(index_, *ctx_.references, it->second.refId)) {
                        // after a dot only the last name of the path is meant
                        if (afterDot && !r->path.empty()) return memberName(r->path.back().name);
                        if (!r->text.empty()) return r->text;
                    }
                }
                if (it->second.name == "CONVERT") {
                    // a conversion is written with its two types: INT_TO_CHAR
                    const std::string from = templateType(e.attrOr("Template0", ""), "src_type "),
                                      to = templateType(e.attrOr("Template1", ""), "dest_type ");
                    if (!from.empty() && !to.empty()) return from + "_TO_" + to;
                }
                if (!it->second.name.empty()) return it->second.name;
            }
        }
        if (!unresolved_) notes_.push_back("a name could not be resolved, shown as {?}");
        unresolved_ = true;
        return "{?}";
    }

    static std::string templateType(const std::string& value, const char* prefix) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (value.compare(0, n, prefix) != 0) return std::string();
        std::string out = value.substr(n);
        for (char& c : out)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return isIdentifier(out) ? out : std::string();
    }

    const CodeReference* symbolRef(const XmlNode& e) const {
        int64_t id = 0;
        const std::string* sy = e.attr("SyId");
        if (!sy || !parseInt(*sy, id) || !ctx_.references) return nullptr;
        auto it = symbols_.find(id);
        if (it == symbols_.end() || !it->second.hasRef) return nullptr;
        return findRef(index_, *ctx_.references, it->second.refId);
    }

    // A comment kept as a text in several languages, outside the tokens.
    void multiLanguageComment(const XmlNode& e) {
        int64_t n = intAttr(e, "DictId", 0);
        std::string body;
        bool found = false;
        if (ctx_.comments && n >= 1 && static_cast<size_t>(n) <= ctx_.comments->size()) {
            body = (*ctx_.comments)[static_cast<size_t>(n) - 1];
            found = true;
        }
        if (!found) notes_.push_back("text of comment " + std::to_string(n) + " not found");
        std::string plain;
        for (char c : body)
            if (c != '\r') plain += c;
        text_ += "(/*" + plain + "*/)";
    }
};

// ---- LAD and FBD ----

enum Prec { kOr = 1, kXor = 2, kAnd = 3, kCompare = 4, kNot = 5, kAtom = 6 };

struct Expr {
    std::string text;  // empty: power flows unconditionally
    int prec = kAtom;
};

struct Source {
    enum Type { None, Rail, Operand, Pin } type = None;
    uint64_t uid = 0;
    std::string pin;
};

struct GPart {
    uint64_t uid = 0;
    std::string kind;  // gate, call, instruction
    std::string name;
    std::string instance;
    std::string dataType;  // the type a box was set to
    std::vector<std::pair<std::string, std::string>> options;
    std::set<std::string> negated;
    std::vector<std::pair<std::string, Source>> inputs;
    std::vector<std::pair<std::string, uint64_t>> outputsToOperand;  // pin, operand uid
    std::set<std::string> outputPins;
    bool inlined = false;   // pure logic, written into the expressions that use it
    bool coil = false;
    bool consumed = false;
    int label = 0;
};

bool isCoil(const std::string& gate) { return gate.size() >= 4 && gate.compare(gate.size() - 4, 4, "Coil") == 0; }

const char* compareOperator(const std::string& gate) {
    if (gate == "Eq") return "=";
    if (gate == "Ne") return "<>";
    if (gate == "Gt") return ">";
    if (gate == "Lt") return "<";
    if (gate == "Ge") return ">=";
    if (gate == "Le") return "<=";
    return nullptr;
}

bool isInlineGate(const std::string& gate) {
    return gate == "Contact" || gate == "PContact" || gate == "NContact" || gate == "O" || gate == "A" ||
           gate == "X" || gate == "Not" || compareOperator(gate) != nullptr;
}

class Listing {
public:
    Listing(const std::vector<CodeReference>& refs, std::vector<std::string>& notes) : refs_(refs), notes_(notes) {
        for (size_t i = 0; i < refs.size(); ++i) index_[refs[i].refId] = i;
    }

    bool load(const XmlNode& root) {
        const XmlNode* parts = root.child("Parts");
        const XmlNode* wires = root.child("Wires");
        if (parts)
            for (const auto& p : parts->children) part(*p);
        if (wires)
            for (const auto& w : wires->children)
                if (w->name == "Wire") wire(*w);
        return true;
    }

    void write(std::vector<NetworkElement>& elements, std::vector<std::string>& lines) {
        int label = 0;
        for (GPart& p : parts_)
            if (!p.inlined) p.label = ++label;
        for (GPart& p : parts_) {
            if (p.inlined) continue;
            std::string s = statement(p);
            const std::string head = std::to_string(p.label) + ": ";
            // long statements one pin per line
            if (head.size() + s.size() > kMaxLineWidth && !lastPins_.empty()) {
                lines.push_back(head + lastHead_ + "(");
                for (size_t i = 0; i < lastPins_.size(); ++i)
                    lines.push_back(std::string(head.size() + 4, ' ') + lastPins_[i] +
                                    (i + 1 < lastPins_.size() ? "," : ")"));
            } else {
                lines.push_back(head + s);
            }
        }
        // logic that leads nowhere
        for (GPart& p : parts_) {
            if (!p.inlined || p.consumed) continue;
            const std::string pin = p.outputPins.empty() ? std::string("out") : *p.outputPins.begin();
            Source s;
            s.type = Source::Pin;
            s.uid = p.uid;
            s.pin = pin;
            std::set<uint64_t> before = consumedNow_;
            Expr e = expr(s, 0);
            lines.push_back("open: " + (e.text.empty() ? std::string("TRUE") : e.text));
        }
        for (const GPart& p : parts_) elements.push_back(element(p));
    }

private:
    const std::vector<CodeReference>& refs_;
    std::vector<std::string>& notes_;
    std::map<int64_t, size_t> index_;
    std::vector<GPart> parts_;
    std::map<uint64_t, size_t> partIndex_;
    std::map<uint64_t, std::string> operands_;  // operand uid -> text; empty: nothing entered
    std::set<uint64_t> consumedNow_;
    std::string lastHead_;
    std::vector<std::string> lastPins_;
    bool loop_ = false;

    std::string refTextOf(const XmlNode& n) const {
        const std::string* r = n.attr("RefId");
        int64_t id = 0;
        if (!r || !parseInt(*r, id)) return std::string();
        const CodeReference* ref = findRef(index_, refs_, id);
        return ref ? ref->text : "{?" + *r + "}";
    }

    void part(const XmlNode& p) {
        const uint64_t uid = static_cast<uint64_t>(intAttr(p, "UId", 0));
        if (p.name == "ORef") {
            operands_[uid] = refTextOf(p);
            return;
        }
        GPart g;
        g.uid = uid;
        if (p.name == "Part") {
            g.kind = "gate";
            g.name = p.attrOr("Gate", p.attrOr("Name", "?"));
            g.inlined = isInlineGate(g.name);
            g.coil = isCoil(g.name);
        } else if (p.name == "CRef") {
            g.kind = "call";
            if (const XmlNode* b = p.child("CodeBlock")) g.name = refTextOf(*b);
            if (g.name.empty()) g.name = "{?}";
        } else if (p.name == "LRef") {
            g.kind = "instruction";
            g.name = refTextOf(p);
            if (g.name.empty()) g.name = "{?}";
        } else {
            g.kind = "gate";
            g.name = p.name;
            notes_.push_back("part of unknown kind '" + p.name + "'");
        }
        for (const auto& a : p.attrs) {
            if (a.first == "UId" || a.first == "Gate" || a.first == "RefId" || a.first == "Name") continue;
            g.options.emplace_back(a.first, a.second);
        }
        for (const auto& c : p.children) {
            if (c->name == "Instance") {
                g.instance = refTextOf(*c);
            } else if (c->name == "TemplateValue") {
                g.options.emplace_back(c->attrOr("Name", "?"), c->text);
                if (c->attrIs("Type", "Type")) g.dataType = c->text;
            } else if (c->name == "Negated") {
                const std::string pin = c->attrOr("PinName", c->attrOr("Name", ""));
                g.negated.insert(pin);
                g.options.emplace_back("negated", pin);
            } else if (c->name != "CodeBlock") {
                g.options.emplace_back(c->name, c->attrOr("Name", c->text));
            }
        }
        partIndex_[uid] = parts_.size();
        parts_.push_back(std::move(g));
    }

    GPart* find(uint64_t uid) {
        auto it = partIndex_.find(uid);
        return it == partIndex_.end() ? nullptr : &parts_[it->second];
    }

    // A wire: what it comes from first, then everything it leads to.
    void wire(const XmlNode& w) {
        Source src;
        bool first = true;
        for (const auto& c : w.children) {
            const uint64_t uid = static_cast<uint64_t>(intAttr(*c, "UId", 0));
            if (first) {
                first = false;
                if (c->name == "Powerrail") {
                    src.type = Source::Rail;
                } else if (c->name == "OCon") {
                    src.type = Source::Operand;
                    src.uid = uid;
                } else if (c->name == "PCon") {
                    src.type = Source::Pin;
                    src.uid = uid;
                    src.pin = c->attrOr("PinName", "");
                    if (GPart* p = find(uid)) p->outputPins.insert(src.pin);
                }
                continue;
            }
            if (c->name == "PCon") {
                if (src.type == Source::None) continue;
                if (GPart* p = find(uid)) p->inputs.emplace_back(c->attrOr("PinName", ""), src);
            } else if (c->name == "OCon") {
                if (src.type == Source::Pin)
                    if (GPart* p = find(src.uid)) p->outputsToOperand.emplace_back(src.pin, uid);
            }
        }
    }

    std::string operandText(uint64_t uid) const {
        auto it = operands_.find(uid);
        return it == operands_.end() ? std::string() : it->second;
    }

    static Expr atom(const std::string& text) {
        Expr e;
        e.text = text;
        return e;
    }

    static std::string wrap(const Expr& e, int need) { return e.prec < need ? "(" + e.text + ")" : e.text; }

    // Joins with AND or OR. An operand of AND that is always true drops out;
    // an OR with such an operand is always true.
    static Expr combine(const std::vector<Expr>& in, int prec, const char* word) {
        std::vector<const Expr*> kept;
        for (const Expr& e : in) {
            if (e.text.empty()) {
                if (prec == kOr) return Expr{std::string(), kAtom};
                if (prec == kAnd) continue;
            }
            kept.push_back(&e);
        }
        if (kept.empty()) return Expr{std::string(), kAtom};
        if (kept.size() == 1) return *kept[0];
        Expr out;
        out.prec = prec;
        for (size_t i = 0; i < kept.size(); ++i) {
            if (i) out.text += std::string(" ") + word + " ";
            out.text += kept[i]->text.empty() ? "TRUE" : wrap(*kept[i], prec + (prec == kXor ? 1 : 0));
        }
        return out;
    }

    static Expr negate(const Expr& e) {
        Expr out;
        out.prec = kNot;
        out.text = "NOT " + (e.text.empty() ? std::string("TRUE") : wrap(e, kNot));
        return out;
    }

    const Source* inputOf(const GPart& p, const std::string& pin) const {
        for (const auto& in : p.inputs)
            if (in.first == pin) return &in.second;
        return nullptr;
    }

    Expr pinExpr(const GPart& p, const std::string& pin, int depth) {
        const Source* s = inputOf(p, pin);
        if (!s) return atom("{?}");
        Expr e = expr(*s, depth + 1);
        return p.negated.count(pin) ? negate(e) : e;
    }

    // What arrives over a wire, as an expression.
    Expr expr(const Source& s, int depth) {
        if (s.type == Source::Rail) return Expr{std::string(), kAtom};
        if (s.type == Source::Operand) {
            const std::string t = operandText(s.uid);
            return atom(t.empty() ? "{?}" : t);
        }
        GPart* p = find(s.uid);
        if (!p) return atom("{?}");
        if (depth > kMaxDepth) {
            if (!loop_) notes_.push_back("the wires form a loop or a very long chain, listing incomplete");
            loop_ = true;
            return atom("{...}");
        }
        if (p->inlined) {
            p->consumed = true;
            const std::string& g = p->name;
            if (g == "Contact") return combine({pinExpr(*p, "in", depth), pinExpr(*p, "operand", depth)}, kAnd, "AND");
            if (g == "Not") return negate(pinExpr(*p, "in", depth));
            // an edge contact: the edge of the operand, with its edge memory bit
            if (g == "PContact" || g == "NContact") {
                Expr edge = atom(std::string(g == "PContact" ? "P(" : "N(") + shown(pinExpr(*p, "operand", depth)) + ", " +
                                 shown(pinExpr(*p, "bit", depth)) + ")");
                return combine({pinExpr(*p, "pre", depth), edge}, kAnd, "AND");
            }
            if (const char* op = compareOperator(g)) {
                Expr c;
                c.prec = kCompare;
                c.text = wrap(pinExpr(*p, "in1", depth), kNot) + " " + op + " " + wrap(pinExpr(*p, "in2", depth), kNot);
                if (!inputOf(*p, "pre")) return c;
                return combine({pinExpr(*p, "pre", depth), c}, kAnd, "AND");
            }
            std::vector<Expr> in;
            for (const auto& i : p->inputs) in.push_back(pinExpr(*p, i.first, depth));
            if (g == "O") return combine(in, kOr, "OR");
            if (g == "X") return combine(in, kXor, "XOR");
            return combine(in, kAnd, "AND");
        }
        // power passes through a coil unchanged
        if (p->coil && s.pin == "out") return inputOf(*p, "in") ? pinExpr(*p, "in", depth) : atom("{?}");
        return atom("[" + std::to_string(p->label) + "]." + s.pin);
    }

    static std::string shown(const Expr& e) { return e.text.empty() ? "TRUE" : e.text; }

    std::string statement(GPart& p) {
        lastPins_.clear();
        lastHead_.clear();
        if (p.coil) {
            const Source* target = inputOf(p, "operand");
            std::string name = target ? shown(expr(*target, 0)) : "{?}";
            Expr in = inputOf(p, "in") ? expr(*inputOf(p, "in"), 0) : atom("{?}");
            if (p.negated.count("in")) in = negate(in);
            if (p.negated.count("operand")) in = negate(in);
            std::string left;
            if (p.name == "Coil") left = name;
            else if (p.name == "SCoil") left = "S(" + name + ")";
            else if (p.name == "RCoil") left = "R(" + name + ")";
            else left = p.name + "(" + name + ")";
            return left + " := " + shown(in);
        }
        std::string head;
        if (p.kind == "gate") {
            head = p.name;
            if (!p.dataType.empty()) head += "[" + p.dataType + "]";
        } else {
            head = p.name;
            if (!p.instance.empty()) head += ", " + p.instance;
        }
        std::vector<std::string> pins;
        for (const auto& in : p.inputs) {
            Expr e = expr(in.second, 0);
            // a box directly on the power rail: nothing to say about EN
            if (e.text.empty() && (in.first == "en" || in.first == "EN") && !p.negated.count(in.first)) continue;
            if (in.second.type == Source::Operand && operandText(in.second.uid).empty()) continue;
            if (p.negated.count(in.first)) e = negate(e);
            pins.push_back(in.first + " := " + shown(e));
        }
        for (const auto& o : p.outputsToOperand) {
            const std::string t = operandText(o.second);
            if (t.empty()) continue;
            pins.push_back(o.first + " => " + t);
        }
        std::string s = head + "(";
        for (size_t i = 0; i < pins.size(); ++i) s += (i ? ", " : "") + pins[i];
        s += ")";
        lastHead_ = head;
        lastPins_ = pins;
        return s;
    }

    NetworkElement element(const GPart& p) const {
        NetworkElement e;
        e.uid = p.uid;
        e.kind = p.kind;
        e.name = p.name;
        e.instance = p.instance;
        e.options = p.options;
        auto pinOf = [&](const std::string& name, bool output) -> NetworkElement::Pin& {
            for (auto& x : e.pins)
                if (x.name == name && x.output == output) return x;
            NetworkElement::Pin n;
            n.name = name;
            n.output = output;
            e.pins.push_back(std::move(n));
            return e.pins.back();
        };
        for (const auto& in : p.inputs) {
            std::string what;
            if (in.second.type == Source::Rail) what = "power rail";
            else if (in.second.type == Source::Operand) what = operandText(in.second.uid);
            else what = std::to_string(in.second.uid) + "." + in.second.pin;
            NetworkElement::Pin& pin = pinOf(in.first, false);
            if (!what.empty()) pin.connected.push_back(what);
        }
        for (const std::string& name : p.outputPins) pinOf(name, true);
        for (const auto& o : p.outputsToOperand) {
            const std::string t = operandText(o.second);
            if (!t.empty()) pinOf(o.first, true).connected.push_back(t);
        }
        // the parts an output leads to
        for (const GPart& other : parts_)
            for (const auto& in : other.inputs)
                if (in.second.type == Source::Pin && in.second.uid == p.uid)
                    pinOf(in.second.pin, true).connected.push_back(std::to_string(other.uid) + "." + in.first);
        return e;
    }
};

// ---- reading the project ----

class CodeBuilder {
public:
    CodeBuilder(const Project& p, const ProgramData& prog, const ProtectedVersions* protectedUpTo)
        : project_(p), meta_(p.meta()), prog_(prog), protectedUpTo_(protectedUpTo) {}

    CodeData run() {
        CodeData out;
        // the code blocks by id
        const Container& c = project_.container();
        std::map<uint64_t, Key> blocks;
        std::set<uint64_t> olderThanProtection;
        for (const auto& kv : c.latest()) {
            const Block& b = c.blocks()[kv.second];
            if (c.isSystem(b) || b.deleted()) continue;
            if (!derives(b.type, "CodeBlockData")) continue;
            blocks[b.id] = Key(b.type, b.id);
            if (protectedUpTo_) {
                auto p = protectedUpTo_->find(b.id);
                if (p != protectedUpTo_->end() && kv.second <= p->second) olderThanProtection.insert(b.id);
            }
        }
        const std::set<uint64_t> isProtected = protectedBlockIds(prog_);
        for (const BlockInfo& info : prog_.blockList) {
            if (info.type != "OB" && info.type != "FB" && info.type != "FC") continue;
            auto it = blocks.find(info.id);
            if (it == blocks.end()) continue;
            BlockCode code;
            code.blockId = info.id;
            code.plc = info.plc;
            code.type = info.type;
            code.hasNumber = info.hasNumber;
            code.number = info.number;
            code.name = info.name;
            code.language = info.language;
            code.protection = info.protection;
            ++out.stats.blocks;
            if (isProtected.count(info.id) || olderThanProtection.count(info.id)) {
                code.isProtected = true;
                code.protectedLater = !isProtected.count(info.id);
                ++out.stats.protectedBlocks;
                out.blocks.push_back(std::move(code));
                continue;
            }
            Object o;
            if (decodeKey(it->second, o)) read(o, code);
            else code.notes.push_back("block not readable");
            if (info.interfaceRead) declaredInstances(info, code);
            out.stats.networks += code.networks.size();
            out.stats.references += code.references.size();
            for (const Network& n : code.networks)
                if (n.content == "unread") ++out.stats.unreadNetworks;
            out.blocks.push_back(std::move(code));
        }
        return out;
    }

private:
    // The multi-instances an FB declares: the Static members of its
    // interface whose data type is an FB, alone or as an array. TIA Portal's
    // cross-reference lists them as a use of that FB by the member ("Data
    // type"), Multiple instance.
    void declaredInstances(const BlockInfo& info, BlockCode& code) {
        int64_t next = -1;
        for (const BlockMember& m : info.interfaceMembers) {
            if (m.section != "Static") continue;
            // "ZZFB", a library block without quotes (Filter_PT1), or an
            // array of either
            std::string fb = m.dataType;
            const size_t of = fb.rfind("] of ");
            if (fb.compare(0, 6, "Array[") == 0 && of != std::string::npos) fb = fb.substr(of + 5);
            if (fb.size() >= 2 && fb.front() == '"' && fb.back() == '"') fb = fb.substr(1, fb.size() - 2);
            const BlockInfo* type = nullptr;
            for (const BlockInfo& b : prog_.blockList)
                if (b.plc == info.plc && b.type == "FB" && b.name == fb) type = &b;
            if (!type) continue;
            CodeReference r;
            r.refId = next--;
            r.name = fb;
            r.currentName = fb;
            r.declaredAs = m.name;
            // as the calls are written: a block of the project in quotes, a
            // block of a Siemens library as an instruction
            if (type->system || type->protection == "system") {
                r.kind = "instruction";
                r.text = fb;
            } else {
                r.kind = "block";
                r.text = "\"" + fb + "\"";
                r.dataType = fb;
            }
            CodeUse u;
            u.access = "multiple instance";
            r.uses.push_back(u);
            code.references.push_back(std::move(r));
        }
    }

    const Project& project_;
    const MetaModel& meta_;
    const ProgramData& prog_;
    const ProtectedVersions* protectedUpTo_;
    std::map<uint32_t, bool> identTypes_;

    bool derives(uint32_t type, const char* shortName) const {
        const TypeDef* t = meta_.findById(type);
        return t && meta_.derivesFromShort(t->name, shortName);
    }

    bool decodeKey(const Key& k, Object& o) const {
        const Block* b = project_.live(k.first, k.second);
        if (!b) return false;
        try {
            return project_.decode(*b, o);
        } catch (const ParseError&) {
            return false;
        }
    }

    // Targets of the relations with this name in stored order; entries of
    // lists that do not say which relation they are count when their target
    // is of the given type. Empty positions are kept as {0, 0} when asked:
    // some lists are indexed by position.
    std::vector<Key> targets(const Object& o, const char* relation, const char* typeShort, bool gaps = false) const {
        std::vector<Key> out;
        for (const auto& r : o.relations) {
            const bool empty = !r.targetType && !r.targetId;
            if (r.relation) {
                const RelationDef* d = meta_.relation(r.relation);
                if (!d || d->name != relation) continue;
            } else {
                if (empty || !typeShort || !derives(r.targetType, typeShort)) continue;
            }
            if (empty && !gaps) continue;
            out.emplace_back(r.targetType, r.targetId);
        }
        return out;
    }

    // An object of the older way of keeping the reference table: one object
    // per entry.
    bool isIdentObject(uint32_t type) {
        auto it = identTypes_.find(type);
        if (it != identTypes_.end()) return it->second;
        bool yes = false;
        if (const TypeDef* t = meta_.findById(type)) {
            if (t->kind == TypeKind::ObjectType) {
                for (const LayoutSet& s : meta_.layout(*t)) {
                    const size_t dot = s.name.rfind('.');
                    if ((dot == std::string::npos ? s.name : s.name.substr(dot + 1)) == "IIdentData") yes = true;
                }
            }
        }
        identTypes_[type] = yes;
        return yes;
    }

    static bool intOf(const Value* v, int64_t& out) {
        if (!v) return false;
        if (v->type == Value::Type::Int) {
            out = v->i;
            return true;
        }
        uint64_t u = 0;
        if (!v->asUInt(u)) return false;
        out = static_cast<int64_t>(u);
        return true;
    }

    static std::string strOf(const Value* v) {
        return v && (v->type == Value::Type::String || v->type == Value::Type::Text) ? v->s : std::string();
    }

    void referenceFromObject(const Object& o, std::vector<CodeReference>& out) const {
        CodeReference r;
        if (!intOf(o.attr("IIdentData", "RefId"), r.refId)) return;
        std::string k = o.def ? o.def->shortName() : std::string();
        if (k.size() > 4 && k.compare(k.size() - 4, 4, "Data") == 0) k.erase(k.size() - 4);
        if (k.size() > 3 && k.compare(k.size() - 3, 3, "Acc") == 0) k += "ess";
        r.kindStored = k;
        r.name = strOf(o.attr("IIdentData", "Name"));
        r.scope = strOf(o.attr("IIdentData", "Scope"));
        if (const Value* op = o.attr("IOperandData", "SwType"))
            if (const Value* tn = op->field("TypeName")) r.dataType = strOf(tn);
        if (k == "Constant") r.name = strOf(o.attr("IConstantData", "StringValue"));
        if (const Value* path = o.attr("ISimaticStorageData", "AccessObj")) {
            if (path->type == Value::Type::List) {
                for (const Value& step : path->elements) {
                    CodeReference::Step s;
                    s.name = strOf(step.field("Name"));
                    if (const Value* ix = step.field("Index"))
                        if (ix->type == Value::Type::List)
                            for (const Value& i : ix->elements) {
                                int64_t n = 0;
                                if (intOf(&i, n)) s.index.push_back(n);
                            }
                    r.path.push_back(std::move(s));
                }
            }
        }
        r.modifier = strOf(o.attr("ISimaticStorageData", "AccessModifier"));
        if (const Value* uses = o.attr("IIdentXRefLocations", "IdentXRefLocationsArray")) {
            if (uses->type == Value::Type::List) {
                for (const Value& u : uses->elements) {
                    CodeUse use;
                    intOf(u.field("NetID"), use.networkId);
                    int64_t uid = 0;
                    if (intOf(u.field("UID"), uid)) use.uid = static_cast<uint64_t>(uid);
                    use.accessStored = strOf(u.field("AccessKind"));
                    use.access = accessName(use.accessStored, true);
                    if (const Value* h = u.field("XRefHidden")) use.hidden = h->truthy();
                    r.uses.push_back(std::move(use));
                }
            }
        }
        out.push_back(std::move(r));
    }

    // The lists of objects a reference table links to, with their empty
    // places: the tags, the blocks and data blocks, the instructions. Large
    // lists are kept without relation ids, one list per slot; they are told
    // apart by what they lead to.
    struct LinkLists {
        std::vector<Key> tags, programObjects, instructions;
    };

    LinkLists linkLists(const Object& container) {
        LinkLists out;
        out.tags = targets(container, "SimpleAccessDataToTagData", nullptr, true);
        out.programObjects = targets(container, "TypeOperandToProgramObj", nullptr, true);
        out.instructions = targets(container, "InstructionToInstrProxy", nullptr, true);
        std::map<uint32_t, std::vector<Key>> slots;
        for (const auto& r : container.relations)
            if (!r.relation) slots[r.slot].emplace_back(r.targetType, r.targetId);
        for (const auto& kv : slots) {
            uint32_t type = 0;
            for (const Key& k : kv.second)
                if (k.first) {
                    type = k.first;
                    break;
                }
            if (!type) continue;
            if (derives(type, "TagTableContentData") && out.tags.empty()) out.tags = kv.second;
            else if ((derives(type, "CodeBlockData") || derives(type, "DataBlockData")) && out.programObjects.empty())
                out.programObjects = kv.second;
        }
        return out;
    }

    std::string nameOfKey(const Key& k) {
        Object o;
        if (!(k.first || k.second) || !decodeKey(k, o)) return std::string();
        return o.attrString("ICoreAttributes", "Name");
    }

    // The current names of what the entries of one table refer to.
    void currentNames(const Object& container, std::vector<CodeReference>& refs, size_t first) {
        const std::string xml = documentText(container.attr("IIdentContainerData", "FilcMetaPayload"));
        if (xml.empty()) return;
        std::vector<ReferenceLink> links;
        try {
            links = parseReferenceLinks(xml);
        } catch (const ParseError&) {
            return;
        }
        if (links.empty()) return;
        const LinkLists lists = linkLists(container);
        std::map<int64_t, size_t> index;
        for (size_t i = first; i < refs.size(); ++i) index.emplace(refs[i].refId, i);
        for (const ReferenceLink& l : links) {
            auto it = index.find(l.refId);
            if (it == index.end()) continue;
            CodeReference& r = refs[it->second];
            const std::vector<Key>& list = l.type == 17 ? lists.tags : l.type == 4 ? lists.instructions : lists.programObjects;
            if (l.type == 4 || l.position >= list.size()) continue;
            const Key& target = list[l.position];
            // only where the link leads to the kind of object the entry is about
            const bool tag = r.kindStored == "SimpleAccess", program = r.kindStored == "FBBlock" ||
                             r.kindStored == "FCBlock" || r.kindStored == "OBBlock" ||
                             r.kindStored == "AufDBBlock" || r.kindStored == "DepDBBlock";
            if (!(tag && l.type == 17 && derives(target.first, "TagTableContentData")) &&
                !(program && l.type != 17 && (derives(target.first, "CodeBlockData") || derives(target.first, "DataBlockData"))))
                continue;
            r.currentName = nameOfKey(target);
        }
    }

    void readReferences(const Object& block, BlockCode& code) {
        // one document per kind of entry, kept by a table object of the block
        for (const Key& ck : targets(block, "CoreObject2IdentContainer", nullptr)) {
            Object container;
            if (!decodeKey(ck, container)) continue;
            const size_t first = code.references.size();
            for (const Key& pk : targets(container, "IdentParts", "IdentPartData")) {
                Object part;
                if (!decodeKey(pk, part)) continue;
                const std::string xml = documentText(part.attr("IIdentPartData", "PayLoad"));
                if (xml.empty()) continue;
                try {
                    parseReferencePart(xml, code.references);
                } catch (const ParseError&) {
                    code.notes.push_back("a part of the reference table is not readable");
                }
            }
            currentNames(container, code.references, first);
        }
        // older projects: one object per entry
        for (const auto& r : block.relations) {
            if (!r.targetType || !isIdentObject(r.targetType)) continue;
            if (r.relation) {
                const RelationDef* d = meta_.relation(r.relation);
                if (!d || d->name != "RelatedIdents") continue;
            }
            Object ident;
            if (decodeKey(Key(r.targetType, r.targetId), ident)) referenceFromObject(ident, code.references);
        }
    }

    void read(const Object& block, BlockCode& code) {
        size_t number = 0;
        std::vector<std::pair<Key, std::string>> documents;
        for (const Key& uk : targets(block, "Sources", "CompileUnitData")) {
            Object unit;
            if (!decodeKey(uk, unit)) continue;
            Network n;
            n.id = uk.second;
            n.number = ++number;
            int64_t ref = 0;
            if (intOf(unit.attr("ICompileUnitData", "RefID"), ref)) n.networkId = ref;
            n.languageStored = unit.attrString("ICompileUnitData", "ProgrammingLanguage");
            n.language = languageName(n.languageStored);
            const Value* title = unit.attr("ICoreAttributes", "Comment");
            if (title && title->type == Value::Type::Text) n.title = title->s;
            // the comment below the title is a text object of its own
            for (const Key& ck : targets(unit, "CompileUnitComment", nullptr)) {
                Object text;
                if (!decodeKey(ck, text)) continue;
                const Value* v = text.attr("ICoreTextRepository", "Text");
                if (v && v->type == Value::Type::Text) n.comment = v->s;
            }
            std::string doc;
            const std::string protection = unit.attrString("ICoreAttributes", "Protection");
            const Value* khp = unit.attr("ICoreAttributes", "IsKnowHowProtected");
            if ((!protection.empty() && protection != "NoProtection") || (khp && khp->truthy())) {
                n.content = "unread";
                n.notes.push_back("protected, not read");
            } else {
                doc = documentText(unit.attr("ICompileUnitData", "Data"));
            }
            code.networks.push_back(std::move(n));
            documents.emplace_back(uk, std::move(doc));
        }
        readReferences(block, code);
        finishReferences(code.references, code.networks);
        for (size_t i = 0; i < code.networks.size(); ++i) {
            Network& n = code.networks[i];
            if (n.content == "unread") continue;
            const std::string& doc = documents[i].second;
            if (doc.empty()) {
                n.content = "empty";
                continue;
            }
            content(documents[i].first, doc, code, n);
        }
    }

    void content(const Key& unitKey, const std::string& doc, const BlockCode& code, Network& n) {
        const size_t lt = doc.find('<');
        const bool scl = lt != std::string::npos && doc.compare(lt, 10, "<SCLSource") == 0;
        const bool graphic = lt != std::string::npos && doc.compare(lt, 7, "<FlgNet") == 0;
        if (scl) {
            // comments in several languages are texts of their own, numbered by position
            std::vector<std::string> comments;
            Object unit;
            if (decodeKey(unitKey, unit)) {
                for (const Key& ck : targets(unit, "ElementComments", nullptr, true)) {
                    std::string text;
                    Object t;
                    if ((ck.first || ck.second) && decodeKey(ck, t)) {
                        const Value* v = t.attr("ICoreTextRepository", "Text");
                        if (v && v->type == Value::Type::Text) text = v->s;
                    }
                    comments.push_back(std::move(text));
                }
            }
            SclContext ctx;
            ctx.references = &code.references;
            ctx.comments = &comments;
            if (sclText(doc, ctx, n.lines, n.notes)) {
                n.content = n.lines.empty() ? "empty" : "scl";
                return;
            }
        } else if (graphic) {
            if (graphicNetwork(doc, code.references, n.elements, n.lines, n.notes)) {
                n.content = n.elements.empty() ? "empty" : "graphic";
                return;
            }
        }
        const bool stl = lt != std::string::npos && doc.compare(lt, 11, "<Statements") == 0;
        if (stl && stlText(doc, code.references, n.lines, n.notes)) {
            n.content = n.lines.empty() ? "empty" : "stl";
            return;
        }
        n.content = "unread";
        if (!scl && !graphic && !stl) {
            std::string kind;
            if (lt != std::string::npos)
                for (size_t i = lt + 1; i < doc.size() && i < lt + 40; ++i) {
                    const char ch = doc[i];
                    if (ch == ' ' || ch == '>' || ch == '/' || ch == '\r' || ch == '\n') break;
                    kind += ch;
                }
            n.notes.push_back(kind.empty() ? "content of unknown form, not read"
                                           : "content of the form '" + kind + "' is not read");
        } else {
            n.notes.push_back("content not readable");
        }
    }
};

}  // namespace

std::string decodeXmlName(const std::string& s) {
    if (s.find("_x") == std::string::npos) return s;
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_' && i + 6 < s.size() && s[i + 1] == 'x' && s[i + 6] == '_') {
            unsigned cp = 0;
            bool hex = true;
            for (size_t k = i + 2; k < i + 6; ++k) {
                const char c = s[k];
                cp <<= 4;
                if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                else hex = false;
            }
            if (hex) {
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xc0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (cp & 0x3f));
                }
                i += 6;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

void parseReferencePart(const std::string& xml, std::vector<CodeReference>& out) {
    const std::unique_ptr<XmlNode> root = parseXml(xml);
    if (!root || root->name != "IdentXmlPart") return;
    for (const auto& e : root->children) {
        const XmlNode* id = e->child("ID");
        if (!id) continue;
        CodeReference r;
        r.kindStored = e->name;
        r.refId = intAttr(*id, "RID", 0);
        r.name = id->attrOr("N", "");
        r.scope = id->attrOr("S", "");
        if (const XmlNode* cs = id->child("CS")) {
            for (const auto& c : cs->children) {
                if (c->name != "C") continue;
                CodeUse u;
                u.networkId = intAttr(*c, "NID", 0);
                u.uid = static_cast<uint64_t>(intAttr(*c, "UID", 0));
                u.accessStored = c->attrOr("AK", "");
                u.hidden = c->attrIs("XH", "1");
                r.uses.push_back(std::move(u));
            }
        }
        if (const XmlNode* od = e->child("OD"))
            if (const XmlNode* td = od->child("TD")) r.dataType = typeName(td->attrOr("T", ""));
        if (const XmlNode* ssd = e->child("SSD")) {
            r.modifier = ssd->attrOr("AM", "");
            if (const XmlNode* aos = ssd->child("AOS")) {
                for (const auto& ao : aos->children) {
                    if (ao->name != "AO") continue;
                    CodeReference::Step s;
                    s.name = ao->attrOr("N", "");
                    s.index = intList(ao->attrOr("RIDI", ""));
                    r.path.push_back(std::move(s));
                }
            }
        }
        if (const XmlNode* bad = e->child("BAD")) r.dataBlockRef = intAttr(*bad, "BIRID", 0);
        // the interface of a called block: its parameters in order
        if (const XmlNode* biid = e->child("BIID"))
            if (const XmlNode* list = biid->child("BPIL"))
                for (const auto& bpi : list->children)
                    if (bpi->name == "BPI") r.parameters.push_back(bpi->attrOr("N", ""));
        if (const XmlNode* cd = e->child("CD")) {
            if (const XmlNode* cb = cd->child("CB")) {
                const std::string value = decodeXmlName(cb->attrOr("SV", ""));
                // a literal is written as it was entered; a named constant keeps its name
                if (r.name.empty()) r.name = value;
                if (r.dataType.empty()) r.dataType = cb->attrOr("T", "");
            }
        }
        out.push_back(std::move(r));
    }
}

std::vector<ReferenceLink> parseReferenceLinks(const std::string& xml) {
    std::vector<ReferenceLink> out;
    const std::unique_ptr<XmlNode> root = parseXml(xml);
    if (!root || root->name != "FILCMetaInfo") return out;
    for (const auto& list : root->children) {
        if (list->name != "FILC") continue;
        for (const auto& idx : list->children) {
            if (idx->name != "Idx") continue;
            const int64_t position = intAttr(*idx, "Value", -1);
            if (position < 0 || position > 1000000) continue;
            for (const auto& id : idx->children) {
                if (id->name != "Id") continue;
                ReferenceLink l;
                l.refId = intAttr(*id, "RefId", 0);
                l.type = intAttr(*id, "Type", 0);
                l.position = static_cast<size_t>(position);
                out.push_back(l);
            }
        }
    }
    return out;
}

void finishReferences(std::vector<CodeReference>& refs, const std::vector<Network>& networks) {
    std::map<int64_t, size_t> index;
    for (size_t i = 0; i < refs.size(); ++i) index.emplace(refs[i].refId, i);
    std::map<int64_t, size_t> position;
    for (const Network& n : networks) position.emplace(n.networkId, n.number);
    for (CodeReference& r : refs) {
        r.kind = kindName(r.kindStored);
        // only accesses can be told to be a reading one when nothing is stated
        const bool statesKind = !(r.kind == "data block member" || r.kind == "local" || r.kind == "tag" ||
                                  r.kind == "constant" || r.kind == "local constant" || r.kind == "data block");
        for (CodeUse& u : r.uses) {
            if (u.access.empty()) u.access = accessName(u.accessStored, statesKind || !u.accessStored.empty());
            auto it = position.find(u.networkId);
            u.network = it == position.end() ? 0 : it->second;
        }
    }
    // current names: of the object itself, and of the data block a member belongs to
    for (CodeReference& r : refs) {
        if (!r.currentName.empty() && r.currentName != r.name) {
            r.name = r.currentName;
            r.renamed = true;
        }
        if (r.kindStored == "GlobalAccess" && r.dataBlockRef && !r.path.empty()) {
            auto db = index.find(r.dataBlockRef);
            if (db != index.end() && !refs[db->second].currentName.empty() &&
                refs[db->second].currentName != r.path[0].name) {
                r.path[0].name = refs[db->second].currentName;
                r.renamed = true;
            }
        }
        if (r.kind == "data block member" && !r.path.empty()) r.container = r.path[0].name;
    }
    for (CodeReference& r : refs) r.text = refText(index, refs, r, 0);
    // an array index that is a renamed tag makes the access renamed too
    for (CodeReference& r : refs)
        for (const auto& step : r.path)
            for (int64_t ix : step.index) {
                auto it = index.find(ix);
                if (it != index.end() && refs[it->second].renamed) r.renamed = true;
            }
}

bool sclText(const std::string& xml, const SclContext& ctx, std::vector<std::string>& lines,
             std::vector<std::string>& notes) {
    std::unique_ptr<XmlNode> root;
    try {
        root = parseXml(xml);
    } catch (const ParseError&) {
        return false;
    }
    if (!root || root->name != "SCLSource") return false;
    SclWriter w(ctx, notes);
    if (const XmlNode* table = root->child("Symbols")) w.symbols(*table);
    const XmlNode* statements = root->child("RootStatements");
    if (!statements) return false;
    w.walk(*statements, 0);
    lines = w.lines();
    return true;
}

namespace {
bool hasLabel(const XmlNode& statement) {
    for (const auto& piece : statement.children)
        if (piece->name == "Label") return true;
    return false;
}
}  // namespace

bool stlText(const std::string& xml, const std::vector<CodeReference>& references, std::vector<std::string>& lines,
             std::vector<std::string>& notes) {
    std::unique_ptr<XmlNode> root;
    try {
        root = parseXml(xml);
    } catch (const ParseError&) {
        return false;
    }
    if (!root || root->name != "Statements") return false;
    std::map<int64_t, size_t> index;
    for (size_t i = 0; i < references.size(); ++i) index[references[i].refId] = i;
    std::set<std::string> unknown;
    bool unresolved = false;
    // One statement is one line: the instruction, then its operand in a
    // column of its own, as the editor shows them. The blanks the project
    // stores (NumBLs) are those that were typed; the editor ignores them.
    // A label goes in front, a comment at the end; the parameters of a
    // CALL follow on lines of their own.
    auto operand = [&](const XmlNode& piece) -> std::string {
        int64_t id = 0;
        const std::string* ref = piece.attr("RefId");
        const CodeReference* r = ref && parseInt(*ref, id) ? findRef(index, references, id) : nullptr;
        if (r && !r->text.empty()) return r->text;
        if (!unresolved) notes.push_back("a name could not be resolved, shown as {?}");
        unresolved = true;
        return "{?}";
    };
    auto unknownPiece = [&](const std::string& name) {
        if (unknown.insert(name).second)
            notes.push_back("piece of unknown kind '" + name + "', shown as {?" + name + "}");
        return "{?" + name + "}";
    };
    for (const auto& st : root->children) {
        if (st->name != "Statement") continue;
        std::string label, line, comment;
        std::vector<std::pair<std::string, std::string>> parameters;  // name, operand
        for (const auto& piece : st->children) {
            if (piece->name == "Label") {
                for (const auto& c : piece->children)
                    if (c->name == "OpdAccess") label += operand(*c);
                label += ":";
                continue;
            }
            if (piece->name == "LC") {
                comment = "//" + piece->attrOr("DispName", piece->attrOr("TE", ""));
                continue;
            }
            if (piece->name == "CallInfo") {
                int64_t id = 0;
                const std::string* ref = piece->attr("RefId");
                const CodeReference* callee = ref && parseInt(*ref, id) ? findRef(index, references, id) : nullptr;
                for (const auto& pe : piece->children) {
                    if (pe->name != "ParaExpression") continue;
                    const int64_t n = intAttr(*pe, "FPNum", 0);
                    std::string name = callee && n >= 1 && static_cast<size_t>(n) <= callee->parameters.size()
                                           ? callee->parameters[static_cast<size_t>(n - 1)]
                                           : "{?" + std::to_string(n) + "}";
                    std::string value;
                    for (const auto& c : pe->children) value += c->name == "OpdAccess" ? operand(*c) : unknownPiece(c->name);
                    parameters.emplace_back(name, value);
                }
                continue;
            }
            const std::string* kw = piece->attr("Kw");
            if (piece->name == "Token" && !piece->attr("DispName") && kw && *kw == ",") {
                line += ",";  // between the block and its instance: CALL "FB", "DB"
                continue;
            }
            // a labelled statement is written with single blanks: "M001: NOP 0"
            if (!line.empty()) line.append(line.size() < 6 && !hasLabel(*st) ? 6 - line.size() : 1, ' ');
            if (const std::string* shown = piece->attr("DispName")) {
                line += *shown;
            } else if (piece->attr("RefId")) {
                line += operand(*piece);
            } else if (const std::string* text = piece->attr("TE")) {
                line += *text;
            } else if (const std::string* text2 = piece->attr("Text")) {
                line += *text2;
            } else if (piece->name == "Token" && kw && !kw->empty() &&
                       kw->find_first_not_of("0123456789") != std::string::npos) {
                line += *kw;
            } else {
                line += unknownPiece(piece->name);
            }
        }
        if (!label.empty()) line = line.empty() ? label : label + " " + line;
        if (!comment.empty()) line = line.empty() ? comment : line + " " + comment;
        lines.push_back(std::move(line));
        // the parameters of a CALL as the editor lists them: the names padded
        // to one width, then := and the operand ("in1  :=\"ZZB\"")
        size_t width = 0;
        for (const auto& p : parameters) width = std::max(width, p.first.size());
        for (const auto& p : parameters)
            lines.push_back("      " + p.first + std::string(width + 1 - p.first.size(), ' ') + ":=" + p.second);
    }
    while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos) lines.pop_back();
    return true;
}

bool graphicNetwork(const std::string& xml, const std::vector<CodeReference>& references,
                    std::vector<NetworkElement>& elements, std::vector<std::string>& lines,
                    std::vector<std::string>& notes) {
    std::unique_ptr<XmlNode> root;
    try {
        root = parseXml(xml);
    } catch (const ParseError&) {
        return false;
    }
    if (!root || root->name != "FlgNet") return false;
    Listing l(references, notes);
    l.load(*root);
    l.write(elements, lines);
    return true;
}

std::set<uint64_t> protectedBlockIds(const ProgramData& prog) {
    std::set<uint64_t> out;
    for (const BlockInfo& b : prog.blockList) {
        if (b.protectionStored == "KnowHowProtection" || b.protectionStored == "SystemKnowHowProtection" ||
            b.protection.find("know-how") != std::string::npos || b.protection == "system")
            out.insert(b.id);
    }
    return out;
}

ProtectedVersions protectedVersions(const Project& wholeFile) {
    ProtectedVersions out;
    const Container& c = wholeFile.container();
    const MetaModel& meta = wholeFile.meta();
    std::map<uint32_t, bool> codeBlockType;
    for (size_t i = 0; i < c.blocks().size(); ++i) {
        const Block& b = c.blocks()[i];
        if (c.isSystem(b) || b.deleted()) continue;
        auto known = codeBlockType.find(b.type);
        if (known == codeBlockType.end()) {
            const TypeDef* t = meta.findById(b.type);
            known = codeBlockType.emplace(b.type, t && meta.derivesFromShort(t->name, "CodeBlockData")).first;
        }
        if (!known->second) continue;
        Object o;
        try {
            if (!wholeFile.decode(b, o)) continue;
        } catch (const ParseError&) {
            continue;
        }
        const std::string p = o.attrString("ICoreAttributes", "Protection");
        if (p == "KnowHowProtection" || p == "SystemKnowHowProtection") out[b.id] = i;
    }
    return out;
}

CodeData buildCode(const Project& project, const ProgramData& prog, const ProtectedVersions* protectedUpTo) {
    return CodeBuilder(project, prog, protectedUpTo).run();
}

}  // namespace tia
