// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tia {

// SHA-256 of a byte range (FIPS 180-4).
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);

}  // namespace tia
