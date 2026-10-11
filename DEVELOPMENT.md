# Development

How tiaconv is developed: the ground rules, how a question about the file
format is settled, how a release is checked, and what is still open. The
user-facing description is in [README.md](README.md), the file format in
[docs/FORMAT.md](docs/FORMAT.md), the history of releases in
[CHANGELOG.md](CHANGELOG.md).

State at the time of writing: version 0.13.0. TIA Portal V21 is available
through a trial licence until about 27–29 October 2026; everything under
"Open work, while the trial lasts" needs it.

## Ground rules

These hold for every change; a feature that cannot respect them is not
added.

- **Read only.** tiaconv reads a project and never writes to it. Test
  projects are only read as well, from a copy.
- **Protection is reported, never removed.** Know-how protected blocks are
  listed as protected and their code is not read, also from earlier saves
  that still hold a version from before the protection
  (`protectedVersions`). Copy protection and write protection are reported
  as settings.
- **No passwords.** No output contains a password or anything derived from
  one, and tiaconv does not work on the password protection scheme.
  `--objects` redacts the text of every attribute whose name contains
  `assword`, `Salt`, `ProtectionIV` or `VerificationTag` (blobs by size only),
  every value that a structure itself names as a password, and everything
  below an attribute with such a name.
- **Users and roles are not read.** They are stored in protected form
  (`...Encrypted` attributes). These attributes are not examined or decoded.
- **Test data is made by the author.** The fixtures in `tests/fixtures` are
  projects made for this repository in TIA Portal V21, with throwaway
  passwords. Projects of others (the public samples used during
  development) are not added to the repository.
- Commits and pushes are made by the author of the repository.

## Repository

| path | what |
|---|---|
| `src/` | the C++17 sources: container and blocks (`container`), the type model (`meta`), objects (`project`), hardware inventory (`inventory`), program (`program`), block code (`code`), save history and diff (`history`), outputs (`output`), the command line (`main`), loading folders and archives (`source`), XML (`xml`) |
| `tests/tests.cpp` | unit tests on small hand-made inputs |
| `tests/fixtures_test.cpp` | tests against the V21 test projects: every documented save, the code listings and cross-references compared with TIA Portal |
| `tests/cli_*.cmake` | the command line: CSV headers, `--history`, `--save`, `diff` |
| `tests/fixtures/` | the test projects and their documentation (`README.md`: one table per project, one row per save) |
| `tools/plf_explore.py` | the Python prototype the C++ code started from; used for exploring the format (`Project`, `decode`) |
| `tools/check_storage_rule.py` | checks the rule for which attributes are stored |
| `tools/inventory_prototype.py` | the first hardware inventory, in Python |
| `docs/FORMAT.md` | the file format as far as it is known, with what is verified and what is not |

## Building and testing

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

CI (`.github/workflows/build.yml`) builds and tests on Linux, macOS and
Windows with `-DTIACONV_STATIC=ON` and attaches the three binaries to a
release for every `v*` tag.

Before a release, besides the tests:

1. **Sanitizers.** A build with `-fsanitize=address,undefined` runs the
   whole `ctest` suite (about ten minutes).
2. **Damaged files.** A fuzz run of a few hundred damaged copies of the
   project files (bit flips, damaged XML in the code and reference parts,
   truncation, overwritten runs, 32-bit fields set to extremes) through the
   sanitizer build with every output switched on, `--history` and `--save N`
   included. Expected exit codes are 0, 1 and 2; anything else, or any
   sanitizer message, is a bug. The same for `tiaconv diff`.
3. **Regression.** Every output (`--members`, `--csv`, `--tags-csv`,
   `--blocks-csv`, `--block-list-csv`, `--xref-csv`, `--code`, `--json`)
   of the previous release and the new one, on all fixtures and samples;
   every difference must be one the changelog announces.
4. **Package from scratch.** The source zip is unpacked into an empty
   directory, configured, built and tested there.
5. **Windows binary.** Cross-compiled with mingw-w64 and stripped. It has not
   yet been run on Windows (see Housekeeping).

The fuzz and regression scripts used so far were kept outside the
repository; adding them under `tools/` is on the list below.

## How a question about the format is settled

The format is not documented by Siemens. What tiaconv claims is settled by
a test project made in TIA Portal for the purpose, compared with what TIA
Portal itself shows.

1. **One project, one action per save.** A step list is agreed on first
   (one row per save). The author does each action in TIA Portal V21 and
   saves; the project file then holds every state, and the save history
   shows each action on its own. A step that took two saves, or two steps in
   one save, is fine: it is noted in the fixture's table as it happened.
2. **Staging.** After each step or group of steps, `System/PEData.plf` is
   copied from the author's PC (read only, one folder at a time) and
   **a copy of every state is kept**. TIA Portal can rewrite the whole file
   (it did when a block was know-how protected) and drop the earlier saves;
   without the copies, those states are lost. TIA Portal keeps a backup of
   its own in `<project>.backup/<date>.<time>/…zip`.
3. **References from TIA Portal.** Screenshots of the editor, of the
   cross-reference list (Tools > Cross-references, "Show objects with
   references", the Access column wide enough to read), of tables and
   dialogs; sources generated from blocks (right-click > Generate source
   from blocks, one block at a time: an SCL and an STL block together are
   refused); TIA Portal's own exports of tag tables.
4. **Compare, then pin.** tiaconv's output is compared with the reference;
   what differs is fixed, and the result is pinned in
   `tests/fixtures_test.cpp` (the history of each save, the listings, the
   cross-reference rows). The fixture's `README.md` gets the step table and
   the list of references.
5. **Document.** FORMAT.md describes the stored form and says what was
   verified against what; the README's verification table says what was
   checked and what was not.

Pitfalls seen so far:

- A step list that names a tag renamed earlier in the same project
  (`"ZZA"` after it became `"ZZALPHA"`) produces an undefined name. Check
  the current names before writing steps.
- The name typed in the "Add new block" dialog can be missed; the block
  then appears as `Block_1`.
- In a step list written as a Markdown table, `-|P|-` breaks the table.
- Names stored in the code and the reference table can be older than the
  object's name until the block is opened or compiled; tiaconv uses the
  object's current name (the reference table's links).
- A compile error in a test project (a timer's ET wired to a read-only
  member) blocks the steps after it; the author removes it and recompiles.

## Done

The changelog has the details. In short:

- Container, type model, object decoding for V13, V15.1, V16, V19, V21.
- Hardware inventory: devices, modules, firmware, addresses, subnets, IO
  systems, port connections, S7 and HMI connections, CPU security settings.
- Program: block list with properties and protection, tags, constants,
  data block members with types, start values, comments and offsets, HMI
  tags.
- Save history (`--history`) and the state after any save (`--save N`).
- Block code (`--code`): SCL, STL, LAD and FBD per network, with the
  cross-reference (`--xref-csv`) in TIA Portal's words.
- `tiaconv diff`: two projects, or two saves of one, in the terms of the
  save history.
- Verified against TIA Portal in V21: see the README's verification table.
  In 0.12.1 and 0.13.0: every kind of access of the cross-reference list
  (read only, write, read and write, call, single instance, multiple
  instance, definition and jump of a label); multi-instances of FBs,
  counters and timers; SCL `WHILE`, `REPEAT`, `CASE` ranges, comments in
  several languages; STL comments, labels, jumps, `CALL` with parameters;
  LAD negated coil and edge contacts.

## Open work, while the trial lasts

Each item is a short series in a test project; the project to use is
named. In order of value per minute of the author's time.

1. **`ZZSCL2` cross-references** (`s14_code`). One screenshot of the list
   for `ZZSCL2`, Access column wide. Its seven rows (`"ZZN1"`, `"ZZN2"` read
   and write, `"ZZN3"` read, `"ZZOUT"`, `"ZZC"` write) are pinned in the
   tests as tiaconv gives them but not yet compared.

2. **The interface of an FB or FC in the history** (`s14_code`). Today an
   FB's own Input, Output, InOut, Static and Temp members are not items of
   the history (`memP`, `memN` of `ZZLAD2` do not appear). Steps, one save
   each, on `ZZLAD2` or a new FB:
   add an Input `start` (Bool); add an Output `count` (Int); rename the
   Static `memP` to `edgeP`; change the type of `count` to DInt; give
   `start` a comment; set a default value; delete a member. Reference: a
   screenshot of the interface after the series. This is also the start of
   "Declarations" below. (The interface is already read for FBs that are
   not know-how protected, for the multi-instance rows of the
   cross-reference: `BlockInfo::interfaceMembers`.)

3. **Offsets in an instance block with standard access** (`s14_code` or
   a new project). An FB with "Optimized block access" switched off, four
   or five members in Input, Output, InOut and Static (Bool, Int, Real, a
   String), called once to get its instance block. Reference: the Offset
   column of the instance block in the editor. So far section offsets plus
   member offsets have only been seen in the V13 sample.

4. **Instances of library types** (`s14_code`). Add a block to the project
   library as a type, release it, use an instance of it in the program,
   save; then a new version of the type and an update of the instance.
   The relation `IsInstanceOf` is not read yet; the block list should say
   which type and version a block is an instance of.

5. **Two projects made apart** (new project). A new project with a CPU,
   some of the same tags and two of the same blocks copied in by hand (or
   through a global library). Then `tiaconv diff` against `s14_code`: the
   lineages differ, so items are paired by name; check that blocks and tags
   with the same names pair up and the differences make sense.

6. **Security on an S7-1500** (`s09_security`). On an S7-1500, change the
   rights of the Anonymous user, then the access level, one save each, with
   a screenshot of the protection settings after each. On the S7-1200 the
   access level follows the Anonymous user's rights; for the S7-1500 only a
   new CPU's state was compared.

7. **Technology objects and central modules** (new project or
   `s10_connections`). An S7-1500 with a PID_Compact technology object, a
   central DI/DQ module next to the CPU, and a speed axis if the trial
   allows it. These bring kinds of hardware and system constants not seen
   yet.

Not worth trial time: PROFIBUS, I-devices and shared devices (larger
hardware setups); GRAPH and fail-safe blocks (not read).

## Open work, features

- **Declarations.** The interface of FBs and FCs (sections, members, data
  types, default values, comments) in the outputs and the history; then
  the declaration part of a source file (`VAR_INPUT ... END_VAR`), so that
  an SCL or STL block can be written out as a whole.
- **`--code-dir DIR`.** One file per block (`.scl`, `.awl`, and a text file
  per LAD/FBD block in tiaconv's listing), named after the block, so that a
  project's code can be kept in git and compared with ordinary tools.
  Protected blocks get a file that says so, without code.
- **Security findings report.** From what is already read: PUT/GET access,
  web server and OPC UA settings, access levels, the Anonymous user,
  unprotected blocks that write outputs, data blocks writable from HMI or
  OPC UA, connections to unspecified partners. Each finding with the
  setting it comes from and why it matters for IEC 62443 / NIS2 work.
- **Asset and vulnerability export.** Order numbers and firmware versions in
  a form that can be matched with Siemens ProductCERT advisories (CSAF) or
  the CPE names used by vulnerability databases.
- **Network diagram.** Subnets, devices, IO systems and port connections as
  a Graphviz or Mermaid file.

## Housekeeping

- **Run `tiaconv.exe` on Windows** once, on the author's PC: on a test
  project folder, an `.ap21` file and an archive; with CSV output opened in
  Excel (the BOM and `--no-bom`).
- **Add the fuzz and regression scripts** under `tools/`, so that the
  release checks above can be run by anyone.
- **Keep `tools/__pycache__` out of the source zip.** `.gitignore` already
  keeps it out of the repository; the zip is packed from the working
  directory and has to leave it out as well.
- **First tagged release on GitHub**, so that CI attaches the three
  binaries; check that the Windows binary from CI behaves like the
  cross-compiled one.
- The README's counts (files, objects) and FORMAT.md's sample table are
  updated by hand when a fixture changes; a small script could print them.

## Open questions about the format

The list is kept in [docs/FORMAT.md](docs/FORMAT.md#open-questions). Those
that a test project could answer are in "Open work, while the trial lasts";
the others (V11, V12, V14, V17, V18, V20 layouts, projects with several
`.plf` files, the derivation of automatic PROFINET names, the `h`
attribute of a start value, ...) need projects or versions not at hand.
