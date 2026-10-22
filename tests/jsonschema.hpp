// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// For the tests: a JSON reader, and a check of a document against a JSON
// Schema that uses only these keywords: type, enum, const, properties,
// required, additionalProperties (false), items, oneOf, $ref to
// "#/$defs/...", and the annotations $schema, $id, title, description. Any
// other keyword makes the check fail, so the schema cannot use one that is
// not checked here.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace jsontest {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double number = 0;
    bool integer = false;
    std::string s;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;  // in the order written

    const Value* get(const std::string& k) const {
        for (const auto& m : members)
            if (m.first == k) return &m.second;
        return nullptr;
    }
    const Value& operator[](const std::string& k) const {
        static const Value none;
        const Value* v = get(k);
        return v ? *v : none;
    }
    int64_t i() const { return static_cast<int64_t>(number); }
};

class Reader {
public:
    explicit Reader(const std::string& text) : t_(text) {}
    Value document() {
        Value v = value();
        space();
        if (p_ != t_.size()) fail("text after the document");
        return v;
    }

private:
    const std::string& t_;
    size_t p_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("JSON at " + std::to_string(p_) + ": " + what);
    }
    void space() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) ++p_;
    }
    bool take(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (t_.compare(p_, n, word) != 0) return false;
        p_ += n;
        return true;
    }
    Value value() {
        space();
        if (p_ >= t_.size()) fail("unexpected end");
        Value v;
        const char c = t_[p_];
        if (c == '{') {
            v.type = Value::Object;
            ++p_;
            space();
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return v;
            }
            for (;;) {
                space();
                if (p_ >= t_.size() || t_[p_] != '"') fail("expected a key");
                std::string k = string();
                for (const auto& m : v.members)
                    if (m.first == k) fail("key '" + k + "' twice");
                space();
                if (p_ >= t_.size() || t_[p_++] != ':') fail("expected ':'");
                v.members.emplace_back(std::move(k), value());
                space();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == '}') {
                    ++p_;
                    return v;
                }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            ++p_;
            space();
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return v;
            }
            for (;;) {
                v.items.push_back(value());
                space();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == ']') {
                    ++p_;
                    return v;
                }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::String;
            v.s = string();
            return v;
        }
        if (take("true")) {
            v.type = Value::Bool;
            v.b = true;
            return v;
        }
        if (take("false")) {
            v.type = Value::Bool;
            return v;
        }
        if (take("null")) return v;
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = p_;
            if (t_[p_] == '-') ++p_;
            bool frac = false;
            while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' ||
                                      t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-')) {
                if (t_[p_] == '.' || t_[p_] == 'e' || t_[p_] == 'E') frac = true;
                ++p_;
            }
            v.type = Value::Number;
            v.number = std::strtod(t_.substr(start, p_ - start).c_str(), nullptr);
            v.integer = !frac;
            return v;
        }
        fail(std::string("unexpected '") + c + "'");
    }
    std::string string() {
        ++p_;  // the quote
        std::string out;
        while (p_ < t_.size() && t_[p_] != '"') {
            const unsigned char c = static_cast<unsigned char>(t_[p_]);
            if (c < 0x20) fail("control character in a string");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++p_;
                continue;
            }
            if (++p_ >= t_.size()) fail("unexpected end");
            const char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (p_ + 4 > t_.size()) fail("short \\u");
                    const unsigned long cp = std::strtoul(t_.substr(p_, 4).c_str(), nullptr, 16);
                    p_ += 4;
                    // the writer only escapes control characters
                    if (cp >= 0x80) fail("\\u above 0x7f not expected");
                    out += static_cast<char>(cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
        if (p_ >= t_.size()) fail("unterminated string");
        ++p_;
        return out;
    }
};

inline Value parse(const std::string& text) { return Reader(text).document(); }

// The errors of `doc` against `schema`, at most `limit`; empty when it fits.
class Validator {
public:
    explicit Validator(const Value& schema) : root_(schema) {}

    std::vector<std::string> check(const Value& doc, size_t limit = 20) {
        errors_.clear();
        limit_ = limit;
        validate(doc, root_, "$");
        return errors_;
    }

private:
    const Value& root_;
    std::vector<std::string> errors_;
    size_t limit_ = 20;

    void error(const std::string& path, const std::string& what) {
        if (errors_.size() < limit_) errors_.push_back(path + ": " + what);
    }

    static bool equal(const Value& a, const Value& b) {
        if (a.type != b.type) return false;
        switch (a.type) {
            case Value::Null: return true;
            case Value::Bool: return a.b == b.b;
            case Value::Number: return a.number == b.number;
            case Value::String: return a.s == b.s;
            default: return false;  // not used for structures here
        }
    }

    static bool hasType(const Value& v, const std::string& t) {
        if (t == "null") return v.type == Value::Null;
        if (t == "boolean") return v.type == Value::Bool;
        if (t == "string") return v.type == Value::String;
        if (t == "array") return v.type == Value::Array;
        if (t == "object") return v.type == Value::Object;
        if (t == "number") return v.type == Value::Number;
        if (t == "integer") return v.type == Value::Number && v.integer;
        return false;
    }

    // true when it fits; errors only when `report`
    bool validate(const Value& v, const Value& s, const std::string& path, bool report = true) {
        const size_t before = errors_.size();
        const size_t savedLimit = limit_;
        if (!report) limit_ = 0;
        bool ok = true;
        auto bad = [&](const std::string& what) {
            ok = false;
            error(path, what);
        };
        for (const auto& kw : s.members) {
            const std::string& k = kw.first;
            const Value& a = kw.second;
            if (k == "$schema" || k == "$id" || k == "title" || k == "description" || k == "$defs") continue;
            if (k == "type") {
                bool any = false;
                if (a.type == Value::String) any = hasType(v, a.s);
                else
                    for (const auto& t : a.items) any = any || hasType(v, t.s);
                if (!any) bad("not of the type the schema gives");
            } else if (k == "enum") {
                bool any = false;
                for (const auto& e : a.items) any = any || equal(v, e);
                if (!any) bad("not one of the values the schema allows");
            } else if (k == "const") {
                if (!equal(v, a)) bad("not the value the schema requires");
            } else if (k == "required") {
                if (v.type == Value::Object)
                    for (const auto& r : a.items)
                        if (!v.get(r.s)) bad("'" + r.s + "' missing");
            } else if (k == "properties") {
                if (v.type == Value::Object)
                    for (const auto& m : v.members)
                        if (const Value* ps = a.get(m.first))
                            if (!validate(m.second, *ps, path + "." + m.first, report)) ok = false;
            } else if (k == "additionalProperties") {
                if (a.type != Value::Bool || a.b) {
                    bad("schema: additionalProperties other than false is not supported");
                } else if (v.type == Value::Object) {
                    const Value* props = s.get("properties");
                    for (const auto& m : v.members)
                        if (!props || !props->get(m.first)) bad("'" + m.first + "' not in the schema");
                }
            } else if (k == "items") {
                if (v.type == Value::Array)
                    for (size_t i = 0; i < v.items.size(); ++i)
                        if (!validate(v.items[i], a, path + "[" + std::to_string(i) + "]", report)) ok = false;
            } else if (k == "oneOf") {
                size_t fits = 0;
                for (const auto& alt : a.items)
                    if (validate(v, alt, path, false)) ++fits;
                if (fits != 1) {
                    // say why for the alternative of the same type
                    bad("fits " + std::to_string(fits) + " of the oneOf alternatives");
                    if (report)
                        for (const auto& alt : a.items) {
                            const Value* t = alt.get("type");
                            if (t && t->type == Value::String && hasType(v, t->s)) validate(v, alt, path, true);
                            if (!t) validate(v, alt, path, true);
                        }
                }
            } else if (k == "$ref") {
                const std::string prefix = "#/$defs/";
                const Value* target = nullptr;
                if (a.s.compare(0, prefix.size(), prefix) == 0)
                    if (const Value* defs = root_.get("$defs")) target = defs->get(a.s.substr(prefix.size()));
                if (!target) bad("schema: $ref '" + a.s + "' not found");
                else if (!validate(v, *target, path, report)) ok = false;
            } else {
                bad("schema: keyword '" + k + "' is not supported by this check");
            }
        }
        limit_ = savedLimit;
        if (!report) errors_.resize(before);
        return ok;
    }
};

}  // namespace jsontest
