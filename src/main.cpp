// tiaconv - TIA Portal project reader
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "container.hpp"
#include "history.hpp"
#include "inventory.hpp"
#include "output.hpp"
#include "program.hpp"
#include "project.hpp"
#include "source.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef TIACONV_VERSION
#define TIACONV_VERSION "0.0.0"
#endif

namespace {

const char kUsage[] =
    "tiaconv " TIACONV_VERSION " - hardware, network, tags and data blocks from a TIA Portal project\n"
    "\n"
    "Usage: tiaconv [options] <project>\n"
    "\n"
    "  <project>   a project folder, a project file (.ap13 ... .ap21), a project\n"
    "              archive (.zap13 ... .zap21) or a PEData.plf file\n"
    "\n"
    "Options:\n"
    "  -j, --json FILE     write the inventory as JSON (\"-\" for standard output)\n"
    "  -c, --csv FILE      write the inventory as CSV, one row per module or interface\n"
    "      --tags-csv FILE    write the PLC tags as CSV\n"
    "      --hmi-tags-csv FILE    write the tags of the HMI devices as CSV\n"
    "      --constants-csv FILE   write the constants as CSV: hardware identifiers,\n"
    "                             user constants and the other system constants\n"
    "      --block-list-csv FILE  write the list of blocks as CSV, one row per block\n"
    "      --blocks-csv FILE  write the data block members as CSV\n"
    "      --no-bom        CSV files without the UTF-8 byte-order mark at the start\n"
    "      --members       print the members of every data block in the text report\n"
    "      --objects FILE  write every decoded object as JSON lines (for research)\n"
    "      --meta FILE     write the type model embedded in the project (XML)\n"
    "      --all-devices   also list device objects outside the project tree\n"
    "      --all-items     list every device item, including ports and internal items\n"
    "      --verify        check the SHA-256 hash of every block (V14+ projects)\n"
    "      --save N        show the project as it was after its N-th save\n"
    "      --history       add the save history: what changed with each save\n"
    "                      (reads the file once per save; with --save N, up to save N)\n"
    "      --history-csv FILE  write the save history as CSV, one row per change\n"
    "  -q, --quiet         do not print the text report\n"
    "  -h, --help          show this help\n"
    "  -V, --version       show the version\n"
    "\n"
    "The project is only read, never modified. TIA Portal does not need to be\n"
    "installed.\n";

struct Args {
    std::string input, json, csv, objects, meta, tagsCsv, blocksCsv, blockListCsv, constantsCsv, historyCsv, hmiTagsCsv;
    bool allDevices = false, allItems = false, verify = false, quiet = false, members = false, noBom = false;
    bool history = false;
    size_t save = 0;  // 0: the current state
};

// Writes to a file, or to standard output for "-".
template <typename Fn>
void emit(const std::string& target, Fn fn) {
    if (target == "-") {
        fn(std::cout);
        return;
    }
    std::ofstream f(std::filesystem::u8path(target), std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + target);
    fn(f);
    f.flush();
    if (!f) throw std::runtime_error("error while writing " + target);
}

// CSV files start with a UTF-8 byte-order mark: without it, Excel reads the
// file in the local code page and garbles every non-ASCII name or comment.
// Not on standard output, where the next program in a pipe would trip on it.
template <typename Fn>
void emitCsv(const std::string& target, bool bom, Fn fn) {
    emit(target, [&](std::ostream& o) {
        if (bom && target != "-") o << "\xef\xbb\xbf";
        fn(o);
    });
}

int run(const std::vector<std::string>& argv) {
    Args a;
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argv.size()) throw std::invalid_argument("option " + s + " needs a value");
            dst = argv[++i];
        };
        if (s == "-h" || s == "--help") {
            std::cout << kUsage;
            return 0;
        } else if (s == "-V" || s == "--version") {
            std::cout << "tiaconv " TIACONV_VERSION "\n";
            return 0;
        } else if (s == "-j" || s == "--json") value(a.json);
        else if (s == "-c" || s == "--csv") value(a.csv);
        else if (s == "--objects") value(a.objects);
        else if (s == "--tags-csv") value(a.tagsCsv);
        else if (s == "--hmi-tags-csv") value(a.hmiTagsCsv);
        else if (s == "--constants-csv") value(a.constantsCsv);
        else if (s == "--blocks-csv") value(a.blocksCsv);
        else if (s == "--block-list-csv") value(a.blockListCsv);
        else if (s == "--history") a.history = true;
        else if (s == "--history-csv") value(a.historyCsv);
        else if (s == "--members") a.members = true;
        else if (s == "--no-bom") a.noBom = true;
        else if (s == "--meta") value(a.meta);
        else if (s == "--all-devices") a.allDevices = true;
        else if (s == "--all-items") a.allItems = true;
        else if (s == "--verify") a.verify = true;
        else if (s == "--save") {
            std::string v;
            value(v);
            if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos || std::stoul(v) == 0)
                throw std::invalid_argument("--save needs the number of a save, 1 or higher");
            a.save = std::stoul(v);
        }
        else if (s == "-q" || s == "--quiet") a.quiet = true;
        else if (s.size() > 1 && s[0] == '-') throw std::invalid_argument("unknown option " + s);
        else if (a.input.empty()) a.input = s;
        else throw std::invalid_argument("more than one project given");
    }
    if (a.input.empty()) {
        std::cerr << kUsage;
        return 1;
    }

    tia::LoadedSource src = tia::loadProjectData(a.input);
    tia::ContainerOptions copt;
    copt.verifyHashes = a.verify;
    copt.throughSave = a.save;
    auto fileData = std::make_shared<const std::vector<uint8_t>>(std::move(src.data));
    tia::Project project(tia::Container::parse(fileData, copt));
    if (a.save && a.save > project.container().saveCount())
        throw std::invalid_argument(project.container().saveCount()
                                        ? "--save " + std::to_string(a.save) + ": the file records " +
                                              std::to_string(project.container().saveCount()) + " saves"
                                        : std::string("--save: this file records no saves"));
    if (project.meta().empty())
        throw tia::ParseError("no type model found in the file; it may be encrypted or of an unknown version");

    tia::InventoryOptions iopt;
    iopt.allItems = a.allItems;
    tia::Inventory inv = tia::buildInventory(project, iopt);
    for (auto& w : src.warnings) inv.warnings.push_back(std::move(w));
    if (a.verify && project.container().layout() == tia::Layout::V11)
        inv.warnings.push_back("--verify: this project layout stores no block hashes");

    tia::ProgramData prog = tia::buildProgramData(project);
    tia::linkHmiTags(inv, prog);

    tia::ReportContext ctx;
    ctx.toolVersion = TIACONV_VERSION;
    ctx.source = src.description;
    ctx.layout = tia::layoutName(project.container().layout());
    ctx.hashesVerified = project.container().hashesVerified();
    ctx.hashErrors = project.container().hashErrors();
    ctx.allDevices = a.allDevices;
    ctx.members = a.members;
    ctx.shownSave = a.save;

    tia::History history;
    if (a.history || !a.historyCsv.empty()) {
        // The file is read once per save: say so when that takes a while.
        const auto started = std::chrono::steady_clock::now();
        auto told = started;
        history = tia::buildHistory(fileData, a.save, [&](size_t n, size_t of) {
            const auto now = std::chrono::steady_clock::now();
            if (now - told < std::chrono::seconds(3)) return;
            told = now;
            std::cerr << "tiaconv: reading save " << n << " of " << of << " for the history\n";
        });
        if (a.history) ctx.history = &history;
    }

    // Keep standard output clean when a machine-readable format goes there.
    const bool stdoutTaken = a.json == "-" || a.csv == "-" || a.objects == "-" || a.meta == "-" ||
                             a.tagsCsv == "-" || a.blocksCsv == "-" || a.blockListCsv == "-" ||
                             a.constantsCsv == "-" || a.historyCsv == "-" || a.hmiTagsCsv == "-";
    if (!a.quiet) tia::writeText(stdoutTaken ? std::cerr : std::cout, inv, prog, ctx);
    if (!a.json.empty()) emit(a.json, [&](std::ostream& o) { tia::writeJson(o, inv, prog, ctx); });
    const bool bom = !a.noBom;
    if (!a.tagsCsv.empty()) emitCsv(a.tagsCsv, bom, [&](std::ostream& o) { tia::writeTagsCsv(o, prog); });
    if (!a.hmiTagsCsv.empty())
        emitCsv(a.hmiTagsCsv, bom, [&](std::ostream& o) { tia::writeHmiTagsCsv(o, prog); });
    if (!a.constantsCsv.empty())
        emitCsv(a.constantsCsv, bom, [&](std::ostream& o) { tia::writeConstantsCsv(o, prog); });
    if (!a.blockListCsv.empty())
        emitCsv(a.blockListCsv, bom, [&](std::ostream& o) { tia::writeBlockListCsv(o, prog); });
    if (!a.blocksCsv.empty()) emitCsv(a.blocksCsv, bom, [&](std::ostream& o) { tia::writeBlocksCsv(o, prog); });
    if (!a.historyCsv.empty())
        emitCsv(a.historyCsv, bom, [&](std::ostream& o) { tia::writeHistoryCsv(o, history); });
    if (!a.csv.empty()) emitCsv(a.csv, bom, [&](std::ostream& o) { tia::writeCsv(o, inv, ctx); });
    if (!a.objects.empty()) emit(a.objects, [&](std::ostream& o) { tia::writeObjects(o, project); });
    if (!a.meta.empty())
        emit(a.meta, [&](std::ostream& o) {
            for (const auto& x : project.metaXml()) o << x;
        });
    return 0;
}

int guarded(const std::vector<std::string>& argv) {
    try {
        return run(argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << "tiaconv: " << e.what() << "\nTry 'tiaconv --help'.\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "tiaconv: " << e.what() << "\n";
        return 2;
    }
}

}  // namespace

#ifdef _WIN32
int main() {
    SetConsoleOutputCP(CP_UTF8);
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; wargv && i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    if (wargv) LocalFree(wargv);
    return guarded(args);
}
#else
int main(int argc, char** argv) { return guarded(std::vector<std::string>(argv, argv + argc)); }
#endif
