# tiaconv

Reads a Siemens TIA Portal project **without TIA Portal** and lists what is in
it:

- stations, CPUs and modules with order numbers and firmware versions,
- network interfaces with their IP settings and PROFINET names, and subnets,
- the security settings of each CPU: access level, PUT/GET, web server,
  OPC UA server, NTP, display protection, access control,
- PLC tags with data type, address and comment,
- data blocks with their members, data types, start values, comments and,
  for blocks with standard access, absolute offsets.

It is meant for asset inventories and security assessments (NIS2, IEC 62443),
where you are handed a project folder or archive and need the hardware list
and the data that is reachable over the network, without a licensed
engineering station.

- single executable, no installation, no dependencies
- Windows, Linux, macOS
- read-only: the project is never modified
- output as text, JSON or CSV

> **Status: early.** Checked against four public projects (V13, V15.1, V16,
> V19) and ten V21 test projects made for this repository. In those, every
> name, address and module was entered by hand and is read back exactly, and
> tags and function block interfaces match what TIA Portal itself exported.
> See [What is verified](#what-is-verified) for what is not covered yet.

## Usage

    tiaconv MyProject/                   # project folder
    tiaconv MyProject/MyProject.ap15_1   # project file
    tiaconv MyProject.zap15_1            # project archive
    tiaconv System/PEData.plf            # the data file itself

    tiaconv --json inventory.json --csv inventory.csv MyProject/
    tiaconv --members MyProject/         # also print every data block member
    tiaconv -q --tags-csv tags.csv --blocks-csv blocks.csv MyProject/

Example (a public V15.1 project):

    Project:  InveoRFIDPlayground
    Created:  2024-10-01T08:02:00.610Z  by User
    Modified: 2025-05-29T07:44:53.917Z  by User

    Device: S71500/ET200MP station_1  [S71500.Device]
      Pos   Name           Type                   Order number         Firmware  In
      0     Rail_0         Rack                   6ES7 590-1***0-0AA0  -         -
      1     PLC_4          CPU 1511-1 PN          6ES7 511-1AK02-0AB0  V2.6      Rail_0
            X1 (PROFINET interface_1)  IP 192.168.0.10 / 255.255.255.0  PROFINET name "plc_1"  subnet PN/IE_1
      3     CPU display_1  S71500.CPU.CpuDisplay  6ES7591-1AA00-0AA0   -         PLC_4

    Device: HMI_1  [HMI.Device]
      ...

    Subnets:
      PN/IE_1  (2 nodes)
          192.168.0.10     S71500/ET200MP station_1 / PLC_4 / X1
          192.168.0.3      HMI_1 / PROFINET Interface_1 / X1

    Tags:
      PLC    Table              Name   Type  Address
      PLC_4  Default tag table  Tag_1  Bool  %M0.0
      PLC_4  Default tag table  Tag_2  Bool  %M0.1
      ...

    Data blocks:
      PLC    Block    Name               Kind                        Access     Members
      PLC_4  DB3      lcd                global                      standard   8
            0.0     useLcd : Bool
            0.1     showTime : Bool
            0.2     clearLcd : Bool
            0.3     backLight : Bool := true
            2       line1 : String[20]
            24      line2 : String[20]
            ...
      PLC_4  DB5      KeypadFunction_DB  instance of KeypadFunction  optimized  6
            clear : Bool   [Input]
            number : String[20]   [Output]
            character : Byte   [InOut]
            lastChar : Char := ' '   [Static]
            ...

Options:

| option | |
|---|---|
| `-j`, `--json FILE` | inventory as JSON (`-` for standard output) |
| `-c`, `--csv FILE` | hardware inventory as CSV, one row per module or interface |
| `--tags-csv FILE` | PLC tags as CSV, one row per tag |
| `--blocks-csv FILE` | data block members as CSV, one row per member, nested members as `outer.inner` |
| `--no-bom` | write the CSV files without the UTF-8 byte-order mark (see below) |
| `--members` | print the members of every data block in the text report (the JSON always has them) |
| `--objects FILE` | every decoded object as JSON lines: attributes and relations. For research and for comparing two versions of a project |
| `--meta FILE` | the type model embedded in the project (XML) |
| `--all-devices` | also list device objects outside the project tree (copies kept for library types) |
| `--all-items` | list every device item, including ports and internal items |
| `--verify` | check the SHA-256 hash of every block (V14 and later) |
| `-q`, `--quiet` | no text report |

Exit status: 0 on success, 1 for a usage error, 2 when the project cannot be
read.

### Reading the output

- **IP addresses are what the project says**, not what a device answers on
  the network. When the project states that the address is assigned outside
  the project (for example set directly at the device), the text report says
  so and the JSON has `"ip_assigned_elsewhere": true`.
- **PROFINET device names.** When a device has "generate PROFINET device name
  automatically" switched on, TIA Portal derives the name from the device
  name and does not keep the stored copy up to date (a PLC renamed to
  `ZZALPHA` still stores `plc_1`). The report then says `PROFINET name
  automatic`; the stale copy is in the JSON as `profinet_name_stored`.
- `ip_set_by_user` in the JSON is a flag from the project. In V21 it is false
  until someone types an address; a V13 project has it false on an address
  that is clearly not the default, so treat it as a hint.
- `configured_mac` in the JSON is the MAC address stored in the project. In all
  projects seen so far it is a placeholder (`08-00-06-01-00-00`), not the MAC
  of the real device.
- An S7-1200 rack has the literal order number `Rack`; that is what the
  project stores.

### Security settings

Under every CPU the report lists what the project says about access to it:

    Security settings:
      Access level: HMI access
      PUT/GET access: permitted
      Web server: activated, HTTPS only, access enabled on X1
      Time synchronisation: NTP, server 192.168.77.50
      Display protection: on
      Not set in the project (TIA Portal default applies): OPC UA server

- **A project stores a setting only once someone has changed it.** What was
  never touched is listed in the last line as "not set", and is `null` in the
  JSON. tiaconv does not turn that into "off", because the default depends on
  the CPU and its firmware. Examples: on an S7-1500 PUT/GET access is off
  unless ticked, but an S7-1200 before firmware V4 and an S7-300/400 have no
  such option and always answer PUT/GET. A current S7-1500 allows only secure
  PG/PC and HMI communication and has access control on unless told
  otherwise; an older one has neither feature.
- **Access level.** The project stores a number, and the same number means
  different things on different CPUs. The name is printed for the
  combinations that were checked against TIA Portal (S7-1500; S7-1200 with
  firmware before V4); for anything else the report says `level 2`.
  Level 2 and above means a password is needed for the access above that
  level.
- **Passwords are never printed**, in any output: not the stored protected
  form, not its length. tiaconv also does not say whether an individual
  optional password is set; the file gives no reliable sign of that short of
  working on the password protection itself, which it does not do.
- **Web server.** It is reachable only if it is activated on the CPU *and*
  access is enabled on an interface; both are shown.
- These are the settings in the project. What is loaded in the CPU can
  differ.
- In the hardware CSV the settings are extra columns at the end of the CPU's
  rows.

### CSV files

The CSV files are UTF-8 and start with a byte-order mark, so that Excel shows
names and comments with accented or non-Latin characters correctly when the
file is opened by double-click. Most tools skip the mark. In Python, open the
file with `encoding="utf-8-sig"`, or write it with `--no-bom`. CSV written to
standard output (`-`) never has the mark.

Excel reads the `offset` column as numbers and shows `262.0` as `262`, which
drops the bit position. Import that column as text, or use `offset_bits` from
the JSON.

### Tags and data blocks

- **Tags** are the PLC tags of the tag tables. System constants (hardware
  identifiers) and user constants are not listed, so a table that TIA Portal
  shows as `Default tag table [49]` can have 4 tags here: the other 45 are
  system constants.
- **Comments** are shown after `//`. When a comment exists in several
  languages, one is shown: the project's default text if it has one,
  otherwise the first language stored. `--objects` has all of them. Members
  of library blocks carry Siemens' own comments. Comments are not read from
  V13 projects.
- **Access** is `standard` or `optimized`. Only blocks with standard access
  have fixed addresses, so only those get an offset: `0.3` is byte 0 bit 3,
  `24` is byte 24. A member at offset `2` of DB3 with type `Int` is
  `DB3.DBW2`. Offsets come from the project, they are not computed.
- **Start values** (`:= true`) are the values written in the project. A
  member without one starts at the default of its type. For a member of a
  PLC data type or a function block instance, the default of that type is
  shown unless the block overrides it. These are start values, not the
  values in a running PLC.
- **Instance blocks** show the interface of their function block with the
  section of each member (`[Input]`, `[Output]`, `[InOut]`, `[Static]`).
  Temporary variables and constants are not part of a data block and are
  left out.
- `(type not followed)` marks a member whose type definition is not stored
  with the block. That is normal for some system data types (`TCON_Param`
  passed as in/out parameter, for instance).
- Arrays are listed as one member with their declared type
  (`Array[0..9] of Byte`); elements are not expanded.
- Copies of tags and blocks that the project keeps for library types are not
  listed.
- A data type that the project does not name shows as `type#N`.

## Building

Needs CMake 3.16+ and a C++17 compiler.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release

Add `-DTIACONV_STATIC=ON` for a statically linked executable.

## What is verified

| | |
|---|---|
| File structure | Every byte of the fourteen project files is accounted for by the block list; in V15.1, V16, V19 and V21 all block hashes match. |
| Object decoding | All 22 621 objects decode without an out-of-range read. |
| Device name, IP address, mask, router, PROFINET name, subnet | Read back exactly as entered in the V21 test projects, each at the step where it was entered. |
| Order number, type, firmware | For the V16 sample all three match what its author documented; for the V19 sample the CPU type does. In the V21 test projects all three were confirmed by the person who configured them. |
| Which attributes are stored | The rule reproduces, for all 2 629 object types listed there, the resolved layout table that the V13 sample carries; and with it every plain attribute segment of all samples is covered exactly, byte for byte, with two exceptions (one audit-trail object in each of two projects). `tools/check_storage_rule.py` repeats both checks. |
| CPU security settings | V21: one test project was saved 23 times with one setting changed each time, on three CPUs (S7-1500 firmware V1.8 and V4.1, S7-1200 firmware V2.2). tiaconv is run on the project as it was after each save and shows exactly that change: every access level, PUT/GET, web server on the module and on the interface, HTTPS only, NTP with its server, display protection, OPC UA server, legacy communication, protection of configuration data, access control. For the newest CPU the result also matches the overview page of TIA Portal's security wizard. **Not checked:** S7-1200 from firmware V4, S7-300/400, ET 200SP CPUs, software controllers, HMI devices. |
| Tags | V21: the eight tags of the test project's tag table are identical to TIA Portal's own export of that table (name, data type, address, comment), and the number of tags per table matches the project tree. V13 sample: the four system-memory tags have the addresses TIA Portal assigns to them (`%MB1`, `%M1.1` .. `%M1.3`). |
| Data block members, types, sections, start values, comments | V21: the instance block of a function block is identical to the source TIA Portal generated for it (four members: name, type, section, start value, comment); two global blocks with a PLC data type inside are identical, row by row, to what TIA Portal's block editor shows (name, type, start value, including the defaults of the data type); block numbers and kinds match the project tree. V19 sample: all 150 members of the five instance blocks of the author's own function blocks, nested instances and structures included, are identical to the declarations in the SCL sources published next to the project, 93 comments among them. V16 sample: the local port in the connection block (502) is the port the example's client program connects to; interface 64 and connection type `16#0B` are the usual values for a TCP connection of an S7-1200. V13: parameters and defaults of Siemens library blocks are as documented (`MB_SERVER.IP_PORT := 502`). |
| Offsets | V21: all twelve offsets of a standard block (Bool, Byte, Int, DInt, Real, Date, String, arrays, a nested PLC data type) are the ones in the Offset column of TIA Portal's block editor. In the ten blocks of the V13 and V15.1 samples that have offsets they are consistent with the sizes of the data types. Offsets in instance blocks with standard access: V13 sample only, **not compared with TIA Portal**. |
| `standard` / `optimized` | V21: of two global blocks made with the same members, the one with "optimized block access" switched off is reported as standard, the other as optimized; a block whose generated source says `S7_Optimized_Access := 'TRUE'` is reported as optimized. |
| Anonymous structures, user constants | Structures are checked on the V19 sample only; user constants are not read. |
| V11, V12, V14, V17, V18, V20 projects | **Not tested.** |
| Large projects split over several data files | **Not supported**; a warning is printed. |
| Protected projects, know-how-protected blocks | **Not tested.** tiaconv does not try to remove any protection. |

`tests/tests.cpp` builds small synthetic projects in memory, including tags
and data blocks with nested types, sections and start values.
`tests/fixtures_test.cpp` reads the V21 test projects in `tests/fixtures`:
eight that differ from each other by one known change each, one with a small
program whose tag table and block source were exported from TIA Portal for
comparison, and one in which a security setting was changed before each of
23 saves; that one is read as it was after every save. Corrupted and truncated
files are rejected or read partially with a warning; they must never crash the
tool.

## How it works

A project keeps all its data in `System/PEData.plf`: an append-only list of
objects, plus an XML description of every object type. `tiaconv` reads that
description first and uses it to decode the objects, so it does not rely on
fixed offsets per TIA version. Details, and what is still unknown, are in
[docs/FORMAT.md](docs/FORMAT.md).

`tools/` holds the Python scripts used to explore the format.

## Credits and licence

The newer file layout was first described publicly by Nils Emmerich (ERNW) in
["TIA Project Parser"](https://insinuator.net/2026/08/tia-project-parser/);
see [docs/FORMAT.md](docs/FORMAT.md) for what builds on that work.

`tiaconv` is free software under the GNU General Public License, version 3 or
later; see [LICENSE](LICENSE). It bundles [miniz](https://github.com/richgel999/miniz)
(MIT licence, `third_party/miniz`).

This project is not affiliated with or endorsed by Siemens. SIMATIC, STEP 7 and
TIA Portal are trademarks of Siemens AG.
