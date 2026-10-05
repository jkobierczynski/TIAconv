// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xml.hpp"

#include <cstdlib>

#include "bytes.hpp"

namespace tia {

const std::string* XmlNode::attr(const std::string& key) const {
    for (const auto& a : attrs)
        if (a.first == key) return &a.second;
    return nullptr;
}

std::string XmlNode::attrOr(const std::string& key, const std::string& fallback) const {
    const std::string* v = attr(key);
    return v ? *v : fallback;
}

bool XmlNode::attrIs(const std::string& key, const std::string& value) const {
    const std::string* v = attr(key);
    return v && *v == value;
}

namespace {

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

std::string unescape(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t end = s.find(';', i);
        if (end == std::string::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, end - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.size() > 1 && ent[0] == '#') {
            unsigned long cp = (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                 : std::strtoul(ent.c_str() + 1, nullptr, 10);
            appendUtf8(out, cp);
        } else {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}

    std::unique_ptr<XmlNode> run() {
        skipMisc();
        if (pos_ >= t_.size()) throw ParseError("XML: no root element");
        auto root = element(0);
        return root;
    }

private:
    const std::string& t_;
    size_t pos_ = 0;

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    static bool isNameEnd(char c) { return isSpace(c) || c == '>' || c == '/' || c == '='; }

    bool startsWith(const char* s) const { return t_.compare(pos_, std::char_traits<char>::length(s), s) == 0; }

    void skipSpace() {
        while (pos_ < t_.size() && isSpace(t_[pos_])) ++pos_;
    }

    void skipUntil(const char* terminator) {
        size_t e = t_.find(terminator, pos_);
        if (e == std::string::npos) throw ParseError("XML: unterminated construct");
        pos_ = e + std::char_traits<char>::length(terminator);
    }

    // Skips text, comments, processing instructions, CDATA and DOCTYPE up to the next tag.
    void skipMisc() {
        for (;;) {
            size_t lt = t_.find('<', pos_);
            if (lt == std::string::npos) {
                pos_ = t_.size();
                return;
            }
            pos_ = lt;
            if (startsWith("<!--")) skipUntil("-->");
            else if (startsWith("<![CDATA[")) skipUntil("]]>");
            else if (startsWith("<?")) skipUntil("?>");
            else if (startsWith("<!")) skipUntil(">");
            else return;
        }
    }

    std::string name() {
        size_t s = pos_;
        while (pos_ < t_.size() && !isNameEnd(t_[pos_])) ++pos_;
        if (pos_ == s) throw ParseError("XML: empty name");
        return t_.substr(s, pos_ - s);
    }

    static std::string localName(const std::string& n) {
        size_t c = n.find(':');
        return c == std::string::npos ? n : n.substr(c + 1);
    }

    std::unique_ptr<XmlNode> element(int depth) {
        if (depth > 256) throw ParseError("XML: nesting too deep");
        ++pos_;  // '<'
        auto node = std::make_unique<XmlNode>();
        node->name = localName(name());
        for (;;) {
            skipSpace();
            if (pos_ >= t_.size()) throw ParseError("XML: unterminated tag");
            if (t_[pos_] == '/') {
                if (pos_ + 1 >= t_.size() || t_[pos_ + 1] != '>') throw ParseError("XML: bad empty tag");
                pos_ += 2;
                return node;
            }
            if (t_[pos_] == '>') {
                ++pos_;
                break;
            }
            std::string key = name();
            skipSpace();
            if (pos_ >= t_.size() || t_[pos_] != '=') throw ParseError("XML: attribute without value");
            ++pos_;
            skipSpace();
            if (pos_ >= t_.size() || (t_[pos_] != '"' && t_[pos_] != '\'')) throw ParseError("XML: unquoted attribute");
            char q = t_[pos_++];
            size_t e = t_.find(q, pos_);
            if (e == std::string::npos) throw ParseError("XML: unterminated attribute");
            node->attrs.emplace_back(std::move(key), unescape(t_.substr(pos_, e - pos_)));
            pos_ = e + 1;
        }
        for (;;) {
            skipMisc();
            if (pos_ >= t_.size()) throw ParseError("XML: missing end tag");
            if (startsWith("</")) {
                skipUntil(">");
                return node;
            }
            node->children.push_back(element(depth + 1));
        }
    }
};

}  // namespace

std::unique_ptr<XmlNode> parseXml(const std::string& text) { return Parser(text).run(); }

}  // namespace tia
