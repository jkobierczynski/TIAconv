// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "miniz.h"

namespace fs = std::filesystem;

namespace tia {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.u8string());
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) throw std::runtime_error("cannot read " + p.u8string());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("cannot read " + p.u8string());
    return data;
}

bool isZip(const std::vector<uint8_t>& d) { return d.size() > 4 && std::memcmp(d.data(), "PK\x03\x04", 4) == 0; }

// Other .plf files next to PEData.plf are not understood yet; say so.
void noteSiblings(const fs::path& plf, LoadedSource& out) {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(plf.parent_path(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (lower(e.path().extension().u8string()) != ".plf") continue;
        if (lower(e.path().filename().u8string()) == "pedata.plf") continue;
        names.push_back(e.path().filename().u8string());
    }
    for (auto& w : extraDataFileWarnings(names)) out.warnings.push_back(std::move(w));
}

// "PEData-01.plf": a further data file of a large project.
bool isDataPart(const std::string& name) {
    const std::string n = lower(name);
    const std::string head = "pedata-", tail = ".plf";
    if (n.size() <= head.size() + tail.size() || n.compare(0, head.size(), head) != 0 ||
        n.compare(n.size() - tail.size(), tail.size(), tail) != 0)
        return false;
    const std::string digits = n.substr(head.size(), n.size() - head.size() - tail.size());
    return digits.find_first_not_of("0123456789") == std::string::npos;
}

LoadedSource fromPlf(const fs::path& plf) {
    LoadedSource out;
    out.data = readFile(plf);
    out.description = plf.u8string();
    noteSiblings(plf, out);
    return out;
}

LoadedSource fromFolder(const fs::path& dir) {
    for (const fs::path& candidate : {dir / "System" / "PEData.plf", dir / "PEData.plf"}) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return fromPlf(candidate);
    }
    throw std::runtime_error("no System/PEData.plf found in " + dir.u8string());
}

LoadedSource fromArchive(const fs::path& file, const std::vector<uint8_t>& zipData) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        throw std::runtime_error("cannot read archive " + file.u8string());
    LoadedSource out;
    bool found = false;
    std::vector<std::string> extra;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        std::string name = lower(st.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string suffix = ".plf";
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const bool main = name == "system/pedata.plf" ||
                          (name.size() > 18 && name.compare(name.size() - 18, 18, "/system/pedata.plf") == 0);
        if (!main) {
            std::string base = st.m_filename;
            std::replace(base.begin(), base.end(), '\\', '/');
            extra.push_back(base.substr(base.rfind('/') == std::string::npos ? 0 : base.rfind('/') + 1));
            continue;
        }
        if (found) continue;
        if (st.m_uncomp_size > (static_cast<mz_uint64>(1) << 32)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("PEData.plf in the archive is too large");
        }
        out.data.resize(static_cast<size_t>(st.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip, i, out.data.data(), out.data.size(), 0)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("cannot extract PEData.plf from " + file.u8string());
        }
        out.description = file.u8string() + " -> " + st.m_filename;
        found = true;
    }
    mz_zip_reader_end(&zip);
    if (!found) throw std::runtime_error("no System/PEData.plf inside " + file.u8string());
    for (auto& w : extraDataFileWarnings(extra)) out.warnings.push_back(std::move(w));
    return out;
}

}  // namespace

std::vector<std::string> extraDataFileWarnings(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    std::vector<std::string> parts, out;
    for (const auto& n : names) {
        if (isDataPart(n)) parts.push_back(n);
        else out.push_back("additional data file not read: " + n);
    }
    if (!parts.empty()) {
        std::string list;
        for (const auto& n : parts) list += (list.empty() ? "" : ", ") + n;
        out.insert(out.begin(), "the project is spread over several data files; only PEData.plf is read, not " + list +
                                    ", so what is reported is incomplete");
    }
    return out;
}

LoadedSource loadProjectData(const std::string& path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return fromFolder(p);
    if (!fs::is_regular_file(p, ec)) throw std::runtime_error("no such file or folder: " + path);

    const std::string ext = lower(p.extension().u8string());
    if (ext == ".plf") return fromPlf(p);
    // .ap13 / .ap15_1 / .al17 ...: the project file sits beside the System folder.
    if (ext.rfind(".ap", 0) == 0 || ext.rfind(".al", 0) == 0) return fromFolder(p.parent_path().empty() ? fs::path(".") : p.parent_path());

    std::vector<uint8_t> data = readFile(p);
    if (isZip(data)) return fromArchive(p, data);

    LoadedSource out;
    out.data = std::move(data);
    out.description = path;
    return out;
}

}  // namespace tia
