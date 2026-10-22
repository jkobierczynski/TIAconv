// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "programjson.hpp"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
    // the shortest text that reads back as the same double
    void value(double x) {
        before();
        char buf[40];
        for (int precision = 6; precision <= 17; ++precision) {
            std::snprintf(buf, sizeof buf, "%.*g", precision, x);
            if (std::strtod(buf, nullptr) == x) break;
        }
        std::string t = buf;
        if (t.find_first_of(".eE") == std::string::npos) t += ".0";
        out_ << t;
    }
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

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// A whole number as a constant or a bound is written: 10, -3, 16#FF, 2#101,
// 8#17; false for anything else.
bool parseWhole(const std::string& text, int64_t& out) {
    std::string t = trim(text);
    if (t.empty()) return false;
    int base = 10;
    bool negative = false;
    if (t[0] == '-' || t[0] == '+') {
        negative = t[0] == '-';
        t = t.substr(1);
    }
    const size_t hash = t.find('#');
    if (hash != std::string::npos) {
        const std::string b = t.substr(0, hash);
        if (b == "16") base = 16;
        else if (b == "8") base = 8;
        else if (b == "2") base = 2;
        else return false;
        t = t.substr(hash + 1);
    }
    std::string digits;
    for (char c : t)
        if (c != '_') digits += c;
    if (digits.empty() || digits.size() > 20) return false;
    char* end = nullptr;
    const long long v = std::strtoll(digits.c_str(), &end, base);
    if (*end) return false;
    out = negative ? -v : v;
    return true;
}

struct Elementary {
    const char* name;
    int64_t bits;
};
// The elementary types, and the system types that are a number of a fixed
// size (hardware identifiers, OB numbers, process image partitions).
const Elementary kElementary[] = {
    {"Bool", 1},          {"Byte", 8},          {"Word", 16},        {"DWord", 32},       {"LWord", 64},
    {"SInt", 8},          {"USInt", 8},         {"Int", 16},         {"UInt", 16},        {"DInt", 32},
    {"UDInt", 32},        {"LInt", 64},         {"ULInt", 64},       {"Real", 32},        {"LReal", 64},
    {"Time", 32},         {"LTime", 64},        {"S5Time", 16},      {"Date", 16},        {"Time_Of_Day", 32},
    {"TOD", 32},          {"LTime_Of_Day", 64}, {"LTOD", 64},        {"Date_And_Time", 64}, {"DT", 64},
    {"LDT", 64},          {"Char", 8},          {"WChar", 16},       {"Timer", 16},       {"Counter", 16},
    {"Hw_Any", 16},       {"Hw_Device", 16},    {"Hw_DpMaster", 16}, {"Hw_DpSlave", 16},  {"Hw_Io", 16},
    {"Hw_IoSystem", 16},  {"Hw_SubModule", 16}, {"Hw_Module", 16},   {"Hw_Interface", 16}, {"Hw_Hsc", 16},
    {"Hw_Pwm", 16},       {"Hw_Pto", 16},       {"OB_Any", 16},      {"OB_Delay", 16},    {"OB_Tod", 16},
    {"OB_Cyclic", 16},    {"OB_Att", 16},       {"OB_PCycle", 16},   {"OB_HwInt", 16},    {"OB_Diag", 16},
    {"OB_TimeError", 16}, {"OB_Startup", 16},   {"Pip", 16},         {"DB_Any", 16},      {"DB_WWW", 16},
    {"DB_Dyn", 16},       {"Conn_Any", 16},     {"Conn_Prg", 16},    {"Conn_Ouc", 16},    {"Conn_R_ID", 32},
    {"Port", 16},         {"Aom_Ident", 32},    {"Event_Any", 32},   {"Event_Att", 32},   {"Event_HwInt", 32},
    {"Rtm", 16},
};

const char* const kParameterTypes[] = {"Variant", "Any", "Pointer", "Block_FB", "Block_FC", "Block_DB",
                                       "Block_SDB", "Remote", "Void"};

TypeDescriptor::Bound bound(const std::string& text) {
    TypeDescriptor::Bound b;
    b.text = trim(text);
    b.known = parseWhole(b.text, b.value);
    return b;
}

std::string canonicalElementary(const std::string& name, int64_t& bits) {
    const std::string l = lower(name);
    for (const Elementary& e : kElementary)
        if (lower(e.name) == l) {
            bits = e.bits;
            return e.name;
        }
    return std::string();
}

TypeDescriptor describe(const std::string& dataType, int depth);

}  // namespace

TypeDescriptor describeType(const std::string& dataType) { return describe(dataType, 0); }

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Digits of `base` with "_" between them, into `out`; false when there are
// none or another character, or the value does not fit 64 bits.
bool digitsOf(const std::string& t, int base, uint64_t& out) {
    out = 0;
    bool any = false;
    for (char c : t) {
        if (c == '_') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        if (out > (UINT64_MAX - static_cast<uint64_t>(d)) / static_cast<uint64_t>(base)) return false;
        out = out * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
        any = true;
    }
    return any;
}

// "16#FF", "2#1010", "123", "-5": a whole number.
bool wholeOf(std::string t, ParsedValue& v) {
    bool negative = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
        negative = t[0] == '-';
        t = t.substr(1);
    }
    int base = 10;
    const size_t hash = t.find('#');
    if (hash != std::string::npos) {
        const std::string b = t.substr(0, hash);
        if (b == "16") base = 16;
        else if (b == "8") base = 8;
        else if (b == "2") base = 2;
        else return false;
        t = t.substr(hash + 1);
    }
    uint64_t u = 0;
    if (!digitsOf(t, base, u)) return false;
    v.kind = "int";
    if (negative) {
        if (u > static_cast<uint64_t>(INT64_MAX) + 1) return false;
        v.integer = u == static_cast<uint64_t>(INT64_MAX) + 1 ? INT64_MIN : -static_cast<int64_t>(u);
    } else if (u > static_cast<uint64_t>(INT64_MAX)) {
        v.unsignedBig = true;
        v.unsignedValue = u;
    } else {
        v.integer = static_cast<int64_t>(u);
    }
    return true;
}

bool realOf(const std::string& t, ParsedValue& v) {
    // a real has a point or an exponent; a whole number too large for 64
    // bits is not one
    if (t.empty() || t.find_first_of(".eE") == std::string::npos) return false;
    std::string c;
    for (char ch : t)
        if (ch != '_') c += ch;
    char* end = nullptr;
    const double x = std::strtod(c.c_str(), &end);
    if (end == c.c_str() || *end || !std::isfinite(x)) return false;
    // only what TIA Portal writes: digits, one point, an exponent
    for (char ch : c)
        if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-'))
            return false;
    v.kind = "real";
    v.real = x;
    return true;
}

// T#1D_2H_3M_4S_5MS, T#-5s, T#1.5S, LT#10NS: into nanoseconds.
bool durationOf(std::string t, ParsedValue& v) {
    bool negative = false;
    if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
        negative = t[0] == '-';
        t = t.substr(1);
    }
    const std::string u = upper(t);
    long double total = 0;
    size_t i = 0;
    bool any = false;
    while (i < u.size()) {
        if (u[i] == '_') {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < u.size() && (std::isdigit(static_cast<unsigned char>(u[j])) || u[j] == '.')) ++j;
        if (j == i) return false;
        const long double n = std::strtold(u.substr(i, j - i).c_str(), nullptr);
        long double unit = 0;
        if (u.compare(j, 2, "MS") == 0) unit = 1e6L, j += 2;
        else if (u.compare(j, 2, "US") == 0) unit = 1e3L, j += 2;
        else if (u.compare(j, 2, "NS") == 0) unit = 1, j += 2;
        else if (j < u.size() && u[j] == 'D') unit = 86400e9L, j += 1;
        else if (j < u.size() && u[j] == 'H') unit = 3600e9L, j += 1;
        else if (j < u.size() && u[j] == 'M') unit = 60e9L, j += 1;
        else if (j < u.size() && u[j] == 'S') unit = 1e9L, j += 1;
        else return false;
        total += n * unit;
        any = true;
        i = j;
    }
    if (!any || total > 9.2e18L) return false;
    v.kind = "duration";
    v.ns = static_cast<int64_t>(total + 0.5L) * (negative ? -1 : 1);
    return true;
}

// Days from 1970-01-01 to a date (proleptic Gregorian).
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool numberRun(const std::string& t, size_t& i, size_t maxDigits, int64_t& out) {
    size_t j = i;
    while (j < t.size() && j - i < maxDigits && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
    if (j == i) return false;
    out = std::strtoll(t.substr(i, j - i).c_str(), nullptr, 10);
    i = j;
    return true;
}

bool dateAt(const std::string& t, size_t& i, int64_t& days) {
    int64_t y, m, d;
    if (!numberRun(t, i, 4, y) || i >= t.size() || t[i++] != '-' || !numberRun(t, i, 2, m) || i >= t.size() ||
        t[i++] != '-' || !numberRun(t, i, 2, d))
        return false;
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    days = daysFromCivil(y, m, d);
    return true;
}

// hh:mm[:ss[.fraction]] into nanoseconds since midnight
bool timeAt(const std::string& t, size_t& i, int64_t& ns) {
    int64_t h, m, s = 0;
    if (!numberRun(t, i, 2, h) || i >= t.size() || t[i++] != ':' || !numberRun(t, i, 2, m)) return false;
    if (i < t.size() && t[i] == ':') {
        ++i;
        if (!numberRun(t, i, 2, s)) return false;
    }
    int64_t frac = 0;
    if (i < t.size() && t[i] == '.') {
        ++i;
        int64_t scale = 100000000;
        size_t digits = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
            if (digits++ < 9) {
                frac += (t[i] - '0') * scale;
                scale /= 10;
            }
            ++i;
        }
        if (!digits) return false;
    }
    if (h > 23 || m > 59 || s > 59) return false;
    ns = ((h * 60 + m) * 60 + s) * 1000000000LL + frac;
    return true;
}

// 'text' with $ escapes: $$ $' $L $N $P $R $T and $hh (two hex digits,
// four for a WString)
bool stringOf(const std::string& t, bool wide, ParsedValue& v) {
    if (t.size() < 2 || t.front() != '\'' || t.back() != '\'') return false;
    std::string out;
    for (size_t i = 1; i + 1 < t.size(); ++i) {
        const char c = t[i];
        if (c == '\'') return false;  // a lone quote inside
        if (c != '$') {
            out += c;
            continue;
        }
        if (i + 2 > t.size() - 1) return false;
        const char e = static_cast<char>(std::toupper(static_cast<unsigned char>(t[++i])));
        if (e == '$') out += '$';
        else if (e == '\'') out += '\'';
        else if (e == 'L' || e == 'N') out += '\n';
        else if (e == 'P') out += '\f';
        else if (e == 'R') out += '\r';
        else if (e == 'T') out += '\t';
        else {
            const size_t n = wide ? 4 : 2;
            if (i + n > t.size() - 1) return false;
            uint64_t code = 0;
            if (!digitsOf(t.substr(i, n), 16, code)) return false;
            i += n - 1;
            // as UTF-8
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
    }
    v.kind = "string";
    v.string = out;
    return true;
}

// The code of a one-character string (UTF-8 decoded); -1 when it is not one.
int64_t charCode(const std::string& s) {
    if (s.empty()) return -1;
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return s.size() == 1 ? c : -1;
    if ((c & 0xE0) == 0xC0 && s.size() == 2) return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && s.size() == 3) return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return -1;
}

}  // namespace

ParsedValue parseValue(const std::string& text, const TypeDescriptor* type) {
    ParsedValue v;
    v.text = trim(text);
    v.kind = "unknown";
    std::string t = v.text;
    // the type the value is for, where it is elementary or a string
    std::string want;
    if (type && (type->kind == "elementary" || type->kind == "string")) want = upper(type->name);
    // a type in front: INT#5, REAL#1.5, W#16#FF, DT#..., T#5S, STRING#'a'
    std::string prefix;
    const size_t hash = t.find('#');
    if (hash != std::string::npos && hash > 0 && t[0] != '\'' && !std::isdigit(static_cast<unsigned char>(t[0])) &&
        t[0] != '-' && t[0] != '+') {
        prefix = upper(t.substr(0, hash));
        t = t.substr(hash + 1);
    }
    static const std::set<std::string> durations = {"T", "TIME", "LT", "LTIME", "S5T", "S5TIME"};
    static const std::set<std::string> bitsOrInts = {
        "B", "W", "DW", "LW", "BYTE", "WORD", "DWORD", "LWORD", "SINT", "INT", "DINT", "LINT",
        "USINT", "UINT", "UDINT", "ULINT"};
    ParsedValue p = v;
    if (!prefix.empty()) {
        if (durations.count(prefix)) {
            if (durationOf(t, p)) return p;
        } else if (prefix == "D" || prefix == "DATE") {
            size_t i = 0;
            int64_t days = 0;
            if (dateAt(t, i, days) && i == t.size()) {
                p.kind = "date";
                p.days = days - daysFromCivil(1990, 1, 1);
                return p;
            }
        } else if (prefix == "TOD" || prefix == "TIME_OF_DAY" || prefix == "LTOD" || prefix == "LTIME_OF_DAY") {
            size_t i = 0;
            int64_t ns = 0;
            if (timeAt(t, i, ns) && i == t.size()) {
                p.kind = "time_of_day";
                p.ns = ns;
                return p;
            }
        } else if (prefix == "DT" || prefix == "DATE_AND_TIME" || prefix == "LDT" || prefix == "DTL") {
            size_t i = 0;
            int64_t days = 0, ns = 0;
            // nanoseconds since 1970 hold the years 1677 to 2262 (LDT's range)
            if (dateAt(t, i, days) && i < t.size() && t[i++] == '-' && timeAt(t, i, ns) && i == t.size() &&
                days > -106000 && days < 106000) {
                p.kind = "date_and_time";
                p.ns = days * 86400000000000LL + ns;
                return p;
            }
        } else if (bitsOrInts.count(prefix)) {
            if (wholeOf(t, p)) return p;
        } else if (prefix == "REAL" || prefix == "LREAL") {
            if (wholeOf(t, p) || realOf(t, p)) {
                if (p.kind == "int") {
                    p.real = static_cast<double>(p.integer);
                    p.kind = "real";
                }
                return p;
            }
        } else if (prefix == "BOOL") {
            const std::string u = upper(t);
            if (u == "TRUE" || u == "1" || u == "FALSE" || u == "0") {
                p.kind = "bool";
                p.boolean = u == "TRUE" || u == "1";
                return p;
            }
        } else if (prefix == "STRING" || prefix == "WSTRING" || prefix == "CHAR" || prefix == "WCHAR") {
            if (stringOf(t, prefix[0] == 'W', p)) {
                if (prefix == "CHAR" || prefix == "WCHAR") {
                    const int64_t code = charCode(p.string);
                    if (code < 0) return v;
                    p.kind = "char";
                    p.integer = code;
                }
                return p;
            }
        }
        return v;
    }
    const std::string u = upper(t);
    if (u == "TRUE" || u == "FALSE") {
        p.kind = "bool";
        p.boolean = u == "TRUE";
        return p;
    }
    if (!t.empty() && t[0] == '\'') {
        if (!stringOf(t, want == "WSTRING" || want == "WCHAR", p)) return v;
        if (want == "CHAR" || want == "WCHAR") {
            const int64_t code = charCode(p.string);
            if (code < 0) return v;
            p.kind = "char";
            p.integer = code;
        }
        return p;
    }
    if (wholeOf(t, p)) {
        if (want == "REAL" || want == "LREAL") {
            p.kind = "real";
            p.real = p.unsignedBig ? static_cast<double>(p.unsignedValue) : static_cast<double>(p.integer);
            p.integer = 0;
            p.unsignedBig = false;
        } else if (want == "BOOL" && !p.unsignedBig && (p.integer == 0 || p.integer == 1)) {
            p.kind = "bool";
            p.boolean = p.integer == 1;
            p.integer = 0;
        } else if ((want == "TIME" || want == "S5TIME") && !p.unsignedBig) {
            p.kind = "duration";  // a number for a Time counts milliseconds
            p.ns = p.integer * 1000000;
            p.integer = 0;
        } else if (want == "LTIME" && !p.unsignedBig) {
            p.kind = "duration";  // and for an LTime nanoseconds
            p.ns = p.integer;
            p.integer = 0;
        }
        return p;
    }
    if (realOf(t, p)) return p;
    return v;
}

namespace {

TypeDescriptor describe(const std::string& dataType, int depth) {
    TypeDescriptor d;
    const std::string t = trim(dataType);
    const std::string l = lower(t);
    if (t.empty() || depth > 16) {  // arrays of arrays: no deeper than this
        d.kind = "unknown";
        d.name = t;
        return d;
    }
    if (l.compare(0, 5, "array") == 0) {
        const size_t open = t.find('[', 5), close = t.find(']', 5);
        const std::string between = open == std::string::npos ? std::string() : trim(t.substr(5, open - 5));
        const std::string after = close == std::string::npos ? std::string() : trim(t.substr(close + 1));
        if (open != std::string::npos && close != std::string::npos && close > open && between.empty() &&
            lower(after).compare(0, 3, "of ") == 0) {
            d.kind = "array";
            d.name = "Array";
            const std::string dims = t.substr(open + 1, close - open - 1);
            size_t from = 0;
            while (from <= dims.size()) {
                size_t comma = dims.find(',', from);
                if (comma == std::string::npos) comma = dims.size();
                const std::string dim = trim(dims.substr(from, comma - from));
                TypeDescriptor::Dim x;
                const size_t dots = dim.find("..");
                if (dots == std::string::npos) {
                    x.low = bound(dim);  // "*": a variable bound
                    x.high = bound(dim);
                } else {
                    x.low = bound(dim.substr(0, dots));
                    x.high = bound(dim.substr(dots + 2));
                }
                d.dims.push_back(x);
                from = comma + 1;
            }
            d.element = std::make_shared<TypeDescriptor>(describe(after.substr(3), depth + 1));
            return d;
        }
        d.kind = "unknown";
        d.name = t;
        return d;
    }
    if (l.compare(0, 6, "string") == 0 || l.compare(0, 7, "wstring") == 0) {
        const bool wide = l[0] == 'w';
        const std::string rest = trim(t.substr(wide ? 7 : 6));
        int64_t n = 254;
        if (rest.empty() || (rest.front() == '[' && rest.back() == ']' && parseWhole(rest.substr(1, rest.size() - 2), n))) {
            d.kind = "string";
            d.name = wide ? "WString" : "String";
            d.length = n;
            return d;
        }
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        d.kind = "named";
        d.name = t.substr(1, t.size() - 2);
        d.quoted = true;
        return d;
    }
    if (l == "struct") {
        d.kind = "struct";
        d.name = "Struct";
        return d;
    }
    if (l == "void") {
        d.kind = "void";
        d.name = "Void";
        return d;
    }
    for (const char* p : kParameterTypes)
        if (lower(p) == l) {
            d.kind = "parameter";
            d.name = p;
            return d;
        }
    int64_t bits = -1;
    const std::string e = canonicalElementary(t, bits);
    if (!e.empty()) {
        d.kind = "elementary";
        d.name = e;
        d.bits = bits;
        return d;
    }
    bool identifier = true;
    for (unsigned char c : t)
        if (!(std::isalnum(c) || c == '_')) identifier = false;
    d.kind = identifier ? "named" : "unknown";
    d.name = t;
    return d;
}

// What the names in data types stand for in one PLC, the values of its
// constants, and the system types met on the way.
class Types {
public:
    Types(const ProgramData& prog, const std::string& plc) : prog_(prog), plc_(plc) {
        for (const BlockInfo& b : prog.blockList)
            if (b.plc == plc || b.plc.empty()) blocks_.emplace(b.name, &b);
        for (const Constant& c : prog.constants)
            if (c.plc == plc) globals_[c.name] = c.value;
    }

    const BlockInfo* block(const std::string& name) const {
        auto it = blocks_.find(name);
        return it == blocks_.end() ? nullptr : it->second;
    }

    // "plc_type", "system_type", "instance" or "named" (not found)
    std::string kindOf(const TypeDescriptor& d) const {
        auto it = blocks_.find(d.name);
        if (it != blocks_.end()) {
            const std::string& t = it->second->type;
            if (t == "UDT") return "plc_type";
            if (t == "SDT") return "system_type";
            if (t == "FB" || t == "SFB") return "instance";
        }
        return d.quoted ? "named" : "system_type";
    }

    void resolve(TypeDescriptor::Bound& b, const std::map<std::string, std::string>* locals) const {
        if (b.known || b.text.empty()) return;
        std::string value;
        if (b.text[0] == '#' && locals) {
            auto it = locals->find(b.text.substr(1));
            if (it != locals->end()) value = it->second;
        } else if (b.text.size() >= 2 && b.text.front() == '"' && b.text.back() == '"') {
            auto it = globals_.find(b.text.substr(1, b.text.size() - 2));
            if (it != globals_.end()) value = it->second;
        }
        if (!value.empty()) b.known = parseWhole(value, b.value);
    }

    // The PLC data types and system types with their members: from the
    // definition of a PLC data type where it was read, otherwise as first met
    // in a member of that type (the members of an array's element). Instances
    // of FBs are not types here: their members are the FB's interface.
    void collect(const std::vector<BlockMember>& list) {
        for (const BlockMember& m : list) {
            TypeDescriptor d = describeType(m.dataType);
            if (d.kind == "array" && d.element) d = *d.element;
            if (d.kind == "named" && !m.members.empty()) {
                const std::string k = kindOf(d);
                if ((k == "plc_type" || k == "system_type") && !found_.count(d.name))
                    found_.emplace(d.name, Found{k, &m.members});
            }
            collect(m.members);
        }
    }
    void define(const BlockInfo& udt) { found_[udt.name] = Found{"plc_type", &udt.declarations}; }
    struct Found {
        std::string kind;
        const std::vector<BlockMember>* members;
    };
    const std::map<std::string, Found>& found() const { return found_; }

    void write(Json& j, TypeDescriptor d, const std::map<std::string, std::string>* locals) const {
        if (d.kind == "named") d.kind = kindOf(d);
        for (auto& x : d.dims) {
            resolve(x.low, locals);
            resolve(x.high, locals);
        }
        j.beginObject(true);
        j.key("kind").value(d.kind);
        j.key("name").text(d.name);
        j.key("bits");
        if (d.bits >= 0) j.value(d.bits);
        else j.null();
        j.key("length");
        if (d.length >= 0) j.value(d.length);
        else j.null();
        j.key("dims");
        j.beginArray();
        for (const auto& x : d.dims) {
            j.beginObject();
            for (const auto* b : {&x.low, &x.high}) {
                j.key(b == &x.low ? "low" : "high");
                if (b->known) j.value(b->value);
                else j.null();
                j.key(b == &x.low ? "low_text" : "high_text").value(b->text);
            }
            j.endObject();
        }
        j.endArray();
        j.key("element");
        if (d.element) write(j, *d.element, locals);
        else j.null();
        j.endObject();
    }

private:
    const ProgramData& prog_;
    std::string plc_;
    std::map<std::string, const BlockInfo*> blocks_;
    std::map<std::string, std::string> globals_;
    std::map<std::string, Found> found_;
};

void parsedValue(Json& j, const ParsedValue& v) {
    j.beginObject(true);
    j.key("kind").value(v.kind);
    j.key("text").value(v.text);
    j.key("bool");
    if (v.kind == "bool") j.value(v.boolean);
    else j.null();
    j.key("int");
    if (v.kind == "int" || v.kind == "char") {
        if (v.unsignedBig) j.value(v.unsignedValue);
        else j.value(v.integer);
    } else {
        j.null();
    }
    j.key("real");
    if (v.kind == "real") j.value(v.real);
    else j.null();
    j.key("ns");
    if (v.kind == "duration" || v.kind == "time_of_day" || v.kind == "date_and_time") j.value(v.ns);
    else j.null();
    j.key("days");
    if (v.kind == "date") j.value(v.days);
    else j.null();
    j.key("string");
    if (v.kind == "string" || v.kind == "char") j.value(v.string);
    else j.null();
    j.endObject();
}

// The default value of an elementary type or a string; false for others.
bool defaultValue(const TypeDescriptor& t, ParsedValue& v) {
    v = ParsedValue();
    if (t.kind == "string") {
        v.kind = "string";
        return true;
    }
    if (t.kind != "elementary") return false;
    const std::string n = t.name;
    if (n == "Bool") v.kind = "bool";
    else if (n == "Real" || n == "LReal") v.kind = "real";
    else if (n == "Time" || n == "LTime" || n == "S5Time") v.kind = "duration";
    else if (n == "Date") v.kind = "date";
    else if (n == "Time_Of_Day" || n == "TOD" || n == "LTime_Of_Day" || n == "LTOD") v.kind = "time_of_day";
    else if (n == "Date_And_Time" || n == "DT") {
        v.kind = "date_and_time";  // DT#1990-01-01-00:00:00
        v.ns = 7305LL * 86400000000000LL;
    } else if (n == "LDT") v.kind = "date_and_time";  // LDT#1970-01-01-00:00:00
    else if (n == "Char" || n == "WChar") {
        v.kind = "char";
        v.string = std::string(1, '\0');
    } else v.kind = "int";
    return true;
}

// Where the value of a named constant comes from: the user constants of the
// PLC and the local constants of the block.
struct ConstantValues {
    std::map<std::string, const Constant*> globals;
    std::map<std::string, const BlockMember*> locals;
};

// The value of an operand or reference that is a constant; false for
// anything else or a constant whose value is not known.
bool constantValue(const std::string& kind, const std::string& text, const std::string& dataType,
                   const ConstantValues& cv, ParsedValue& out) {
    if (kind == "constant") {
        const TypeDescriptor t = describeType(dataType);
        out = parseValue(text, &t);
        return true;
    }
    if (kind == "user constant" && text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        auto it = cv.globals.find(text.substr(1, text.size() - 2));
        if (it == cv.globals.end()) return false;
        const TypeDescriptor t = describeType(it->second->dataType);
        out = parseValue(it->second->value, &t);
        return true;
    }
    if (kind == "local constant" && text.size() >= 2 && text[0] == '#') {
        auto it = cv.locals.find(text.substr(1));
        if (it == cv.locals.end()) return false;
        const TypeDescriptor t = describeType(it->second->dataType);
        // a constant with the default value of its type has none stored
        if (!it->second->hasStartValue) return defaultValue(t, out);
        out = parseValue(it->second->startValue, &t);
        return true;
    }
    return false;
}

// The local constants of a block's interface: name -> value.
std::map<std::string, std::string> localConstants(const std::vector<BlockMember>& declarations) {
    std::map<std::string, std::string> out;
    for (const BlockMember& m : declarations)
        if (m.section == "Constant" && m.hasStartValue) out[m.name] = m.startValue;
    return out;
}

void members(Json& j, const std::vector<BlockMember>& list, Types& types,
             const std::map<std::string, std::string>* locals, const MemberAccess& outer = MemberAccess()) {
    j.beginArray();
    for (const BlockMember& m : list) {
        const TypeDescriptor d = describeType(m.dataType);
        j.beginObject(m.members.empty());
        j.key("name").value(m.name);
        j.key("data_type").text(m.dataType);
        j.key("type");
        types.write(j, d, locals);
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
        j.key("start");
        if (m.hasStartValue) parsedValue(j, parseValue(m.startValue, &d));
        else j.null();
        j.key("retain").value(isRetain(m));
        const MemberAccess a = memberAccess(m, outer);
        j.key("accessible_from_hmi").value(a.accessible);
        j.key("writable_from_hmi").value(a.writable);
        j.key("setpoint").value(a.setpoint);
        j.key("unresolved").value(m.unresolved);
        j.key("comment").text(m.comment);
        j.key("members");
        // the members of a multi-instance take the bounds from its FB's constants
        TypeDescriptor inner = d.kind == "array" && d.element ? *d.element : d;
        const BlockInfo* fb = inner.kind == "named" && types.kindOf(inner) == "instance" ? types.block(inner.name) : nullptr;
        if (fb && fb->declarationsRead) {
            const std::map<std::string, std::string> own = localConstants(fb->declarations);
            members(j, m.members, types, &own, a);
        } else {
            members(j, m.members, types, locals, a);
        }
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

void element(Json& j, const NetworkElement& e, const ConstantValues& cv) {
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
            j.key("value");
            ParsedValue pv;
            if (constantValue(o.kind, o.text, o.dataType, cv, pv)) parsedValue(j, pv);
            else j.null();
            j.endObject();
        }
        j.endArray();
        j.endObject();
    }
    j.endArray();
    j.endObject();
}

void blockCode(Json& j, const BlockCode& c, const ConstantValues& cv) {
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
        for (const auto& e : n.elements) element(j, e, cv);
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
        j.key("value");
        ParsedValue pv;
        if (constantValue(r.kind, r.text, r.dataType, cv, pv)) parsedValue(j, pv);
        else j.null();
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

    // the types: PLC data types from their definitions, then whatever else is
    // met in the members
    Types types(prog, plc);
    for (const BlockInfo& b : prog.blockList)
        if (b.plc == plc && b.type == "UDT" && b.declarationsRead) types.define(b);
    for (const BlockInfo& b : prog.blockList)
        if (b.plc == plc && b.type == "UDT" && b.declarationsRead) types.collect(b.declarations);
    for (const DataBlock& d : prog.blocks)
        if (d.plc == plc) types.collect(d.members);
    std::map<std::string, const BlockInfo*> codeBlocks;
    for (const BlockInfo& b : prog.blockList)
        if (b.plc == plc && (b.type == "OB" || b.type == "FB" || b.type == "FC")) {
            codeBlocks[b.name] = &b;
            if (b.declarationsRead) types.collect(b.declarations);
        }

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
        j.key("type");
        types.write(j, describeType(t.dataType), nullptr);
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
        j.key("parsed");
        if (!c.value.empty()) {
            const TypeDescriptor t = describeType(c.dataType);
            parsedValue(j, parseValue(c.value, &t));
        } else {
            j.null();
        }
        j.key("kind").text(c.kind);
        j.key("system").value(c.system);
        j.key("comment").text(c.comment);
        j.endObject();
    }
    j.endArray();

    std::vector<Reason> reasons;
    j.key("types");
    j.beginArray();
    for (const auto& t : types.found()) {
        j.beginObject();
        j.key("name").value(t.first);
        j.key("kind").value(t.second.kind);
        j.key("members");
        members(j, *t.second.members, types, nullptr);
        j.endObject();
    }
    j.endArray();

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
        // an instance data block: the local constants of its FB give array bounds
        std::map<std::string, std::string> locals;
        auto fb = codeBlocks.find(d.instanceOf);
        if (fb != codeBlocks.end()) locals = localConstants(fb->second->declarations);
        j.key("members");
        members(j, d.members, types, &locals);
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
        if (open && b.declarationsRead) {
            const std::map<std::string, std::string> locals = localConstants(b.declarations);
            members(j, b.declarations, types, &locals);
        }
        else j.null();
        j.key("code");
        if (open) {
            ConstantValues cv;
            for (const Constant& k : prog.constants)
                if (k.plc == plc) cv.globals[k.name] = &k;
            if (b.declarationsRead)
                for (const BlockMember& m : b.declarations)
                    if (m.section == "Constant") cv.locals[m.name] = &m;
            blockCode(j, *c, cv);
        }
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
