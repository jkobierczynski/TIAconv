// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save history: what changed from one save to the next. A project file keeps
// earlier versions of its objects, so the project can be read as it was after
// each save; the history is the difference between consecutive states of
// what tiaconv reports (hardware, network, security settings, connections,
// blocks, data block members, tags, constants).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "inventory.hpp"

namespace tia {

struct HistoryChange {
    std::string change;     // "added", "removed" or "changed"
    std::string kind;       // "device", "module", "interface", "block", "tag", ...
    std::string item;       // "ZZBRAVO / Block_1 [FB1]"
    std::string description;  // added, removed: what the item is, "FB, LAD"
    std::string attribute;  // changed: which attribute
    std::string from, to;   // changed: its value before and after; empty = not set
    // added, removed: the item this one is part of when that was added or
    // removed in the same save (a module of a new station); empty otherwise
    std::string partOf;
    // changed: the attribute is a time stamp TIA Portal keeps ("compiled")
    bool isTime = false;
    // changed: says only that something changed ("modified")
    bool isMarker = false;
    // changed: a value that follows from compiling ("load memory")
    bool isResult = false;
    std::string key, parentKey;  // internal identity of the item and of what it belongs to
};

struct HistorySave {
    size_t number = 0;  // 1 = the first save in the file
    // Not a save: what the file holds after its last save marker. `number`
    // is then one more than the number of saves.
    bool afterLastSave = false;
    // When. "save": the time stored with the save itself (older layout).
    // "latest_change": the newer layout stores no time for a save; this is
    // the latest "modified" time among the objects the save wrote, so the
    // save was made then or later. Empty when no object it wrote has a time.
    std::string time;
    std::string timeSource;
    // Who: "last modified by" of the project object when the save wrote it,
    // otherwise of the object changed last that names a user.
    std::string by;
    bool hasProject = false;  // the file holds a project object at this save
    // Nothing of a project is in the file yet: a file starts with a save
    // that holds only the type model.
    bool beforeProject = false;
    // The first state of the project in the file. Its contents are counted,
    // not listed as changes.
    bool firstState = false;
    std::vector<std::pair<std::string, size_t>> contents;  // firstState: kind, number of items
    size_t objectsWritten = 0;  // objects this save wrote to the file, deleted ones included
    size_t objectsDeleted = 0;  // of those, written as deleted
    // The same per object type, most frequent first: type name, written, deleted
    struct TypeCount {
        std::string type;
        size_t written = 0, deleted = 0;
    };
    std::vector<TypeCount> objectTypes;
    std::vector<HistoryChange> changes;
    std::string problem;  // the state after this save could not be read
};

struct History {
    size_t savesInFile = 0;
    std::vector<ProjectEvent> events;  // TIA Portal's own project history, as of the last save covered
    std::vector<HistorySave> saves;
    std::vector<std::string> notes;
};

// Reads the file once per save. `throughSave` limits the history to the
// first so many saves; 0 = all of them.
// `progress` is called before each state is read, with its number and how
// many there are.
History buildHistory(std::shared_ptr<const std::vector<uint8_t>> data, size_t throughSave = 0,
                     const std::function<void(size_t, size_t)>& progress = {});

}  // namespace tia
