// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal XML reader: elements, attributes and the text directly inside an
// element. Comments and processing instructions are skipped. Element names
// are stored without their namespace prefix.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tia {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;
    // The character data directly inside the element, entities resolved;
    // empty for an element that holds only white space between its children.
    std::string text;

    // First child element with this name, or nullptr.
    const XmlNode* child(const std::string& childName) const;

    // Attribute value, or nullptr when absent.
    const std::string* attr(const std::string& key) const;
    std::string attrOr(const std::string& key, const std::string& fallback) const;
    bool attrIs(const std::string& key, const std::string& value) const;
};

// Parses a document and returns its root element. Throws ParseError.
std::unique_ptr<XmlNode> parseXml(const std::string& text);

}  // namespace tia
