// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PEData.plf container: a file header followed by a list of
// object blocks. See docs/FORMAT.md.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bytes.hpp"

namespace tia {

enum class Layout {
    V11,  // seen in V13 projects: 46-byte file header, 28-byte block header, no hashes
    V14   // seen in V15.1, V16, V19: 98-byte file header, 44-byte block header, SHA-256 per block
};

const char* layoutName(Layout l);

struct Block {
    size_t offset = 0;      // of the block in the file
    uint32_t size = 0;      // including the block header, excluding a trailing hash
    uint32_t type = 0;      // object type id (0 / 0x7xxxx for system objects)
    uint64_t id = 0;        // object id
    uint16_t flags = 0;     // bit 0x04: object deleted
    uint8_t slotCount = 0;  // number of entries in the slot table
    uint32_t headerLen = 0;

    bool deleted() const { return (flags & 0x04) != 0; }
};

struct SaveMarker {
    size_t offset = 0;
    std::string kind;    // "commit" or "close"
    uint64_t ticks = 0;  // .NET DateTime ticks, kind bits included
};

struct ContainerOptions {
    bool verifyHashes = false;
    // Read the project as it was after this many saves; 0 = as it is now.
    // Saves append to the file, so earlier states are usually still in it
    // (TIA Portal rewrites the file now and then, which drops them).
    size_t throughSave = 0;
};

class Container {
public:
    // Takes ownership of the file contents. Throws ParseError when the data is
    // not a recognisable PLF file.
    static Container parse(std::vector<uint8_t> data, const ContainerOptions& opt = {});

    Layout layout() const { return layout_; }
    Span data() const { return Span(data_.data(), data_.size()); }
    const std::vector<Block>& blocks() const { return blocks_; }
    const std::vector<SaveMarker>& markers() const { return markers_; }

    // Bytes of one block, header included.
    Span blockData(const Block& b) const { return data().sub(b.offset, b.size); }

    bool isSystem(const Block& b) const;

    // Payload of a system object: the bytes between its length field and the
    // 0xFF terminator.
    Span systemPayload(const Block& b) const;

    // Latest version of every object, keyed by (type, id). Later blocks
    // supersede earlier ones.
    const std::map<std::pair<uint32_t, uint64_t>, size_t>& latest() const { return latest_; }

    // Number of completed saves recorded in the file. In the newer layout
    // each save ends with one system object of type 0x7000C (verified by
    // saving a V21 project 23 times, one change per save); in the older one
    // with a commit marker.
    size_t saveCount() const { return saveCount_; }

    size_t hashErrors() const { return hashErrors_; }
    bool hashesVerified() const { return hashesVerified_; }
    // False when the block list stopped before the end of the file.
    bool complete() const { return complete_; }
    const std::string& stopReason() const { return stopReason_; }

private:
    std::vector<uint8_t> data_;
    Layout layout_ = Layout::V14;
    std::vector<Block> blocks_;
    std::vector<SaveMarker> markers_;
    std::map<std::pair<uint32_t, uint64_t>, size_t> latest_;
    size_t saveCount_ = 0;
    size_t hashErrors_ = 0;
    bool hashesVerified_ = false;
    bool complete_ = true;
    std::string stopReason_;
};

}  // namespace tia
