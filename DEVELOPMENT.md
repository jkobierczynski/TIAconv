# Development

What tiaconv is built on, what has been found and implemented so far, and
what is still unclear or still to do. The user-facing description is in
[README.md](README.md), the file format in [docs/FORMAT.md](docs/FORMAT.md),
the releases in [CHANGELOG.md](CHANGELOG.md).

State at the time of writing: version 0.15.0. A STEP 7 Professional V21
trial licence is available until about 27–29 October 2026; the items marked
*(trial)* below need it.

## Ground rules

These hold for every change; a feature that cannot respect them is not
added.

- **Read only.** tiaconv reads a project and never writes to it.
- **Protection is reported, never removed.** Know-how protected blocks are
  listed as protected and their code is not read, also from earlier saves
  that still hold a version from before the protection. Copy protection and
  write protection are reported as settings.
- **No passwords.** No output contains a password or anything derived from
  one, and tiaconv does not work on the password protection scheme.
  `--objects` redacts the text of every attribute whose name contains
  `assword`, `Salt`, `ProtectionIV` or `VerificationTag` (blobs by size only),
  every value that a structure itself names as a password, and everything
  below an attribute with such a name.
- **Users and roles are not read.** They are stored in protected form
  (`...Encrypted` attributes); these attributes are not examined or decoded.
- **Test data is made by the author.** The fixtures in `tests/fixtures` are
  projects made for this repository in TIA Portal V21, with throwaway
  passwords. Projects of others are not added to the repository.

## Repository

| path | what |
|---|---|
| `src/` | C++17: container and blocks (`container`), type model (`meta`), objects (`project`), hardware inventory (`inventory`), program (`program`), block code (`code`), save history and diff (`history`), outputs (`output`), command line (`main`), loading folders and archives (`source`), XML (`xml`) |
| `tests/tests.cpp` | unit tests on small hand-made inputs |
| `tests/fixtures_test.cpp` | tests against the V21 test projects, compared with what TIA Portal shows |
| `tests/cli_*.cmake` | the command line |
| `tests/fixtures/` | the test projects; `README.md` there documents each save |
| `tools/` | the Python prototype the C++ code started from (`plf_explore.py`), and a check of the attribute storage rule |
| `docs/FORMAT.md` | the file format as far as it is known |

## Building and checking

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build

CI (`.github/workflows/build.yml`) builds and tests on Linux, macOS and
Windows with `-DTIACONV_STATIC=ON` and attaches the three binaries to a
release for every `v*` tag.

Before a release, besides the tests: the whole suite under
AddressSanitizer and UBSan; a few hundred damaged copies of the project
files through the sanitizer build with every output on (exit codes 0, 1, 2
only, no sanitizer message), for `tiaconv diff` too; every output of the
previous release and the new one compared on all fixtures and samples, each
difference announced in the changelog; the source zip unpacked, built and
tested in an empty directory.

How a format question is settled: a small project made in TIA Portal for
the purpose, one action per save, so that the file holds every state and
the save history shows each action on its own; then compared with what
TIA Portal shows (the editor, the cross-reference list, generated sources,
its own exports). What agrees is pinned in the fixture tests; FORMAT.md
says what was verified against what. Copies of every state are kept,
because TIA Portal can rewrite the whole file.

## What was found and implemented

### The file

- `System/PEData.plf` is a list of objects plus a compressed XML
  description of every object type, so it describes itself. Two layouts:
  "v11" (V13) and "v14" (V15.1 to V21, with a SHA-256 after every block).
  The last block per object wins; save markers separate the saves
  (`$$COMMIT$$` / `##CLOSE###` records in V13, `Commit` / `Close` system
  objects later), so a file normally holds every earlier state.
- Which attributes of an object are stored follows a rule that reproduces
  the layout table the V13 sample carries for all 2 629 object types.
- Structures and arrays in attribute values, relations, and relation lists
  without ids (typed lists) are decoded.
- TIA Portal does not only append: it can rewrite the file and drop the
  earlier saves. Setting know-how protection does this; the old file is
  kept in `<project>.backup/<date>.<time>/…zip`, and that zip still holds
  the block's code from before the protection.
- A file can start with a save that holds only the type model; a project
  archive goes on after its last save marker.

### Hardware and network (0.2.0, 0.5.0, 0.6.0)

- Devices, modules, order numbers, firmware versions, interfaces, IP
  addresses, masks, routers, subnets, PROFINET names; addresses assigned
  outside the project are reported as such.
- With "generate PROFINET device name automatically" TIA Portal derives the
  name and does not update the stored copy. tiaconv derives it as TIA Portal
  does (0.15.0): the CPU's name in lower case, and the converted name with
  `xa`, `xb`, `xd`, `xh`, `xq` codes and a CRC-16/ARC, or IDNA for letters
  beyond ASCII (FORMAT.md, "Automatic PROFINET names").
- PROFINET IO systems, port connections (topology), S7 and HMI connections
  including partners outside the project.
- CPU security settings: access level, PUT/GET, web server, OPC UA server,
  NTP, display protection, access control, legacy communication, protection
  of configuration data. On CPUs with user management the stored access
  level is the one TIA Portal derives from the Anonymous user's rights;
  users and roles decide the rest and are not read.

### Program (0.3.0 to 0.8.0, 0.10.0)

- PLC tags with comments; data blocks with members, data types, sections,
  start values, comments and offsets; PLC data types and their defaults.
- The block list: number, kind, language, folder, protection (know-how,
  write, copy protection with serial), compile state, time stamps, download
  history, memory sizes, and for data blocks write protection in the device
  and access from OPC UA and the web server.
- Constants: hardware identifiers with what each stands for, user
  constants, other system constants.
- HMI tags of each HMI device, with connection, the PLC at the other end,
  the PLC tag or data block member, address, acquisition cycle and mode,
  limits, linear scaling and the index tag of a multiplexed tag (0.15.0).

### Save history and diff (0.9.0, 0.12.0)

- `--history`: what each save added, removed or changed, with time and
  user; `--save N`: the project as after save N. In the v14 layout a save
  carries no time of its own; the latest change it holds is a lower bound.
- TIA Portal's own project events (created, converted) are read.
- `tiaconv diff`: two projects or two saves, in the terms of the history.
  Versions of one project (Save as included) keep their object identities
  and creation time and are paired by identity; projects made apart are
  paired by name, because object ids are sequential and coincide by chance.

### Interfaces of FBs and FCs (0.14.0)

- An FB or FC keeps its interface in the same form as a data block
  (sections, members, default values, comments). The save history and
  `tiaconv diff` list its members; a rename is a removed and an added
  member, because members have no identity of their own. Not read for
  know-how protected blocks.
- Offsets of an instance block with standard access follow from the FB's
  interface: checked against TIA Portal's Offset column.

### Block code (0.11.0 to 0.13.0)

- A code block's networks are the `CompileUnitData` objects of its relation
  `Sources`, in network order; each has a stable id (`RefID`), a title, a
  comment, a language and the code as an XML document.
- SCL is stored as a token tree of the source text; STL as one statement
  per line; LAD and FBD as parts and wires. tiaconv writes SCL and STL as
  their source text (identical to TIA Portal's editor and generated
  sources in all comparisons), and LAD/FBD in a text notation of its own.
- Every block keeps a reference table of what its code names, with the kind
  of access per use; it is the data TIA Portal's cross-reference list is
  made from. TIA Portal's list has a row per use; `--xref-csv` has one row
  per network and kind of access, the JSON every use. It is written in TIA
  Portal's words: read only, write,
  read and write, call, single instance, multiple instance, and definition
  and jump for labels. A data block has a row of its own only where it is
  used as a whole; a multi-instance an FB declares has a row for its
  declaration (an FB's, not an instruction's: TIA Portal lists none for a
  timer or counter instance).
- Names in the stored code can be older than the object's name until the
  block is compiled; the reference table links each tag, block and data
  block to the object, so tiaconv shows the current name, as the editor
  does.
- Constructs checked against TIA Portal V21: SCL `IF`/`ELSE`, `FOR`, `CASE`
  with ranges, `WHILE`, `REPEAT`, `REGION`, calls, comments of all three
  kinds; STL instructions, comments, labels, jumps, `CALL` with parameters;
  LAD contacts, branches, set/reset and negated coils, edge contacts,
  comparisons, MOVE, ADD, timers, counters, block calls, multi-instances;
  FBD with negated inputs.

## What is still unclear or to do

### Needs TIA Portal *(trial)*

- **The other columns of an interface**: Retain, accessible from HMI,
  writable, visible, setpoint. Not read.
- **Instances of library types** (relation `IsInstanceOf`): not read; the
  block list should say which type and version a block is an instance of.
- **Two projects made apart** with largely the same contents: where
  `tiaconv diff` pairs by name; no such pair has been compared.
- **Security on an S7-1500**: whether the access level follows the
  Anonymous user's rights as on the S7-1200 (only a new CPU's state was
  compared). Also unknown: the access level numbers of an S7-1200 with
  firmware V4.0 to V4.6.
- **The automatic PROFINET name** for combinations not seen (an umlaut
  with a space, a dot with characters to convert, a hyphen at the end), for
  IO devices and for CPUs with two PROFINET interfaces.
- **HMI leftovers**: the list of a multiplexed tag (not found in the file),
  upper limit 1 and lower limit 1 (assumed); PC runtimes and WinCC Unified;
  HMI tags in V13.
- **A user constant in TIA Portal's cross-reference list**: tiaconv lists it
  as read; not compared.
- **The download state** (TiaProjectParser's two versions of an object) and
  download times: PLCSIM Advanced did not start an instance here.
- **Technology objects and S7-1500 central modules**: identifier kinds and
  objects not seen yet; technology objects are listed like blocks, their
  meaning is not looked at.
- **Download times**: no test project was ever downloaded.
- Not worth trial time: PROFIBUS, I-devices, shared devices, MRP domains;
  GRAPH and fail-safe blocks (not read).

### Needs projects not at hand

- V11, V12, V14, V18 and V20 projects (untested; V17 read from one
  third-party archive, not compared with TIA Portal); projects split over
  several data files (`PEData-01.plf`, ...: not read, the tool names them
  in a warning; how they fit together needs one such project); protected
  (encrypted) projects (reported, not opened).
- V13: code reads as plausible SCL but has no source to compare with;
  comments in V13 projects are not decoded.
- Anonymous structures: checked on the V19 sample only.
- Further open questions about the format are listed in
  [docs/FORMAT.md](docs/FORMAT.md#open-questions).

### Features

- **Declarations.** The interface of FBs and FCs is read and in the history
  (0.14.0); still to do: in the text report and the JSON, and as the
  declaration part of a source (`VAR_INPUT ... END_VAR`), so that an SCL or
  STL block comes out as a complete source like the one TIA Portal
  generates.
- **`--code-dir DIR`.** One text file per block, so that a project's code
  can be kept in git; with `tiaconv diff`, change management without
  commercial tools. Protected blocks get a file that says so, without code.
- **Security findings report** (NIS2 / IEC 62443). Brings together what is
  now spread over the report: PUT/GET enabled; no access protection or only
  the default level; web server on HTTP; legacy PG/HMI communication; OPC UA
  without security; no NTP; data blocks reachable from OPC UA or the web
  server; HMI tags writing into the PLC; a project file or its `.backup`
  zip that still holds blocks from before they were know-how protected.
  Each finding cites the setting it comes from; a mapping to IEC 62443
  requirements only where it is clear-cut.
- **Asset and vulnerability export.** Order numbers and firmware versions
  in a form that can be matched with Siemens ProductCERT advisories (their
  machine-readable files, supplied offline).
- **Network picture.** Subnets, stations, IP addresses and connections as
  a Mermaid or Graphviz diagram.

### Housekeeping

- Run the current `tiaconv.exe` on Windows. Version 0.4.1 ran there and
  matched the Linux output line for line; the builds since then are only
  cross-compiled, and the MSVC build in CI is unchecked for the newer code.
- Commit and tag the releases from 0.10.0 on, so that CI attaches the
  binaries.
- Add the fuzz and regression scripts under `tools/`, so that the release
  checks can be run by anyone.
- The README's counts (files, objects) and FORMAT.md's sample table are
  updated by hand when a fixture changes; a small script could print them.
