// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finds and loads the PEData.plf of a project given a project folder, a
// project file (.apNN), a project archive (.zapNN) or the .plf itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tia {

struct LoadedSource {
    std::vector<uint8_t> data;
    std::string description;            // where the data came from
    std::vector<std::string> warnings;
};

// `path` is UTF-8. Throws std::runtime_error with a readable message.
LoadedSource loadProjectData(const std::string& path);

// The warnings for .plf files found next to PEData.plf (file names only).
// A large project keeps further data files PEData-01.plf, PEData-02.plf, ...;
// they are named together in one warning, because without them what is
// reported is incomplete. They are not read: how their contents fit with
// PEData.plf is not known.
std::vector<std::string> extraDataFileWarnings(std::vector<std::string> names);

}  // namespace tia
