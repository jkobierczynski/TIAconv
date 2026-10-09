// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save history: what changed from one save to the next. A project file keeps
// earlier versions of its objects, so the project can be read as it was after
// each save; the history is the difference between consecutive states of
// what tiaconv reports (hardware, network, security settings, connections,
// blocks and the code of their networks, data block members, tags, HMI tags,
// constants).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "code.hpp"
#include "inventory.hpp"
#include "project.hpp"

namespace tia {

struct HistoryChange {
    std::string change;     // "added", "removed" or "changed"
    std::string kind;       // "device", "module", "interface", "block", "tag", ...
    std::string item;       // "ZZBRAVO / Block_1 [FB1]"
    std::string description;  // added, removed: what the item is, "FB, LAD"
    std::string attribute;  // changed: which attribute
    // changed: its value before and after; empty = not set. For the code of
    // a network these are lines of text, separated by line feeds: of a
    // changed network the lines taken out and the lines put in, without
    // what stayed the same before and after them; of an added network its
    // code in `to`, of a removed one in `from`.
    std::string from, to;
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

// The difference between two projects, or two states of one: what the
// second has that the first has not, what it lacks, what changed.
struct ProjectDiff {
    std::vector<HistoryChange> changes;
    // The two share the identities of their objects (one is a later version
    // of the other). Otherwise items are paired by kind and name.
    bool sameLineage = true;
    size_t matchedByName = 0;   // items paired by name although their identities differ
    size_t itemsOld = 0, itemsNew = 0;
    size_t unreadBlocks = 0;  // blocks whose code was not compared: protected on either side
};

// A change that says more than that something was compiled or touched:
// not a time stamp, not a bare "modified", not a size that compiling sets.
bool substantialChange(const HistoryChange& c);

// Compares the two projects as tiaconv reports them. `oldProtected` and
// `newProtected`: from protectedVersions() of each whole file. A block that
// is know-how protected on either side is compared without its code.
ProjectDiff diffProjects(const Project& oldProject, const ProtectedVersions& oldProtected, const Project& newProject,
                         const ProtectedVersions& newProtected);

// Reads the file once per save. `throughSave` limits the history to the
// first so many saves; 0 = all of them.
// `progress` is called before each state is read, with its number and how
// many there are.
History buildHistory(std::shared_ptr<const std::vector<uint8_t>> data, size_t throughSave = 0,
                     const std::function<void(size_t, size_t)>& progress = {});

}  // namespace tia
