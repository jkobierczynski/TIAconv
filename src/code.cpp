// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "code.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
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
    if (stored == "GlobalConstant") return "user constant";  // a named constant of a tag table
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

    // A name as the text shows it: of a block or instruction called, a
    // variable, a member after a dot.
    std::string nameOf(const XmlNode& e, bool afterDot = false) { return symbolText(e, afterDot); }

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
    // the empty lines lines() leaves off at the end (the text ends with them)
    size_t trailingEmptyLines() const {
        size_t n = 0;
        for (size_t i = text_.size(); i > 0 && (text_[i - 1] == '\n' || text_[i - 1] == '\r'); --i)
            if (text_[i - 1] == '\n') ++n;
        return n;
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


// ---- SCL as a tree ----

// The token document of SCL is a syntax tree already: statements and
// expressions with their kind (SI) and the data type of each expression's
// result. This turns it into CodeStmt and CodeExpr.
class SclTree {
public:
    SclTree(const SclContext& ctx, std::vector<std::string>& notes) : ctx_(ctx), notes_(notes), names_(ctx, nameNotes_) {}

    void symbols(const XmlNode& table) {
        names_.symbols(table);
        for (const auto& s : table.children) {
            int64_t id = 0;
            const std::string* sid = s->attr("SymID");
            if (!sid || !parseInt(*sid, id)) continue;
            Sym sym;
            if (const std::string* r = s->attr("RefId")) parseInt(*r, sym.refId);
            if (const std::string* t = s->attr("TypeSymID")) parseInt(*t, sym.typeSym);
            sym.name = s->attrOr("Name", s->attrOr("BIName", s->attrOr("BILibName", "")));
            sym.typ = s->attrOr("Typ", "");
            sym.kind = s->name == "Symbol" ? s->attrOr("ClID", "") : s->name;
            symbols_[id] = sym;
        }
    }

    void statements(const XmlNode& container, std::vector<CodeStmt>& out, int depth) {
        if (depth > kMaxTreeDepth) return deep();
        for (const XmlNode* c : significant(container)) {
            if (c->name == "Statement") out.push_back(statement(*c, depth + 1));
            else if (c->name == "Statements") statements(*c, out, depth + 1);
        }
    }

private:
    struct Sym {
        int64_t refId = 0;
        int64_t typeSym = 0;
        std::string name, typ, kind;
    };
    static constexpr int kMaxTreeDepth = 200;
    const SclContext& ctx_;
    std::vector<std::string>& notes_;
    std::vector<std::string> nameNotes_;  // the text already says what could not be named
    SclWriter names_;
    std::map<int64_t, Sym> symbols_;
    std::set<std::string> unknown_;
    bool deep_ = false;

    void deep() {
        if (!deep_) notes_.push_back("statements nested too deeply, tree incomplete");
        deep_ = true;
    }
    void unknown(const std::string& what) {
        if (unknown_.insert(what).second) notes_.push_back(what + " not read into the tree");
    }

    // The children that carry meaning: no blanks, line ends, comments or
    // the ";" of a statement, and the contents of a Fold in its place.
    static std::vector<const XmlNode*> significant(const XmlNode& e) {
        std::vector<const XmlNode*> out;
        collect(e, out, 0);
        return out;
    }
    static void collect(const XmlNode& e, std::vector<const XmlNode*>& out, int depth) {
        for (const auto& c : e.children) {
            const std::string& n = c->name;
            if (n == "BL" || n == "NL" || n == "LC" || n == "MLC" || n == "BC" || n == "BCE" || n == "BCL" ||
                n == "FiSt")
                continue;
            if (n == "Fold" && depth < 50) collect(*c, out, depth + 1);
            else out.push_back(c.get());
        }
    }

    const Sym* sym(const XmlNode& e) const {
        int64_t id = 0;
        const std::string* sy = e.attr("SyId");
        if (!sy || !parseInt(*sy, id)) return nullptr;
        auto it = symbols_.find(id);
        return it == symbols_.end() ? nullptr : &it->second;
    }
    int64_t refOf(const XmlNode& e) const {
        const Sym* s = sym(e);
        return s ? s->refId : 0;
    }

    void body(const XmlNode* statements, std::vector<CodeStmt>& out, int depth) {
        if (statements) this->statements(*statements, out, depth + 1);
    }

    CodeStmt statement(const XmlNode& st, int depth) {
        CodeStmt s;
        if (depth > kMaxTreeDepth) {
            deep();
            s.op = "unknown";
            return s;
        }
        const std::string si = st.attrOr("SI", "");
        const std::vector<const XmlNode*> parts = significant(st);
        auto exprsOf = [&]() {
            std::vector<const XmlNode*> v;
            for (const XmlNode* p : parts)
                if (p->name == "Expression") v.push_back(p);
            return v;
        };
        auto first = [&](const char* name) -> const XmlNode* {
            for (const XmlNode* p : parts)
                if (p->name == name) return p;
            return nullptr;
        };
        if (si == "STAss") {
            const auto e = exprsOf();
            s.op = "assign";
            if (e.size() == 2 && first("OpAs")) {
                s.exprs.push_back(expr(*e[0], depth));
                s.exprs.push_back(expr(*e[1], depth));
            } else {
                s.op = "unknown";
                s.name = "assignment";
                unknown("an assignment of an unknown form");
            }
        } else if (si == "STSub") {
            s.op = "call";
            for (const XmlNode* p : parts)
                if (p->name == "InstCa" || p->name == "FctCa") {
                    s.exprs.push_back(call(*p, depth));
                    break;
                }
            if (s.exprs.empty()) {
                s.op = "unknown";
                s.name = "call";
                unknown("a call of an unknown form");
            }
        } else if (si.compare(0, 2, "IF") == 0) {
            s.op = "if";
            // cond THEN body { ELSIF cond THEN body } [ ELSE body ] END_IF
            bool inElse = false;
            for (const XmlNode* p : parts) {
                if (p->name == "Expression" && !inElse) {
                    CodeStmt::Branch b;
                    b.exprs.push_back(expr(*p, depth));
                    s.branches.push_back(std::move(b));
                } else if (p->name == "Statements") {
                    if (inElse) body(p, s.elseBody, depth);
                    else if (!s.branches.empty()) body(p, s.branches.back().body, depth);
                } else if (p->name == "KwELSE") {
                    inElse = true;
                    s.hasElse = true;
                }
            }
        } else if (si == "STCAS") {
            s.op = "case";
            bool inElse = false;
            for (const XmlNode* p : parts) {
                if (p->name == "Expression" && s.exprs.empty()) {
                    s.exprs.push_back(expr(*p, depth));
                } else if (p->name == "CaseElem") {
                    CodeStmt::Branch b;
                    for (const XmlNode* q : significant(*p)) {
                        if (q->name == "CaseRange") b.exprs.push_back(caseLabel(*q, depth));
                        else if (q->name == "Statements") body(q, b.body, depth);
                    }
                    s.branches.push_back(std::move(b));
                } else if (p->name == "KwELSE") {
                    inElse = true;
                    s.hasElse = true;
                } else if (p->name == "Statements" && inElse) {
                    body(p, s.elseBody, depth);
                }
            }
        } else if (si == "STFOR") {
            s.op = "for";
            for (const XmlNode* e : exprsOf()) s.exprs.push_back(expr(*e, depth));
            body(first("Statements"), s.body, depth);
        } else if (si == "STWHI") {
            s.op = "while";
            for (const XmlNode* e : exprsOf()) s.exprs.push_back(expr(*e, depth));
            body(first("Statements"), s.body, depth);
        } else if (si == "STREP") {
            s.op = "repeat";
            for (const XmlNode* e : exprsOf()) s.exprs.push_back(expr(*e, depth));
            body(first("Statements"), s.body, depth);
        } else if (si == "STRET") {
            s.op = "return";
        } else if (si == "STSim") {
            s.op = "empty";
        } else if (si == "STRegion") {
            s.op = "region";
            if (const XmlNode* c = first("Const")) s.name = c->attrOr("TE", "");
            body(first("Statements"), s.body, depth);
        } else {
            const std::string te = st.attrOr("TE", "");
            if (te == "EXIT" || te == "CONTINUE") {
                s.op = te == "EXIT" ? "exit" : "continue";
            } else {
                s.op = "unknown";
                s.name = si.empty() ? te : si;
                unknown("a statement of the kind '" + s.name + "'");
            }
        }
        return s;
    }

    CodeExpr caseLabel(const XmlNode& range, int depth) {
        for (const XmlNode* q : significant(range)) {
            if (q->name == "CaseSRange") {
                CodeExpr r;
                r.op = "range";
                for (const XmlNode* x : significant(*q))
                    if (x->name == "Expression") r.args.push_back(expr(*x, depth));
                return r;
            }
            if (q->name == "Expression") return expr(*q, depth);
        }
        CodeExpr u;
        u.op = "unknown";
        u.name = "case label";
        return u;
    }

    static const char* binaryOp(const XmlNode& token) {
        static const std::pair<const char*, const char*> ops[] = {
            {"OpPl", "add"}, {"OpMi", "sub"}, {"OpMu", "mul"}, {"OpDi", "div"}, {"OpAND", "and"}, {"OpOR", "or"},
            {"OpXOR", "xor"}, {"OpE", "eq"}, {"OpU", "ne"}, {"OpG", "gt"}, {"OpL", "lt"}, {"OpGE", "ge"},
            {"OpLE", "le"}, {"OpMOD", "mod"}, {"OpExp", "pow"}, {"OpPow", "pow"}};
        for (const auto& o : ops)
            if (token.name == o.first) return o.second;
        const std::string te = token.attrOr("TE", "");
        if (te == "MOD") return "mod";
        if (te == "XOR") return "xor";
        if (te == "AND" || te == "&") return "and";
        if (te == "OR") return "or";
        if (te == "**") return "pow";
        return nullptr;
    }

    CodeExpr unknownExpr(const std::string& what) {
        CodeExpr u;
        u.op = "unknown";
        u.name = what;
        unknown("an expression of the kind '" + what + "'");
        return u;
    }

    CodeExpr expr(const XmlNode& e, int depth) {
        if (depth > kMaxTreeDepth) {
            deep();
            CodeExpr u;
            u.op = "unknown";
            return u;
        }
        ++depth;
        if (e.name != "Expression") return operand(e, depth);
        const std::string si = e.attrOr("SI", "");
        const std::vector<const XmlNode*> parts = significant(e);
        std::vector<const XmlNode*> subs;
        for (const XmlNode* p : parts)
            if (p->name == "Expression") subs.push_back(p);
        CodeExpr x;
        x.type = e.attrOr("Result", "");
        if (si == "ExprPrimD" || si == "ExprPrimC") {
            for (const XmlNode* p : parts) {
                CodeExpr o = operand(*p, depth);
                if (o.type.empty()) o.type = x.type;
                return o;
            }
            return unknownExpr(si);
        }
        if (si == "ExprPrimE") {  // ( ... )
            if (!subs.empty()) return expr(*subs[0], depth);
            return unknownExpr(si);
        }
        if (si == "ExprDot" || si == "ExprSelSlice" || si == "ExprInd") {
            if (subs.empty()) return unknownExpr(si);
            x.args.push_back(expr(*subs[0], depth));
            const XmlNode* last = nullptr;
            for (const XmlNode* p : parts)
                if (p->name == "SymVa") last = p;
            if (si == "ExprInd") {
                x.op = "index";
                for (size_t i = 1; i < subs.size(); ++i) x.args.push_back(expr(*subs[i], depth));
            } else {
                x.op = si == "ExprDot" ? "member" : "slice";
                if (last) x.name = last->attrOr("ODN", names_.nameOf(*last, true));
            }
            if (last) x.refId = refOf(*last);
            return x;
        }
        if (si == "ExprUnaryN" || si == "ExprUnaryM") {
            if (subs.empty()) return unknownExpr(si);
            bool minus = false;
            for (const XmlNode* p : parts)
                if (p->name == "OpMi") minus = true;
            if (si == "ExprUnaryM" && !minus) return expr(*subs[0], depth);  // a "+" in front
            x.op = si == "ExprUnaryN" ? "not" : "neg";
            x.args.push_back(expr(*subs[0], depth));
            return x;
        }
        // a binary operation: operands with the operators between them, left to right
        if (subs.size() >= 2) {
            std::vector<const char*> ops;
            for (const XmlNode* p : parts)
                if (p->name != "Expression") {
                    const char* o = binaryOp(*p);
                    if (!o) return unknownExpr(si + " " + p->name);
                    ops.push_back(o);
                }
            if (ops.size() != subs.size() - 1) return unknownExpr(si);
            CodeExpr acc = expr(*subs[0], depth);
            for (size_t i = 1; i < subs.size(); ++i) {
                CodeExpr b;
                b.op = ops[i - 1];
                b.type = i + 1 == subs.size() ? x.type : std::string();
                b.args.push_back(std::move(acc));
                b.args.push_back(expr(*subs[i], depth));
                acc = std::move(b);
            }
            return acc;
        }
        if (subs.size() == 1) return expr(*subs[0], depth);
        for (const XmlNode* p : parts)
            if (p->name == "FctCa" || p->name == "InstCa") return call(*p, depth);
        return unknownExpr(si);
    }

    CodeExpr operand(const XmlNode& p, int depth) {
        CodeExpr x;
        if (p.name == "FctCa" || p.name == "InstCa") return call(p, depth);
        if (p.name == "Const") {
            x.op = "const";
            x.literal = p.attrOr("TE", "");
            const Sym* s = sym(p);
            if (s && !s->typ.empty()) x.literalType = s->typ;
            else {
                const std::string si = p.attrOr("SI", "");
                if (si.compare(0, 5, "Const") == 0) x.literalType = si.substr(5);
            }
            x.type = x.literalType;
            return x;
        }
        if (p.name == "SymVa" || p.name == "Sub" || p.name == "SymDB") {
            const Sym* s = sym(p);
            if (s && s->kind == "BuiltInVar") {  // ENO of the block itself
                x.op = "builtin";
                x.name = s->name.empty() ? p.attrOr("ODN", "") : s->name;
                return x;
            }
            x.op = "ref";
            x.refId = refOf(p);
            x.name = p.attrOr("ODN", "");
            if (x.name.empty()) x.name = names_.nameOf(p);
            return x;
        }
        if (p.name == "Expression") return expr(p, depth);
        return unknownExpr(p.name);
    }

    std::string refText(int64_t id) const {
        if (!ctx_.references) return std::string();
        for (const CodeReference& r : *ctx_.references)
            if (r.refId == id) return r.text;
        return std::string();
    }

    CodeExpr call(const XmlNode& c, int depth) {
        CodeExpr x;
        x.op = "call";
        const std::vector<const XmlNode*> parts = significant(c);
        const XmlNode* db = nullptr;
        const XmlNode* sub = nullptr;
        for (const XmlNode* p : parts) {
            if (p->name == "SymDB" && !db) db = p;
            if (p->name == "Sub" && !sub) sub = p;
        }
        if (c.name == "InstCa") {
            if (db) {
                // "Instance_DB"(...) or "Instance_DB".TON(...): the data block, then what is called
                x.instanceRefId = refOf(*db);
                const Sym* d = sym(*db);
                if (sub) x.refId = refOf(*sub);
                if (!x.refId && d)
                    if (const Sym* t = symbolById(d->typeSym)) x.refId = t->refId;
            } else if (sub) {
                // a multi-instance: its symbol is the instance, its type what is called
                x.instanceRefId = refOf(*sub);
                if (const Sym* s = sym(*sub))
                    if (const Sym* t = symbolById(s->typeSym)) x.refId = t->refId;
            }
            x.name = x.refId ? refText(x.refId) : std::string();
            if (x.name.empty() && sub) x.name = sub->attrOr("ODN", "");
        } else if (sub) {
            x.refId = refOf(*sub);
            x.name = names_.nameOf(*sub);
        }
        for (const XmlNode* p : parts) {
            if (p->name == "Param") {
                CodeExpr a;
                const std::string si = p->attrOr("SI", "");
                a.op = si == "ParaOut" ? "out" : si == "ParaInOut" ? "inout" : "in";
                for (const XmlNode* q : significant(*p)) {
                    if (q->name == "SymPa") {
                        a.name = q->attrOr("FormalName", q->attrOr("ODN", ""));
                        // V13 keeps a name that needs quotes with them
                        if (a.name.size() >= 2 && a.name.front() == '"' && a.name.back() == '"')
                            a.name = a.name.substr(1, a.name.size() - 2);
                    }
                    else if (q->name == "Expression") a.args.push_back(expr(*q, depth));
                }
                x.args.push_back(std::move(a));
            } else if (p->name == "Expression") {
                // a parameter given by its position
                CodeExpr a;
                a.op = "in";
                a.args.push_back(expr(*p, depth));
                x.args.push_back(std::move(a));
            }
        }
        return x;
    }

    const Sym* symbolById(int64_t id) const {
        auto it = symbols_.find(id);
        return it == symbols_.end() ? nullptr : &it->second;
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
    int64_t refId = 0, instanceRefId = 0;
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

    // The network as statements, in the order the parts are evaluated: a
    // part after the parts whose outputs it reads.
    void tree(std::vector<CodeStmt>& out, std::vector<std::string>& notes) {
        treeNotes_ = &notes;
        prepareTree();
        for (size_t i : evaluationOrder()) {
            GPart& p = parts_[i];
            if (p.coil) coilStatement(p, out);
            else if (p.inlined) sharedStatement(p, out);
            else boxStatement(p, out);
        }
    }

private:
    const std::vector<CodeReference>& refs_;
    std::vector<std::string>& notes_;
    std::map<int64_t, size_t> index_;
    std::vector<GPart> parts_;
    std::map<uint64_t, size_t> partIndex_;
    std::map<uint64_t, std::string> operands_;  // operand uid -> text; empty: nothing entered
    std::map<uint64_t, int64_t> operandRefs_;   // operand uid -> entry of the reference table
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

    static int64_t refIdOf(const XmlNode& n) {
        const std::string* r = n.attr("RefId");
        int64_t id = 0;
        return r && parseInt(*r, id) ? id : 0;
    }

    void part(const XmlNode& p) {
        const uint64_t uid = static_cast<uint64_t>(intAttr(p, "UId", 0));
        if (p.name == "ORef") {
            operands_[uid] = refTextOf(p);
            operandRefs_[uid] = refIdOf(p);
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
            if (const XmlNode* b = p.child("CodeBlock")) {
                g.name = refTextOf(*b);
                g.refId = refIdOf(*b);
            }
            if (g.name.empty()) g.name = "{?}";
        } else if (p.name == "LRef") {
            g.kind = "instruction";
            g.name = refTextOf(p);
            g.refId = refIdOf(p);
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
                g.instanceRefId = refIdOf(*c);
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

    // What arrives over a wire, as an expression; NOT in front where the
    // output it comes from is negated.
    Expr expr(const Source& s, int depth) {
        Expr e = exprOf(s, depth);
        if (s.type == Source::Pin)
            if (const GPart* p = find(s.uid))
                if (p->negated.count(s.pin) && p->outputPins.count(s.pin)) return negate(e);
        return e;
    }

    Expr exprOf(const Source& s, int depth) {
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

    // ---- the tree ----
    //
    // The logic between the boxes (contacts, A, O, X, comparisons) is
    // written into the expressions of the parts that read it. Logic read by
    // more than one part is computed once, into a wire, as the PLC computes
    // it once at the branch: an edge contact must not be evaluated twice,
    // and a box between the two readers may change an operand it reads.
    // Logic that leads nowhere is evaluated for its edges.

    std::map<std::pair<uint64_t, std::string>, size_t> readers_;  // an output -> how many inputs read it
    std::set<uint64_t> nodes_;  // the parts that get statements of their own
    std::set<std::string> unknownTree_;
    std::vector<std::string>* treeNotes_ = nullptr;

    void prepareTree() {
        readers_.clear();
        nodes_.clear();
        for (const GPart& p : parts_)
            for (const auto& in : p.inputs)
                if (in.second.type == Source::Pin) ++readers_[{in.second.uid, in.second.pin}];
        for (const GPart& p : parts_) {
            if (!p.inlined) {
                nodes_.insert(p.uid);
                continue;
            }
            size_t total = 0;
            bool shared = false;
            for (const auto& r : readers_)
                if (r.first.first == p.uid) {
                    total += r.second;
                    if (r.second > 1) shared = true;
                }
            if (shared || total == 0) nodes_.insert(p.uid);
        }
    }
    size_t readersOf(uint64_t uid, const std::string& pin) const {
        auto it = readers_.find({uid, pin});
        return it == readers_.end() ? 0 : it->second;
    }
    bool isNode(const GPart& p) const { return nodes_.count(p.uid) != 0; }
    static bool negatedOut(const GPart& p, const std::string& pin) {
        return p.negated.count(pin) && p.outputPins.count(pin);
    }

    static CodeExpr node(const char* op) {
        CodeExpr e;
        e.op = op;
        return e;
    }
    static CodeExpr trueExpr() {
        CodeExpr e = node("const");
        e.literal = "TRUE";
        e.literalType = e.type = "Bool";
        return e;
    }
    static CodeExpr falseExpr() {
        CodeExpr e = trueExpr();
        e.literal = "FALSE";
        return e;
    }
    static CodeExpr wire(uint64_t uid, const std::string& pin) {
        CodeExpr w = node("wire");
        w.name = std::to_string(uid) + "." + pin;
        return w;
    }
    static bool isTrue(const CodeExpr& e) {
        if (e.op != "const") return false;
        std::string l = e.literal;
        for (char& c : l) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return l == "TRUE" || l == "BOOL#TRUE";
    }
    // An edge updates its memory bit: it has to be evaluated
    static bool sideEffects(const CodeExpr& e) {
        if (e.op == "edge_pos" || e.op == "edge_neg" || e.op == "call") return true;
        for (const auto& a : e.args)
            if (sideEffects(a)) return true;
        return false;
    }
    static CodeExpr unary(const char* op, CodeExpr a) {
        CodeExpr e = node(op);
        e.args.push_back(std::move(a));
        return e;
    }
    // AND drops an operand that is always true; OR with one is always true,
    // unless another operand has an edge to evaluate
    static CodeExpr logic(const char* op, std::vector<CodeExpr> in) {
        const bool isOr = std::string(op) == "or";
        bool anyTrue = false, effects = false;
        for (const auto& e : in) {
            anyTrue = anyTrue || isTrue(e);
            effects = effects || sideEffects(e);
        }
        if (isOr && anyTrue && !effects) return trueExpr();
        std::vector<CodeExpr> kept;
        for (auto& e : in) {
            if (isTrue(e) && std::string(op) == "and") continue;
            kept.push_back(std::move(e));
        }
        if (kept.empty()) return trueExpr();
        CodeExpr acc = std::move(kept[0]);
        for (size_t i = 1; i < kept.size(); ++i) {
            CodeExpr b = node(op);
            b.type = "Bool";
            b.args.push_back(std::move(acc));
            b.args.push_back(std::move(kept[i]));
            acc = std::move(b);
        }
        return acc;
    }

    // The inputs of a part in the order of their pins: in1, in2, ..., in10
    static std::vector<std::pair<std::string, Source>> pinOrder(const GPart& p) {
        auto key = [](const std::string& n) {
            size_t d = n.size();
            while (d > 0 && std::isdigit(static_cast<unsigned char>(n[d - 1]))) --d;
            const long num = d < n.size() ? std::strtol(n.c_str() + d, nullptr, 10) : -1;
            return std::make_pair(n.substr(0, d), num);
        };
        std::vector<std::pair<std::string, Source>> v = p.inputs;
        std::stable_sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return key(a.first) < key(b.first); });
        return v;
    }

    CodeExpr operandExpr(uint64_t uid) const {
        const std::string text = operandText(uid);
        if (text.empty()) return node("missing");
        auto r = operandRefs_.find(uid);
        const int64_t id = r == operandRefs_.end() ? 0 : r->second;
        const CodeReference* ref = id ? findRef(index_, refs_, id) : nullptr;
        CodeExpr e = node("ref");
        e.refId = id;
        e.name = text;
        if (ref) {
            e.type = ref->dataType;
            if (ref->kind == "constant") {
                e.op = "const";
                e.literal = ref->name.empty() ? text : ref->name;
                e.literalType = ref->dataType;
            }
        }
        return e;
    }

    // The value at an output of a part of the logic, from its inputs.
    CodeExpr partValue(GPart& p, int depth) {
        const std::string& g = p.name;
        if (g == "Contact") return logic("and", {pinTree(p, "in", depth), pinTree(p, "operand", depth)});
        if (g == "Not") return unary("not", pinTree(p, "in", depth));
        if (g == "PContact" || g == "NContact") {
            CodeExpr edge = node(g == "PContact" ? "edge_pos" : "edge_neg");
            edge.type = "Bool";
            edge.args.push_back(pinTree(p, "operand", depth));
            edge.args.push_back(pinTree(p, "bit", depth));
            return logic("and", {pinTree(p, "pre", depth), edge});
        }
        if (compareOperator(g)) {
            static const std::map<std::string, const char*> ops = {{"Eq", "eq"}, {"Ne", "ne"}, {"Gt", "gt"},
                                                                   {"Lt", "lt"}, {"Ge", "ge"}, {"Le", "le"}};
            CodeExpr c = node(ops.at(g));
            c.type = "Bool";
            c.args.push_back(pinTree(p, "in1", depth));
            c.args.push_back(pinTree(p, "in2", depth));
            if (!inputOf(p, "pre")) return c;
            return logic("and", {pinTree(p, "pre", depth), c});
        }
        std::vector<CodeExpr> in;
        for (const auto& i : pinOrder(p)) in.push_back(pinTree(p, i.first, depth));
        return logic(g == "O" ? "or" : g == "X" ? "xor" : "and", std::move(in));
    }

    CodeExpr treeOf(const Source& s, int depth) {
        if (s.type == Source::Rail) return trueExpr();
        if (s.type == Source::Operand) return operandExpr(s.uid);
        if (s.type != Source::Pin) return node("missing");
        GPart* p = find(s.uid);
        if (!p) return node("missing");
        if (depth > kMaxDepth) return node("unknown");
        CodeExpr e = isNode(*p) ? wire(p->uid, s.pin) : partValue(*p, depth + 1);
        // a negated output: the wire holds the value before the negation
        return negatedOut(*p, s.pin) ? unary("not", std::move(e)) : e;
    }

    CodeExpr pinTree(GPart& p, const std::string& pin, int depth) {
        const Source* s = inputOf(p, pin);
        if (!s) return node("missing");
        CodeExpr e = treeOf(*s, depth + 1);
        return p.negated.count(pin) && !p.outputPins.count(pin) ? unary("not", std::move(e)) : e;
    }

    // The parts with statements of their own whose outputs reach `p`.
    void sourcesOf(const GPart& p, std::set<size_t>& out, int depth) const {
        if (depth > kMaxDepth) return;
        for (const auto& in : p.inputs) {
            if (in.second.type != Source::Pin) continue;
            auto it = partIndex_.find(in.second.uid);
            if (it == partIndex_.end()) continue;
            const GPart& q = parts_[it->second];
            if (isNode(q)) out.insert(it->second);
            else sourcesOf(q, out, depth + 1);
        }
    }

    // The order on the screen, each part as soon as the parts it reads are
    // done: always the first such part.
    std::vector<size_t> evaluationOrder() const {
        std::vector<size_t> nodes;
        for (size_t i = 0; i < parts_.size(); ++i)
            if (isNode(parts_[i])) nodes.push_back(i);
        std::map<size_t, std::set<size_t>> before;
        for (size_t i : nodes) {
            sourcesOf(parts_[i], before[i], 0);
            before[i].erase(i);
        }
        std::vector<size_t> order;
        std::set<size_t> done;
        while (order.size() < nodes.size()) {
            bool picked = false;
            for (size_t i : nodes) {
                if (done.count(i)) continue;
                bool ready = true;
                for (size_t b : before[i])
                    if (!done.count(b)) ready = false;
                if (!ready) continue;
                order.push_back(i);
                done.insert(i);
                picked = true;
                break;
            }
            if (!picked) {  // a loop: the rest in the order on the screen
                treeNotes_->push_back("the parts form a loop, evaluation order as on the screen");
                for (size_t i : nodes)
                    if (!done.count(i)) {
                        order.push_back(i);
                        done.insert(i);
                    }
            }
        }
        return order;
    }

    static CodeStmt assign(CodeExpr target, CodeExpr value) {
        CodeStmt s;
        s.op = "assign";
        s.exprs.push_back(std::move(target));
        s.exprs.push_back(std::move(value));
        return s;
    }

    // Logic read by several parts, computed once; or leading nowhere,
    // evaluated for its edges.
    void sharedStatement(GPart& p, std::vector<CodeStmt>& out) {
        bool read = false;
        for (const std::string& pin : p.outputPins)
            if (readersOf(p.uid, pin)) {
                out.push_back(assign(wire(p.uid, pin), partValue(p, 0)));
                read = true;
                break;  // these parts have one output
            }
        if (read) return;
        CodeExpr v = partValue(p, 0);
        if (!sideEffects(v)) return;
        CodeStmt s;
        s.op = "eval";
        s.exprs.push_back(std::move(v));
        out.push_back(std::move(s));
    }

    void coilStatement(GPart& p, std::vector<CodeStmt>& out) {
        CodeExpr target = inputOf(p, "operand") ? treeOf(*inputOf(p, "operand"), 0) : node("missing");
        CodeExpr in = inputOf(p, "in") ? treeOf(*inputOf(p, "in"), 0) : node("missing");
        if (p.negated.count("in")) in = unary("not", std::move(in));
        // the power flows on past the coil unchanged: computed once, before
        // the coil writes its operand
        if (readersOf(p.uid, "out")) {
            out.push_back(assign(wire(p.uid, "out"), std::move(in)));
            in = wire(p.uid, "out");
        }
        if (p.name == "Coil") {
            if (p.negated.count("operand")) in = unary("not", std::move(in));
            out.push_back(assign(std::move(target), std::move(in)));
        } else if ((p.name == "SCoil" || p.name == "RCoil") && isTrue(in)) {
            out.push_back(assign(std::move(target), p.name == "RCoil" ? falseExpr() : trueExpr()));
        } else if (p.name == "SCoil" || p.name == "RCoil") {
            CodeStmt s;
            s.op = "if";
            CodeStmt::Branch b;
            b.exprs.push_back(std::move(in));
            b.body.push_back(assign(std::move(target), p.name == "RCoil" ? falseExpr() : trueExpr()));
            s.branches.push_back(std::move(b));
            out.push_back(std::move(s));
        } else {
            CodeStmt s;
            s.op = "unknown";
            s.name = p.name;
            if (unknownTree_.insert(p.name).second) treeNotes_->push_back("the coil '" + p.name + "' not read into the tree");
            out.push_back(std::move(s));
        }
    }

    // Where the value of an output goes. `direct` take the value: its
    // operands and a wire when other parts read it. A negated output goes
    // into its wire, and its operands get the negation (`after`).
    void targetsOf(const GPart& p, const std::string& pin, std::vector<CodeExpr>& direct,
                   std::vector<CodeStmt>& after) const {
        std::vector<CodeExpr> operands;
        for (const auto& o : p.outputsToOperand)
            if (o.first == pin && !operandText(o.second).empty()) operands.push_back(operandExpr(o.second));
        if (negatedOut(p, pin)) {
            if (operands.empty() && !readersOf(p.uid, pin)) return;
            direct.push_back(wire(p.uid, pin));
            for (auto& o : operands) after.push_back(assign(std::move(o), unary("not", wire(p.uid, pin))));
            return;
        }
        for (auto& o : operands) direct.push_back(std::move(o));
        if (readersOf(p.uid, pin)) direct.push_back(wire(p.uid, pin));
    }

    // value -> the first target; the others from it, so the value is
    // computed once
    static void assignAll(std::vector<CodeExpr> direct, CodeExpr value, std::vector<CodeStmt>& body) {
        if (direct.empty()) return;
        body.push_back(assign(direct[0], std::move(value)));
        for (size_t i = 1; i < direct.size(); ++i) body.push_back(assign(direct[i], direct[0]));
    }

    void boxStatement(GPart& p, std::vector<CodeStmt>& out) {
        static const std::map<std::string, const char*> arithmetic = {
            {"Add", "add"}, {"Sub", "sub"}, {"Mul", "mul"}, {"Div", "div"}, {"Mod", "mod"}};
        auto isEn = [](const std::string& n) { return n == "en" || n == "EN"; };
        auto isEno = [](const std::string& n) { return n == "eno" || n == "ENO"; };
        const char* enPin = inputOf(p, "en") ? "en" : inputOf(p, "EN") ? "EN" : nullptr;
        CodeExpr en = enPin ? pinTree(p, enPin, 0) : trueExpr();
        if (en.op == "missing") en = trueExpr();  // FBD: nothing at EN, the box always runs
        std::vector<CodeStmt> body;
        std::vector<std::string> outs(p.outputPins.begin(), p.outputPins.end());
        for (const auto& o : p.outputsToOperand)
            if (std::find(outs.begin(), outs.end(), o.first) == outs.end()) outs.push_back(o.first);
        std::vector<CodeExpr> enoDirect;
        std::vector<CodeStmt> enoAfter;
        for (const auto& pin : outs)
            if (isEno(pin)) targetsOf(p, pin, enoDirect, enoAfter);
        auto arith = arithmetic.find(p.name);
        // the boxes written as operations: ENO is TRUE when they ran (an
        // overflow is not reported); everything else is a call that sets ENO
        bool operation = false;
        if (p.kind == "gate" && (p.name == "PBox" || p.name == "NBox")) {
            // P_TRIG, N_TRIG: the edge of the power flow, with its edge memory
            operation = true;
            CodeExpr edge = node(p.name == "PBox" ? "edge_pos" : "edge_neg");
            edge.type = "Bool";
            edge.args.push_back(pinTree(p, "in", 0));
            edge.args.push_back(pinTree(p, "bit", 0));
            std::vector<CodeExpr> direct;
            std::vector<CodeStmt> after;
            for (const auto& pin : outs)
                if (!isEno(pin)) targetsOf(p, pin, direct, after);
            if (direct.empty()) {
                CodeStmt s;
                s.op = "eval";
                s.exprs.push_back(std::move(edge));
                body.push_back(std::move(s));
            }
            assignAll(std::move(direct), std::move(edge), body);
            for (auto& a : after) body.push_back(std::move(a));
        } else if (p.kind == "gate" && (p.name == "Move" || p.name == "S_Move" || arith != arithmetic.end())) {
            operation = true;
            CodeExpr value;
            if (arith == arithmetic.end()) {
                value = pinTree(p, "in", 0);
            } else {
                bool first = true;
                for (const auto& in : pinOrder(p)) {
                    if (isEn(in.first)) continue;
                    CodeExpr x = pinTree(p, in.first, 0);
                    if (first) {
                        value = std::move(x);
                        first = false;
                    } else {
                        CodeExpr b = node(arith->second);
                        b.type = p.dataType;
                        b.args.push_back(std::move(value));
                        b.args.push_back(std::move(x));
                        value = std::move(b);
                    }
                }
                if (first) value = node("missing");
            }
            // Move: each of out1, out2, ... gets the value
            for (const auto& pin : outs) {
                if (isEno(pin)) continue;
                std::vector<CodeExpr> direct;
                std::vector<CodeStmt> after;
                targetsOf(p, pin, direct, after);
                assignAll(std::move(direct), value, body);
                for (auto& a : after) body.push_back(std::move(a));
            }
        } else {
            // a block, an instruction, or a box tiaconv has no rule for: a call
            CodeExpr c = node("call");
            c.name = p.name;
            if (p.kind != "gate") {
                c.refId = p.refId;
                c.instanceRefId = p.instanceRefId;
            }
            std::vector<CodeStmt> after;
            // parameters as stored; for a box with numbered inputs in their order
            for (const auto& in : p.kind == "gate" ? pinOrder(p) : p.inputs) {
                if (isEn(in.first)) continue;
                if (in.second.type == Source::Operand && operandText(in.second.uid).empty()) continue;
                CodeExpr a = node("in");
                a.name = in.first;
                a.args.push_back(pinTree(p, in.first, 0));
                c.args.push_back(std::move(a));
            }
            for (const auto& pin : outs) {
                if (isEno(pin)) continue;
                std::vector<CodeExpr> direct;
                targetsOf(p, pin, direct, after);
                if (direct.empty()) continue;
                CodeExpr a = node("out");
                a.name = pin;
                a.args.push_back(direct[0]);
                c.args.push_back(std::move(a));
                for (size_t i = 1; i < direct.size(); ++i) after.push_back(assign(direct[i], direct[0]));
            }
            // the ENO the block or instruction sets
            if (!enoDirect.empty()) {
                CodeExpr a = node("out");
                a.name = "ENO";
                a.args.push_back(enoDirect[0]);
                c.args.push_back(std::move(a));
                for (size_t i = 1; i < enoDirect.size(); ++i) after.push_back(assign(enoDirect[i], enoDirect[0]));
                for (auto& x : enoAfter) after.push_back(std::move(x));
            }
            CodeStmt s;
            s.op = "call";
            s.exprs.push_back(std::move(c));
            body.push_back(std::move(s));
            for (auto& a : after) body.push_back(std::move(a));
            if (p.kind == "gate" && unknownTree_.insert(p.name).second)
                treeNotes_->push_back("the box '" + p.name + "' is written as a call of that name");
        }
        if (operation) {
            std::vector<CodeExpr> d = enoDirect;
            assignAll(std::move(d), trueExpr(), body);
            for (auto& x : enoAfter) body.push_back(x);
        }
        if (isTrue(en)) {
            for (auto& b : body) out.push_back(std::move(b));
            return;
        }
        // not run: ENO is FALSE, the outputs keep what they had
        CodeStmt s;
        s.op = "if";
        CodeStmt::Branch b;
        b.exprs.push_back(std::move(en));
        b.body = std::move(body);
        s.branches.push_back(std::move(b));
        if (!enoDirect.empty()) {
            s.hasElse = true;
            assignAll(enoDirect, falseExpr(), s.elseBody);
            for (auto& x : enoAfter) s.elseBody.push_back(std::move(x));
        }
        out.push_back(std::move(s));
    }

    NetworkElement::Operand operand(uint64_t uid) const {
        NetworkElement::Operand o;
        o.text = operandText(uid);
        auto r = operandRefs_.find(uid);
        if (r != operandRefs_.end()) o.refId = r->second;
        if (const CodeReference* ref = o.refId ? findRef(index_, refs_, o.refId) : nullptr) {
            o.linked = true;
            o.kind = ref->kind;
            o.kindStored = ref->kindStored;
            o.dataType = ref->dataType;
            o.dataBlock = ref->container;
        }
        return o;
    }

    NetworkElement element(const GPart& p) const {
        NetworkElement e;
        e.uid = p.uid;
        e.kind = p.kind;
        e.name = p.name;
        e.instance = p.instance;
        e.options = p.options;
        e.refId = p.refId;
        e.instanceRefId = p.instanceRefId;
        e.dataType = p.dataType;
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
            if (!what.empty() && in.second.type == Source::Operand) pin.operands.push_back(operand(in.second.uid));
        }
        for (const std::string& name : p.outputPins) pinOf(name, true);
        for (const auto& o : p.outputsToOperand) {
            const std::string t = operandText(o.second);
            if (t.empty()) continue;
            NetworkElement::Pin& pin = pinOf(o.first, true);
            pin.connected.push_back(t);
            pin.operands.push_back(operand(o.second));
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
            linkOperands(code, prog_);
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
                n.treeRead = true;  // nothing to run
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
            if (sclText(doc, ctx, n.lines, n.notes, &n.trailingEmptyLines)) {
                n.content = n.lines.empty() ? "empty" : "scl";
                n.treeRead = sclTree(doc, ctx, n.statements, n.treeNotes);
                return;
            }
        } else if (graphic) {
            if (graphicNetwork(doc, code.references, n.elements, n.lines, n.notes, &n.statements, &n.treeNotes)) {
                n.content = n.elements.empty() ? "empty" : "graphic";
                n.treeRead = true;
                return;
            }
        }
        const bool stl = lt != std::string::npos && doc.compare(lt, 11, "<Statements") == 0;
        if (stl && stlText(doc, code.references, n.lines, n.notes, &n.statements)) {
            n.content = n.lines.empty() ? "empty" : "stl";
            n.treeRead = true;
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
                                  r.kind == "constant" || r.kind == "local constant" || r.kind == "user constant" ||
                                  r.kind == "data block");
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
             std::vector<std::string>& notes, size_t* trailingEmptyLines) {
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
    if (trailingEmptyLines) *trailingEmptyLines = lines.empty() ? 0 : w.trailingEmptyLines();
    return true;
}

bool sclTree(const std::string& xml, const SclContext& ctx, std::vector<CodeStmt>& out,
             std::vector<std::string>& notes) {
    std::unique_ptr<XmlNode> root;
    try {
        root = parseXml(xml);
    } catch (const ParseError&) {
        return false;
    }
    if (!root || root->name != "SCLSource") return false;
    SclTree t(ctx, notes);
    if (const XmlNode* table = root->child("Symbols")) t.symbols(*table);
    const XmlNode* statements = root->child("RootStatements");
    if (!statements) return false;
    t.statements(*statements, out, 0);
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
             std::vector<std::string>& notes, std::vector<CodeStmt>* tree) {
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
    // the tree: one statement per line, the instruction in `name` and its
    // operands in `exprs`; a label as a statement of its own
    auto refExpr = [&](const XmlNode& piece) {
        CodeExpr e;
        int64_t id = 0;
        const std::string* ref = piece.attr("RefId");
        if (ref && parseInt(*ref, id)) {
            e.op = "ref";
            e.refId = id;
            if (const CodeReference* r = findRef(index, references, id)) {
                e.name = r->text;
                e.type = r->dataType;
                if (r->kind == "constant") {
                    e.op = "const";
                    e.literal = r->name.empty() ? r->text : r->name;
                    e.literalType = r->dataType;
                }
            }
        } else {
            e.op = "const";
            e.literal = piece.attrOr("DispName", piece.attrOr("TE", piece.attrOr("Text", piece.attrOr("Kw", ""))));
        }
        return e;
    };
    for (const auto& st : root->children) {
        if (st->name != "Statement") continue;
        std::string label, line, comment;
        std::vector<std::pair<std::string, std::string>> parameters;  // name, operand
        CodeStmt stl;
        stl.op = "stl";
        bool named = false;
        for (const auto& piece : st->children) {
            if (piece->name == "Label") {
                for (const auto& c : piece->children)
                    if (c->name == "OpdAccess") label += operand(*c);
                if (tree) {
                    CodeStmt l;
                    l.op = "label";
                    l.name = label;
                    tree->push_back(std::move(l));
                }
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
                    CodeExpr a;
                    a.op = "in";
                    a.name = name;
                    for (const auto& c : pe->children)
                        if (c->name == "OpdAccess") a.args.push_back(refExpr(*c));
                    stl.exprs.push_back(std::move(a));
                }
                continue;
            }
            const std::string* kw = piece->attr("Kw");
            if (piece->name == "Token" && !piece->attr("DispName") && kw && *kw == ",") {
                line += ",";  // between the block and its instance: CALL "FB", "DB"
                continue;
            }
            if (!named) {
                named = true;
                stl.name = piece->attrOr("DispName", piece->attrOr("TE", piece->attrOr("Text", kw ? *kw : std::string())));
            } else {
                stl.exprs.push_back(refExpr(*piece));
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
        if (tree && named) tree->push_back(std::move(stl));
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
                    std::vector<std::string>& notes, std::vector<CodeStmt>* tree,
                    std::vector<std::string>* treeNotes) {
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
    std::vector<std::string> ignored;
    if (tree) l.tree(*tree, treeNotes ? *treeNotes : ignored);
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

namespace {

bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// The size in bytes of an elementary type that an absolute address can
// name; 0 for anything else.
int addressBytes(const std::string& t) {
    if (t == "Byte" || t == "Char" || t == "SInt" || t == "USInt") return 1;
    if (t == "Word" || t == "Int" || t == "UInt" || t == "Date" || t == "S5Time") return 2;
    if (t == "DWord" || t == "DInt" || t == "UDInt" || t == "Real" || t == "Time" || t == "Time_Of_Day" || t == "TOD")
        return 4;
    return 0;
}

// The value type of an IEC timer or counter instance: TON_TIME -> Time,
// CTU_INT -> Int, IEC_TIMER -> Time; "" when not one of these.
std::string iecValueType(const std::string& instanceType, bool& timer) {
    static const std::map<std::string, std::pair<std::string, bool>> iec = {
        {"IEC_TIMER", {"Time", true}},       {"IEC_LTIMER", {"LTime", true}},     {"IEC_COUNTER", {"Int", false}},
        {"IEC_SCOUNTER", {"SInt", false}},   {"IEC_DCOUNTER", {"DInt", false}},   {"IEC_LCOUNTER", {"LInt", false}},
        {"IEC_UCOUNTER", {"UInt", false}},   {"IEC_USCOUNTER", {"USInt", false}}, {"IEC_UDCOUNTER", {"UDInt", false}},
        {"IEC_ULCOUNTER", {"ULInt", false}},
    };
    auto it = iec.find(instanceType);
    if (it != iec.end()) {
        timer = it->second.second;
        return it->second.first;
    }
    const size_t u = instanceType.rfind('_');
    if (u == std::string::npos) return std::string();
    const std::string head = instanceType.substr(0, u), tail = instanceType.substr(u + 1);
    static const std::map<std::string, std::string> types = {
        {"TIME", "Time"}, {"LTIME", "LTime"}, {"INT", "Int"},     {"SINT", "SInt"},   {"DINT", "DInt"},
        {"LINT", "LInt"}, {"UINT", "UInt"},   {"USINT", "USInt"}, {"UDINT", "UDInt"}, {"ULINT", "ULInt"},
    };
    auto t = types.find(tail);
    if (t == types.end()) return std::string();
    if (head == "TON" || head == "TOF" || head == "TP" || head == "TONR") {
        timer = true;
        return t->second;
    }
    if (head == "CTU" || head == "CTD" || head == "CTUD") {
        timer = false;
        return t->second;
    }
    return std::string();
}

class OperandLinker {
public:
    OperandLinker(BlockCode& code, const ProgramData& prog) : code_(code), prog_(prog) {
        for (size_t i = 0; i < code.references.size(); ++i) index_[code.references[i].refId] = i;
    }

    void run() {
        for (Network& n : code_.networks) {
            for (NetworkElement& e : n.elements) element(e);
            for (CodeStmt& s : n.statements) directions(s, 0);
        }
    }

private:
    BlockCode& code_;
    const ProgramData& prog_;
    std::map<int64_t, size_t> index_;

    const CodeReference* ref(int64_t id) const {
        auto it = index_.find(id);
        return it == index_.end() ? nullptr : &code_.references[it->second];
    }

    // A LAD or FBD call wires an in-out parameter like an input: the called
    // block's interface says which it is.
    void directions(CodeStmt& s, int depth) {
        if (depth > 200) return;
        // STL: CALL "FB", "DB" with its parameters
        if (s.op == "stl" && s.name == "CALL" && !s.exprs.empty() && s.exprs[0].op == "ref")
            setDirections(s.exprs[0].refId, s.exprs);
        for (CodeExpr& e : s.exprs) directions(e, depth + 1);
        for (auto& b : s.branches) {
            for (CodeExpr& e : b.exprs) directions(e, depth + 1);
            for (CodeStmt& x : b.body) directions(x, depth + 1);
        }
        for (CodeStmt& x : s.body) directions(x, depth + 1);
        for (CodeStmt& x : s.elseBody) directions(x, depth + 1);
    }
    void directions(CodeExpr& e, int depth) {
        if (depth > 200) return;
        for (CodeExpr& a : e.args) directions(a, depth + 1);
        if (e.op != "call" || !e.refId) return;
        setDirections(e.refId, e.args, e.instanceRefId);
    }
    // From the called block's interface; for an instruction, which has
    // none in the project, from the members of its instance.
    void setDirections(int64_t calledRef, std::vector<CodeExpr>& params, int64_t instanceRef = 0) {
        const std::vector<BlockMember>* members = nullptr;
        if (const CodeReference* called = ref(calledRef)) {
            const std::string& name = called->dataType.empty() ? called->name : called->dataType;
            for (const BlockInfo& b : prog_.blockList)
                if (b.plc == code_.plc && b.name == name && b.declarationsRead) members = &b.declarations;
        }
        if (!members && instanceRef) members = instanceMembers(instanceRef);
        if (!members) return;
        for (CodeExpr& a : params) {
            if (a.op != "in") continue;
            for (const BlockMember& m : *members)
                if (sameName(m.name, a.name)) {
                    if (m.section == "InOut") a.op = "inout";
                    else if (m.section == "Output") a.op = "out";
                }
        }
    }
    // The members of an instance: of an instance data block, or of a
    // multi-instance in this block's interface.
    const std::vector<BlockMember>* instanceMembers(int64_t instanceRef) const {
        const CodeReference* r = ref(instanceRef);
        if (!r) return nullptr;
        if (r->kind == "instance data block") {
            const std::string name = r->name.empty() ? r->text : r->name;
            for (const DataBlock& d : prog_.blocks)
                if (d.plc == code_.plc && (d.name == name || "\"" + d.name + "\"" == name)) return &d.members;
            return nullptr;
        }
        const std::string member = !r->path.empty() ? r->path[0].name : r->name;
        for (const BlockInfo& b : prog_.blockList)
            if (b.id == code_.blockId && b.declarationsRead)
                for (const BlockMember& m : b.declarations)
                    if (m.name == member) return &m.members;
        return nullptr;
    }

    static bool isConstant(const NetworkElement::Operand& o) {
        return o.kind == "constant" || o.kind == "local constant" || o.kind == "user constant";
    }

    void element(NetworkElement& e) {
        std::string boxType = e.dataType, outType;
        for (const auto& opt : e.options)
            if (opt.first == "DestType" && !opt.second.empty()) outType = opt.second;
        bool automatic = false;
        for (const auto& opt : e.options)
            if (opt.first == "AutomaticTyped") automatic = true;
        // first the addresses, then the pin types: an automatic box takes its
        // type from the operands at its data pins
        for (auto& pin : e.pins)
            for (auto& o : pin.operands) address(o);
        if (boxType.empty() && automatic && e.kind == "gate")
            for (const auto& pin : e.pins)
                if (dataPin(pin.name))
                    for (const auto& o : pin.operands)
                        if (boxType.empty() && o.linked && !isConstant(o) && !o.dataType.empty()) boxType = o.dataType;
        for (auto& pin : e.pins) {
            const std::string t = pinType(e, pin, boxType, outType);
            for (auto& o : pin.operands) {
                o.pinType = t;
                if (o.dataType.empty() && !t.empty() && (isConstant(o) || !o.linked)) {
                    o.dataType = t;
                    o.dataTypeFromPin = true;
                }
            }
        }
    }

    static bool dataPin(const std::string& pin) {
        return startsWith(pin, "in") || startsWith(pin, "out");
    }

    std::string pinType(const NetworkElement& e, const NetworkElement::Pin& pin, const std::string& boxType,
                        const std::string& outType) const {
        const std::string& p = pin.name;
        if (sameName(p, "en") || sameName(p, "eno")) return "Bool";
        if (e.kind == "gate") {
            const std::string& g = e.name;
            if (g == "Contact" || g == "PContact" || g == "NContact" || isCoil(g) || g == "A" || g == "O" ||
                g == "X" || g == "Not" || g == "PBox" || g == "NBox")
                return "Bool";
            if (compareOperator(g)) return p == "pre" || p == "out" ? "Bool" : boxType;
            if (g == "Move") return std::string();
            if (startsWith(p, "out") && !outType.empty()) return outType;
            return dataPin(p) ? boxType : std::string();
        }
        if (e.kind == "instruction") {
            const CodeReference* inst = ref(e.instanceRefId);
            bool timer = false;
            const std::string v = inst ? iecValueType(inst->dataType, timer) : std::string();
            if (v.empty()) return std::string();
            if (timer) {
                if (p == "IN" || p == "Q" || p == "R") return "Bool";
                if (p == "PT" || p == "ET") return v;
            } else {
                if (p == "CU" || p == "CD" || p == "R" || p == "LD" || p == "Q" || p == "QU" || p == "QD") return "Bool";
                if (p == "PV" || p == "CV") return v;
            }
            return std::string();
        }
        if (e.kind == "call") {
            const CodeReference* called = ref(e.refId);
            if (!called) return std::string();
            const std::string& name = called->dataType.empty() ? called->name : called->dataType;
            for (const BlockInfo& b : prog_.blockList) {
                if (b.plc != code_.plc || b.name != name || !b.declarationsRead) continue;
                for (const BlockMember& m : b.declarations) {
                    if (m.section == "Return" ? sameName(p, "Ret_Val") : sameName(m.name, p)) {
                        if (m.section == "Input" || m.section == "Output" || m.section == "InOut" || m.section == "Return")
                            return m.dataType;
                    }
                }
            }
        }
        return std::string();
    }

    void address(NetworkElement::Operand& o) const {
        const CodeReference* r = o.linked ? ref(o.refId) : nullptr;
        if (!r) return;
        if (r->kind == "tag") {
            for (const Tag& t : prog_.tags)
                if (t.plc == code_.plc && t.name == r->name) {
                    o.address = t.address;
                    return;
                }
            return;
        }
        if (r->kind != "data block member" || r->path.size() < 2) return;
        const DataBlock* db = nullptr;
        for (const DataBlock& d : prog_.blocks)
            if (d.plc == code_.plc && d.name == r->path[0].name) db = &d;
        if (!db || !db->hasNumber || !db->hasAccess || db->symbolicAccessOnly) return;
        const std::vector<BlockMember>* level = &db->members;
        const BlockMember* m = nullptr;
        for (size_t i = 1; i < r->path.size(); ++i) {
            if (!r->path[i].index.empty()) return;
            m = nullptr;
            for (const BlockMember& x : *level)
                if (x.name == r->path[i].name) m = &x;
            if (!m) return;
            level = &m->members;
        }
        if (!m || !m->hasOffset || !r->modifier.empty()) return;
        const std::string db_ = "%DB" + std::to_string(db->number) + ".DB";
        const uint64_t byte = m->offsetBits / 8;
        if (m->dataType == "Bool") {
            o.address = db_ + "X" + std::to_string(byte) + "." + std::to_string(m->offsetBits % 8);
            return;
        }
        const int n = addressBytes(m->dataType);
        if (n == 1) o.address = db_ + "B" + std::to_string(byte);
        else if (n == 2) o.address = db_ + "W" + std::to_string(byte);
        else if (n == 4) o.address = db_ + "D" + std::to_string(byte);
    }
};

}  // namespace

void linkOperands(BlockCode& code, const ProgramData& prog) { OperandLinker(code, prog).run(); }

CodeData buildCode(const Project& project, const ProgramData& prog, const ProtectedVersions* protectedUpTo) {
    return CodeBuilder(project, prog, protectedUpTo).run();
}

}  // namespace tia
