# tiaconv

Reads a Siemens TIA Portal project **without TIA Portal** and lists what is in
it:

- stations, CPUs and modules with order numbers and firmware versions,
- network interfaces with their IP settings and PROFINET names, and subnets,
- PROFINET IO systems (which IO device belongs to which controller), the
  cabling between ports, and the configured S7 and HMI connections,
- the security settings of each CPU: access level, PUT/GET, web server,
  OPC UA server, NTP, display protection, access control,
- the blocks of each PLC (OB, FB, FC, DB, PLC data types) with number,
  language, folder, know-how, write and copy protection, whether they are
  compiled, and when they were last changed and downloaded,
- hardware identifiers with the module, interface or port each one stands
  for, and user constants with data type, value and comment,
- PLC tags with data type, address and comment,
- the tags of HMI devices (panels): which PLC tag or data block member each
  one stands for, over which connection, with data type and update cycle,
- data blocks with their members, data types, start values, comments and,
  for blocks with standard access, absolute offsets,
- the save history, as far as the file still holds it: what was added,
  removed or changed with each save, when, and by which user.

It is meant for asset inventories and security assessments (NIS2, IEC 62443),
where you are handed a project folder or archive and need the hardware list
and the data that is reachable over the network, without a licensed
engineering station.

- single executable, no installation, no dependencies
- Windows, Linux, macOS
- read-only: the project is never modified
- output as text, JSON or CSV

> **Status: early.** Checked against four public projects (V13, V15.1, V16,
> V19) and fifteen V21 test project files made for this repository. In those, every
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
| `--hmi-tags-csv FILE` | the tags of the HMI devices as CSV, one row per tag |
| `--constants-csv FILE` | constants as CSV, one row each: hardware identifiers, user constants and the other system constants |
| `--block-list-csv FILE` | the list of blocks as CSV, one row per block |
| `--blocks-csv FILE` | data block members as CSV, one row per member, nested members as `outer.inner` |
| `--no-bom` | write the CSV files without the UTF-8 byte-order mark (see below) |
| `--members` | print the members of every data block in the text report (the JSON always has them) |
| `--objects FILE` | every decoded object as JSON lines: attributes and relations. For research and for comparing two versions of a project |
| `--meta FILE` | the type model embedded in the project (XML) |
| `--all-devices` | also list device objects outside the project tree (copies kept for library types) |
| `--all-items` | list every device item, including ports and internal items |
| `--verify` | check the SHA-256 hash of every block (V14 and later) |
| `--save N` | show the project as it was after its N-th save (see below) |
| `--history` | add the save history to the text report and the JSON: what changed with each save (see below) |
| `--history-csv FILE` | the save history as CSV, one row per change |
| `-q`, `--quiet` | no text report |

Exit status: 0 on success, 1 for a usage error, 2 when the project cannot be
read.

### Reading the output

- **Devices** are the stations of the project tree: PLC stations and
  distributed IO stations (the ones TIA Portal lists under "Ungrouped
  devices" or in a device group). Up to version 0.5.0 distributed IO stations
  were only listed with `--all-devices`.
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

### Earlier saves

A save normally appends to the project file: it adds the objects that changed
and leaves the old ones in place. The last line of the report says how many
saves the file records, and `--save N` shows the project as it was after the
N-th one, in every output format. A device or connection that was deleted
later is there again; so is a setting before it was changed.

A file starts with a save that holds nothing but the type model; in a new
project, save 2 is the empty project and save 3 the first thing done in it.

How far back this goes differs. "Save as" keeps the history. TIA Portal now
and then writes the whole file anew by itself, dropping everything old: in a
test this happened within a few saves in which blocks were compiled and
protected. Such a file has the whole project in its second save. The one
project archive (`.zap19`) among the samples and one other public sample
project are different again: one save marker after the type model, and the
whole project written after it without a marker of its own. The last line of the report then reads
`1 save recorded and 3107 objects written after the last`, and `--save 1`
shows an empty project.

### Save history

`--history` reads the project as it was after every save and reports the
differences from one to the next:

    Save history:
      Recorded by TIA Portal:
        2026-10-05T13:00:10.135Z  Project created with TIA Portal V21
      Save 1  no time  -  0 objects written
          No project in the file yet
      Save 2  2026-10-05T13:00:10.683Z  by PC  -  344 objects written
          First state of the project in the file: an empty project
      Save 3  2026-10-05T13:02:12.246Z  by PC  -  202 objects written
          + device S7-1200 station_1  (S71200.Device)
              module S7-1200 station_1 / Rack_0  (S7-1200 Rack, Rack, V1.0)
              module S7-1200 station_1 / PLC_1  (CPU 1212C AC/DC/Rly, 6ES7 212-1BD30-0XB0, V2.2)
              interface S7-1200 station_1 / PLC_1 / X1 : PN(LAN)  (IP 192.168.0.1)
              with 1 block, 13 hardware identifiers, 1 system constant
      Save 5  2026-10-05T13:03:42.678Z  by PC  -  9 objects written
          ~ module S7-1200 station_1 / ZZALPHA: name: PLC_1 -> ZZALPHA
      Save 7  2026-10-05T13:04:40.541Z  by PC  -  11 objects written
          + subnet PN/IE_1
          ~ interface S7-1200 station_1 / ZZALPHA / X1 : PN(LAN): IP address: 192.168.0.1 -> 192.168.77.11; subnet: (not set) -> PN/IE_1
      ...
      Save 32  2026-10-05T21:09:17.464Z  by PC  -  3 objects written
          ~ module S7-1500/ET200MP station_1 / ZZBRAVO: access level: HMI access -> No access (complete protection); PUT/GET access: yes -> no
      Save 64  2026-10-06T13:06:09.906Z  by PC  -  9 objects written, 2 of them as deleted
          - connection S7-1500/ET200MP station_1 / S7_Connection_1  (S7 to S7-1500/ET200MP station_2 / ZZCHARLIE / X1)
      Save 67  2026-10-06T13:16:26.004Z  by PC  -  3 objects written
          No change in what is reported; written: 1 CorePersistenceInfo, 1 DeviceItemData, 1 ProjectData
      Save 78  2026-10-06T20:42:21.305Z  by PC  -  7 objects written
          ~ block ZZBRAVO / ZZFC [FB77]: title: (not set) -> ZZTITLE; comment: (not set) -> ZZCOMMENT; author: (not set) -> ZZAUTH; ...

`+` is added, `-` removed, `~` changed.

- **What is compared** is what tiaconv reports: devices, modules, interfaces
  and their addresses, the security settings of a CPU, subnets, IO systems,
  port connections, connections, blocks and their properties, data block
  members, tags, HMI tags, constants, and the name of the project. A change in
  anything else does not appear as a change. The line then reads `No change
  in what is reported` and names the kinds of object the save wrote, so that
  it is at least visible that something was saved. Save 67 above is a
  password typed into the access level table: tiaconv reports no passwords,
  and so nothing about them changing either. The same goes for users and
  roles.
- **Program code is not read.** That the code of a block changed shows as
  `code changed` (TIA Portal's own time stamp of the block moved), likewise
  `interface changed`, `compiled` and `downloaded`; what changed in the code
  does not show. A block that was modified in a way none of this covers is
  listed as `modified, in something that is not reported`.
- **The first state** of the project in the file is counted, not listed: it
  is not a change. When the file starts with a project that already has
  contents, a note says that the history before it is not in the file.
- **Time.** In the older file layout (V13) every save carries its own time.
  In the newer one (V14 and later) it does not. The time shown is then the
  latest "modified" time among the objects the save wrote: the save was made
  then or later, and before the time of the next one. How much later is not
  in the file. For the last save of one test project it could be compared
  with the time the file was written on disk: 31 ms. A save without any
  change has no time.
- **By** is the user name TIA Portal stored as "last modified by" with the
  project, or with the object changed last when the save did not write the
  project. In the test projects that is the Windows user name; a few small
  saves name nobody.
- **`Recorded by TIA Portal`** is the list TIA Portal keeps itself in the
  project (project created, converted from an older version), as stored.
- **Objects written** counts the objects the save wrote to the file;
  `as deleted` are those it removed. It says how much a save touched, also
  where tiaconv reports no change.
- When a station is added or removed, its modules and interfaces are listed
  under it and the rest (blocks, hardware identifiers, tags) is counted.
  The JSON and the CSV have one entry for each, marked with the item it is
  part of.
- When many items change in the same way in one save (twelve blocks
  downloaded), they share a line, and from the thirteenth added or removed
  item of a kind on, the text report only counts. The JSON and the CSV have
  everything.
- A data block member has no identity of its own in what tiaconv reads: a
  renamed member shows as one removed and one added. An instance data block
  that TIA Portal generated anew shows as `created anew`, with the members
  that changed.
- A change that only follows from a rename (what a hardware identifier
  stands for, after its PLC was renamed) is not listed again.
- What a file holds after its last save marker (see above) is listed as
  `After save N`.

With `--save N` the history stops at save N. The file is read once per save,
so a history takes about as long as a normal run times the number of saves:
two seconds for the 80 saves of the largest test project. A large project
with many saves has not been timed; expect minutes. Progress goes to standard
error.

`--history-csv FILE` has one row per change with the columns `save`, `time`,
`by`, `objects_written`, `objects_deleted`, `change` (`added`,
`removed`, `changed`; or `first_state`, `none`, `no_project`, `not_read` for
a save without a row of changes), `kind`, `item`, `part_of`, `attribute`,
`from`, `to`, `description`. In the JSON the history is under `history`, with
`events`, `saves` and per save `time`, `time_source` (`save` or
`latest_change`), `by`, `objects_written`, `object_types`
and `changes`. TIA Portal's own list is always in the JSON, as
`project.events`.

A history is evidence of what the file holds, not an audit trail: TIA Portal
drops it when it writes the file anew, anyone who can write the file can
change it, and the time and the user name are whatever the engineering
station said they were.

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
- **Users and roles.** A current CPU has user management (seen on S7-1500
  with firmware V3.1 and V4.1 and on S7-1200 with firmware V4.7; tiaconv
  recognises it by the access levels being among the user rights the project
  keeps for the CPU). There, who may do what is decided by users and roles.
  The report says:

      Access control: enabled (TIA Portal default), by users and roles
      Access without login: Read access
      Users and roles: not read, the project stores their names and passwords in protected form

  - *Access without login* is what anyone can do without a user name: the
    rights of the "Anonymous" user. TIA Portal derives an access level from
    those rights, shows it greyed out on the CPU's access control page, and
    stores it; that stored level is what tiaconv prints.
  - "... and by access levels with passwords" means "Use access control via
    access levels" is ticked: the older passwords per level are accepted in
    addition.
  - `Access control: disabled, the CPU has no access protection`: whatever
    level is stored is not in force.
  - The users themselves, their roles and what each role may do are **not
    read**. To see who can log in with which rights, open "Security settings
    > Users and roles" in TIA Portal.
  - In the JSON and the CSV this is `access_protection`: `access_levels`,
    `users_and_roles`, `users_and_roles_and_access_levels` or `none`.
- **Access level.** The project stores a number, and the same number means
  different things on different CPUs. The name is printed for the
  two families that were checked against TIA Portal (S7-1500 with firmware
  V1.8 and V4.1; S7-1200 with firmware V2.2 and V4.7, whose levels changed
  with firmware V4); other firmware versions of those families get the same
  names. For other CPU families the report says `level 2`. On a CPU without
  user management, level 2 and above means a password is needed for the
  access above that level.
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

### IO systems, port connections, connections

This is `tiaconv --save 63 tests/fixtures/s10_connections`:

    IO systems:
      PROFINET IO-System (100)  controller ZZBRAVO  subnet PN/IE_1
          ZZIO1  IO device_1  192.168.77.1
          ZZI02  IO device_2  192.168.77.2
      PROFINET IO-System (100)  controller ZZCHARLIE  subnet PN/IE_1
          no devices assigned

    Port connections:
      S7-1500/ET200MP station_1 / ZZBRAVO / X1 P1  <->  ZZIO1 / IO device_1 / X1 P1R
      ZZIO1 / IO device_1 / X1 P2R                 <->  ZZI02 / IO device_2 / X1 P1R

    Connections:
      Name             Type  From                                                      To                                                          Notes
      S7_Connection_1  S7    S7-1500/ET200MP station_1 / ZZBRAVO / X1 (192.168.77.12)  S7-1500/ET200MP station_2 / ZZCHARLIE / X1 (192.168.77.13)  local ID 100 (hex), partner ID 100 (hex), S7 over TCP/IP, two-way, configured on both sides
      S7_Connection_2  S7    S7-1500/ET200MP station_1 / ZZBRAVO / X1 (192.168.77.12)  partner outside the project, 192.168.77.99                  local ID 101 (hex), S7 over TCP/IP, one-way

- **IO systems.** An IO device is listed under the controller it is assigned
  to; the same is said under the device ("IO device of ZZBRAVO"). A station
  that is not assigned to a controller appears as a device only. An IO system
  whose devices were all moved elsewhere stays in the project, empty, and is
  listed that way. The number in brackets is the IO system number TIA Portal
  shows. Only PROFINET IO systems have been checked.
- **Port connections** are the cables drawn in TIA Portal's topology view,
  with the port names as the project stores them (`X1 P1`, `X1 P1R`). Many
  projects have none: the topology view is optional, and a project without
  it says nothing about how the devices are really cabled.
- **Connections** are the configured connections of the network view: S7
  connections and HMI connections.
  - *From* is the end that sets the connection up, where the project says so.
  - A connection between two PLCs of the same project is stored once on each
    PLC; it is listed once, "configured on both sides".
  - "Partner outside the project" is a connection to an unspecified partner:
    the project only has the address that was typed in.
  - Local and partner ID are printed in hexadecimal, as in TIA Portal's
    connection table; the JSON has both forms.
  - "One-way" means only this end can use the connection for its own
    requests (the partner just answers); "two-way" that both can.
- **Not listed:** connections that a program opens itself (`TCON`, `TSEND_C`,
  Modbus and the like; their parameters are in data blocks, see below),
  OPC UA, and anything a device accepts without a configured connection, such
  as PUT/GET access (see Security settings).
- In the JSON these are `io_systems`, `port_links` and `connections`. The
  hardware CSV has the columns `io_controller` and `io_system` on the row of
  an IO device's interface module.

### CSV files

The CSV files are UTF-8 and start with a byte-order mark, so that Excel shows
names and comments with accented or non-Latin characters correctly when the
file is opened by double-click. Most tools skip the mark. In Python, open the
file with `encoding="utf-8-sig"`, or write it with `--no-bom`. CSV written to
standard output (`-`) never has the mark.

Excel reads the `offset` column as numbers and shows `262.0` as `262`, which
drops the bit position. Import that column as text, or use `offset_bits` from
the JSON.

### Blocks

This is `tiaconv --save 3 tests/fixtures/s11_blocks`, the rows of one PLC:

    Blocks:
      PLC        Block  Name              Kind                 Language  Protection                 Folder                  Compiled  Modified          Downloaded
      ZZBRAVO    OB1    Main              ProgramCycle         LAD       -                          Program blocks          yes       2026-10-06 20:48  -
      ZZBRAVO    OB30   ZZCYCLIC          CyclicInterrupt      LAD       -                          Program blocks          yes       2026-10-06 20:36  -
      ZZBRAVO    FB1    Block_1           -                    FBD       know-how                   Program blocks          yes       2026-10-06 20:49  -
      ZZBRAVO    FB2    Block_2           -                    STL       bound to CPU               Program blocks          no        2026-10-06 20:52  -
      ZZBRAVO    FB77   ZZFC              -                    SCL       write                      Program blocks/Group_1  yes       2026-10-06 20:50  -
      ZZBRAVO    DB1    DB_Standard       global               -         write-protected in device  Program blocks          yes       2026-10-06 20:47  -
      ZZBRAVO    DB2    DB_Optimized      global               -         -                          Program blocks          yes       2026-10-05 18:21  -
      ZZBRAVO    DB3    Block_1_DB        instance of Block_1  -         -                          Program blocks          yes       2026-10-05 18:32  -
      ZZBRAVO    DB4    Block_2_DB        instance of Block_2  -         -                          Program blocks          yes       2026-10-05 19:01  -
      ZZBRAVO    UDT1   User_data_type_1  -                    -         -                          PLC data types          yes       2026-10-05 17:57  -
      Times are UTC.

- **What is listed.** Organization blocks, function blocks, functions, data
  blocks and PLC data types, for every PLC, including the blocks TIA Portal
  keeps under "System blocks" because the program uses them (`MB_SERVER`,
  `PID_Compact`, ...): those are loaded into the CPU like any other. System
  functions and system data types (SFB, SFC, SDT) are counted in a line below
  the table and listed in the JSON and the block list CSV only.
- **Kind** is the event an OB handles (`ProgramCycle`, `CyclicInterrupt`,
  `Startup`, ...) and, for a data block, `global` or the block it is an
  instance of.
- **Protection** as the project states it:
  - `know-how`: the block is know-how protected with a password.
  - `write`: the block is write-protected with a password.
  - `system`: a protected block from a Siemens library.
  - `bound to CPU` / `bound to memory card`: copy protection. The serial
    number, when it was entered in the project, is in the JSON and CSV.
  - `write-protected in device`: a data block the program cannot write to.

  tiaconv reads these settings and nothing behind them: no password, and not
  the code of any block, protected or not. The members of an instance data
  block are listed even when its function block is protected: the project
  stores them unprotected.
- **Compiled** is `no` when the project says the block has to be compiled
  again. TIA Portal sets that when the block is changed (not for a change of
  its title or comment alone) and clears it when the block is compiled. A
  block marked `no` is not what was last compiled, and so not what was last
  downloaded.
- **Modified** is the last time TIA Portal changed the block. That is not
  always an edit by a person: compiling a block can set it too.
- **Downloaded** is the last time the block was downloaded to a device *from
  this project file*, `-` if never. The JSON has the history the project
  keeps (up to 20 downloads per block). A CPU can have been loaded from
  another copy of the project since, so this says what the file knows, not
  what is in the CPU. Not compared with TIA Portal yet.
- **Folder** is the place in the project tree, with the folder names of the
  English user interface and the user's own group names.
- In the JSON the list is `blocks`; `--block-list-csv FILE` writes it as CSV
  (`--blocks-csv` is something else: the members of the data blocks).
- The JSON and the CSV also have: title, comment, author, family, version
  and user-defined ID, optimized access, creation time, time of the last
  compilation, load and work memory in bytes, and the number of networks.
  The memory sizes are the ones TIA Portal lists under Program info >
  Resources; like TIA Portal, tiaconv gives none for a block that has to be
  compiled again.
- For a data block they also say whether it is **reachable from OPC UA and
  from the web server**: `accessible_from_opc_ua` and
  `accessible_from_web_server` are `false` when the box in the block's
  attributes was unticked, and empty (`null`) when it was never touched,
  which on a new block means ticked. Whether anything can really get at the
  block that way also depends on the CPU: its firmware must have the
  feature and the server must be switched on (see Security settings).
- Times are UTC.

### Constants

This is `tiaconv --save 15 tests/fixtures/s12_constants`, the rows of one PLC:

    Hardware identifiers:
      PLC        ID   Name                                   Type          Stands for
      ZZBRAVO    32   Local~Device                           Hw_Device     S7-1500/ET200MP station_1 / ZZBRAVO
      ZZBRAVO    33   Local~Configuration                    Hw_SubModule  S7-1500/ET200MP station_1 / ZZBRAVO
      ZZBRAVO    49   Local                                  Hw_SubModule  S7-1500/ET200MP station_1 / ZZBRAVO
      ZZBRAVO    50   Local~Common                           Hw_SubModule  S7-1500/ET200MP station_1 / ZZBRAVO
      ZZBRAVO    51   Local~MC                               Hw_SubModule  S7-1500/ET200MP station_1 / Card reader/writer_1
      ZZBRAVO    52   Local~Exec                             Hw_SubModule  S7-1500/ET200MP station_1 / CPU exec unit_1
      ZZBRAVO    54   Local~Display                          Hw_SubModule  S7-1500/ET200MP station_1 / CPU display_1
      ZZBRAVO    64   Local~PROFINET_interface_1             Hw_Interface  S7-1500/ET200MP station_1 / PROFINET interface_1
      ZZBRAVO    65   Local~PROFINET_interface_1~Port_1      Hw_Interface  S7-1500/ET200MP station_1 / Port_1
      ZZBRAVO    66   Local~PROFINET_interface_1~Port_2      Hw_Interface  S7-1500/ET200MP station_1 / Port_2
      ZZBRAVO    257  Local~PROFINET_IO-System               Hw_IoSystem   S7-1500/ET200MP station_1 / IOController_PROFINET
      ZZBRAVO    258  IO_device_1~Head                       Hw_SubModule  ZZIO1 / _1
      ZZBRAVO    259  IO_device_1~PROFINET_interface         Hw_Interface  ZZIO1 / PROFINET interface
      ZZBRAVO    260  IO_device_1~PROFINET_interface~Port_1  Hw_Interface  ZZIO1 / Port_1
      ZZBRAVO    261  IO_device_1~PROFINET_interface~Port_2  Hw_Interface  ZZIO1 / Port_2
      ZZBRAVO    262  IO_device_1~IODevice                   Hw_Device     ZZIO1 / IO device_1
      ZZBRAVO    264  IO_device_1~Proxy                      Hw_SubModule  ZZIO1 / IO device_1
      ZZBRAVO    265  IO_device_2~IODevice                   Hw_Device     ZZI02 / IO device_2
      ZZBRAVO    267  IO_device_2~Proxy                      Hw_SubModule  ZZI02 / IO device_2
      ZZBRAVO    268  IO_device_2~Head                       Hw_SubModule  ZZI02 / _1
      ZZBRAVO    269  IO_device_2~PROFINET_interface         Hw_Interface  ZZI02 / PROFINET interface
      ZZBRAVO    270  IO_device_2~PROFINET_interface~Port_1  Hw_Interface  ZZI02 / Port_1
      ZZBRAVO    271  IO_device_2~PROFINET_interface~Port_2  Hw_Interface  ZZI02 / Port_2

    User constants:
      PLC      Table              Name          Type    Value    Comment
      ZZBRAVO  Default tag table  ZZCONST_INT   Int     42       My int constant
      ZZBRAVO  Default tag table  ZZCONST_REAL  Real    3.5
      ZZBRAVO  Tag MyTagTable     ZZCONST_TIME  Time    T#5s
      ZZBRAVO  Tag MyTagTable     ZZCONST_STR   String  'Hello'
      79 other system constants not listed (OB numbers, process image partitions); the JSON and the constants CSV have them.

- **Hardware identifiers** are the numbers TIA Portal gives to every module,
  interface, port and IO device of a PLC (its "system constants"). Programs
  and connection parameters refer to hardware by these numbers: a
  communication block with `InterfaceId := 64` uses the interface that has
  identifier 64 in this list. *Stands for* is the station and the item the
  number belongs to; the IO devices assigned to a PLC appear in that PLC's
  list.
- **User constants** are the constants entered in the "User constants" tab
  of a tag table, with the value as written in the project (`T#5s`,
  `'Hello'`).
- The other system constants (the number of each organization block, the
  process image partitions) are counted in the last line and listed in the
  JSON and the CSV only. In the JSON all constants are in `constants`, with
  `kind` `hardware`, `user`, `ob`, `pip` or `system`.

### Tags and data blocks

- **Tags** are the PLC tags of the tag tables. The number TIA Portal shows
  next to a table counts its constants too: `Default tag table [49]` can be
  4 tags and 45 system constants (see Constants).
- A data block is shown with its **title** after `//` (its comment if it has
  no title); the JSON has both. Up to version 0.6.0 the title was called
  `comment`.
- **Comments** of tags and members are shown after `//`. When a comment exists in several
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
  listed. Neither is what TIA Portal keeps in place of a tag that was
  deleted while something still names it (an HMI tag, for instance): an
  object with the name and nothing else. Up to version 0.9.0 that was listed
  as a tag without table, type and address.
- A data type that the project does not name shows as `type#N`.

### HMI tags

The tags of every HMI device in the project, as in the "HMI tags" table of
TIA Portal and in the order of its export (the order in which the tags were
created):

    HMI tags:
      HMI      Table              Name    Type              Connection        PLC        PLC tag                   Address      Cycle
      ZZPANEL  Default tag table  ZZINT   Int               (internal)        -          -                         -            1 s  // ZZCOMMENT
      ZZPANEL  Default tag table  ZZSTD   Int               HMI_Connection_1  ZZBRAVO    DB_Standard.my_int        %DB1.DBW264  500 ms
      ZZPANEL  Default tag table  ZZMEM   Word              HMI_Connection_1  ZZBRAVO    MyInt                     -            1 s  (link to the PLC tag broken)
      ZZPANEL  Default tag table  ZZABS   Word              HMI_Connection_1  ZZBRAVO    -                         %MW100       1 s
      ZZPANEL  ZZTABLE            ZZUDT   User_data_type_1  HMI_Connection_1  ZZBRAVO    DB_Standard.my_data_type  %DB1.DBX0.0  1 s
      ZZPANEL  ZZTABLE            ZZCH    Word              HMI_Connection_2  ZZCHARLIE  -                         %MW200       1 s
      ZZPANEL  ZZTABLE            ZZOPT1  Real              HMI_Connection_1  ZZBRAVO    DB_Optimized.my_real      -            1 s

An HMI tag is what a panel can read from a PLC and, unless the screen that
uses it is read-only, write to it. The list says for each tag where in the
PLC that is.

- **Connection** is the HMI connection the tag uses and **PLC** the
  controller at its other end, taken from the connections of the project
  (see above). `(internal)` is a tag without a connection: the panel keeps
  the value itself.
- **PLC tag** is the PLC tag or data block member the HMI tag stands for,
  written as in TIA Portal: `Motor_DB.Speed`, `Tag_1`. TIA Portal calls this
  symbolic access.
- **Address** is, for a tag that stands for a PLC tag, the address of that
  PLC tag where it has one: a tag in I, Q or M, or a member of a data block
  with standard access. TIA Portal's own tag table leaves the address of
  such a tag empty; tiaconv shows it because it says what the panel touches
  in the PLC. A member of an optimized block has no address. The HMI tag
  stores an address even then, but a stale one (`%DB1.DBD0` for a Bool); it
  is in the JSON and the CSV as `address_stored` and should not be relied
  on. tiaconv decides which case it is by looking the PLC tag up in the tags
  and data blocks of that PLC.
- A tag with a connection and an address but no PLC tag has **absolute
  access**: it reads and writes whatever is at that address. The JSON and the CSV have
  the kind of access as `access` (`symbolic`, `absolute`, `internal`).
- **Cycle** is the acquisition cycle. The acquisition mode is in the JSON
  and the CSV under the name TIA Portal's tag editor uses: `Cyclic in
  operation` (the default) and `Cyclic continuous`. TIA Portal's export
  writes the second one as `Continuous`, which is also how it is stored
  (`acquisition_mode_stored` in the JSON). Other modes are given as stored.
- Notes at the end of a line: `link to the PLC tag broken` when TIA Portal
  has marked it so (it shows the PLC tag on a red background; deleting the
  PLC tag does that), `PLC tag not found in the PLC's tags and data blocks`
  when the name leads nowhere in what tiaconv reads of that PLC.
- A tag of a structured type (a PLC data type) has its members in the JSON;
  the CSV has their number.
- The tags an HMI device creates itself (`@CurrentUser`,
  `@DiagnosticsIndicatorTag`) are not listed, and neither are screens,
  alarms, scripts or recipes. Which screen uses a tag is not read.
- Passwords are not reported: the settings of an HMI connection include an
  entry named `Password`, and tiaconv does not write its value anywhere.
- To compare with TIA Portal: HMI tags → "Show all tags" → Export gives an
  `.xlsx` with the same rows in the same order as `--hmi-tags-csv`. Its
  `Address` column is empty for symbolic access.

## Building

Needs CMake 3.16+ and a C++17 compiler.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release

Add `-DTIACONV_STATIC=ON` for a statically linked executable.

## What is verified

| | |
|---|---|
| File structure | Every byte of the nineteen project files is accounted for by the block list; in V15.1, V16, V19 and V21 all block hashes match. |
| Object decoding | All 30 324 objects decode without an out-of-range read. |
| Device name, IP address, mask, router, PROFINET name, subnet | Read back exactly as entered in the V21 test projects, each at the step where it was entered. |
| Order number, type, firmware | For the V16 sample all three match what its author documented; for the V19 sample the CPU type does. In the V21 test projects all three were confirmed by the person who configured them. |
| Which attributes are stored | The rule reproduces, for all 2 629 object types listed there, the resolved layout table that the V13 sample carries; and with it every plain attribute segment of all samples is covered exactly, byte for byte, with two exceptions (one audit-trail object in each of two projects). `tools/check_storage_rule.py` repeats both checks. |
| CPU security settings | V21: one test project was saved 23 times with one setting changed each time, on three CPUs (S7-1500 firmware V1.8 and V4.1, S7-1200 firmware V2.2). On a fourth (S7-1200 firmware V4.7) the rights of the Anonymous user were changed in four more saves, and the level tiaconv prints is the one TIA Portal showed after each. tiaconv is run on the project as it was after each save and shows exactly that change: every access level, PUT/GET, web server on the module and on the interface, HTTPS only, NTP with its server, display protection, OPC UA server, legacy communication, protection of configuration data, access control. For the newest CPU the result also matches the overview page of TIA Portal's security wizard. **Not read:** users and roles, which decide about access on a CPU with user management. **Not checked:** that the level follows the Anonymous user on an S7-1500 as well (there only the state of a new CPU was compared), S7-1200 with firmware V4.0 to V4.6, S7-300/400, ET 200SP CPUs, software controllers, HMI devices. |
| IO systems | V21: in a test project saved once per action, an ET 200SP station appears as a device when added, under the controller's IO system when assigned (with the IO system name and number TIA Portal shows), under the other controller after being reassigned; the IO system it left is reported empty. **Not checked:** PROFIBUS, I-devices, shared devices. |
| Port connections | V21: two cables drawn in the topology view appear, one per save, between the ports they were drawn between. |
| S7 connections | V21: a connection between two PLCs of the project and one to an unspecified partner appear in the save in which they were made, with the end points, partner address and local and partner IDs of TIA Portal's connection table; a deleted connection disappears. **Not checked:** other connection types configured in the network view, connections over PROFIBUS or MPI. |
| HMI connections | V21: two HMI connections from a KTP400 Basic panel, one to each of two PLCs, appear in the save in which each was made, with the addresses of both ends; the tags on them show the PLC that TIA Portal's tag table shows as "PLC name". V15.1 and V19 samples: one connection each. |
| Save history (`--history`) | V21: in the test projects 85 saves are documented with the one action taken in TIA Portal before each (hardware, addresses, 23 security settings, IO systems, cables, connections, blocks and their properties, constants, an HMI panel and its tags). For every one of them the history lists exactly that action and what TIA Portal changed along with it, and nothing else; the two saves that only changed a password or a role list no change. Adding up the first state and all additions and removals gives the final contents of each of the nine test projects checked that way. Time: every save that changed something has one, the times are in order, and that of the last save of one test project is 31 ms before the modification time of the file on the PC; no other save could be compared with a clock. V13 sample: the four saves carry their own times and two user names; what was done in each is not known, so the changes listed there are unchecked. TIA Portal's own list reads "created with V21" in the test projects; whether TIA Portal shows the same has not been compared. |
| Earlier saves (`--save`) | V21: test projects with 61 saves of one known action each; the report for save N shows exactly the actions up to N. One of them was rewritten by TIA Portal along the way and lost its history; the copy from before is kept as a fixture of its own. V13 sample: four saves are found and the modification date steps back accordingly, but what was done in each is not known. |
| Blocks | V21: a test project with one action per save: a new function block and a cyclic interrupt OB appear with type, number, language and kind; a block moved into a group shows the group; a number set by hand, title, comment, author, family, version and user-defined ID read back as entered, for a code block and a data block; know-how protection set and removed, write protection, copy protection with its serial number, a data block write-protected in the device and the two "accessible from" options each show in the save in which they were set; a change makes the block "not compiled", compiling makes every block "compiled". The block numbers match two screenshots of the project tree, and one block's protection page matches what is reported for it. V19 sample: title, comment, author, family and version of the author's five function blocks are those in the SCL sources published with the project. The time stamps of one block match its "Time stamps" page, the network counts of two OBs match the editor, and the load and work memory of all ten blocks of a PLC match Program info > Resources. **Not checked:** the download times (none of the test projects was ever downloaded; in the public samples they lie within the project's lifetime), fail-safe blocks, languages other than LAD, FBD, STL and SCL. |
| Tags | V21: the eight tags of the test project's tag table are identical to TIA Portal's own export of that table (name, data type, address, comment), and the number of tags per table matches the project tree. V13 sample: the four system-memory tags have the addresses TIA Portal assigns to them (`%MB1`, `%M1.1` .. `%M1.3`). |
| Data block members, types, sections, start values, comments | V21: the instance block of a function block is identical to the source TIA Portal generated for it (four members: name, type, section, start value, comment); two global blocks with a PLC data type inside are identical, row by row, to what TIA Portal's block editor shows (name, type, start value, including the defaults of the data type); block numbers and kinds match the project tree. V19 sample: all 150 members of the five instance blocks of the author's own function blocks, nested instances and structures included, are identical to the declarations in the SCL sources published next to the project, 93 comments among them. V16 sample: the local port in the connection block (502) is the port the example's client program connects to; interface 64 and connection type `16#0B` are the usual values for a TCP connection of an S7-1200. V13: parameters and defaults of Siemens library blocks are as documented (`MB_SERVER.IP_PORT := 502`). |
| Offsets | V21: all twelve offsets of a standard block (Bool, Byte, Int, DInt, Real, Date, String, arrays, a nested PLC data type) are the ones in the Offset column of TIA Portal's block editor. In the ten blocks of the V13 and V15.1 samples that have offsets they are consistent with the sizes of the data types. Offsets in instance blocks with standard access: V13 sample only, **not compared with TIA Portal**. |
| `standard` / `optimized` | V21: of two global blocks made with the same members, the one with "optimized block access" switched off is reported as standard, the other as optimized; a block whose generated source says `S7_Optimized_Access := 'TRUE'` is reported as optimized. |
| Constants | V21: four user constants entered one per save (Int, Real, Time, String, in two tag tables, one with a comment) read back with name, type, value, comment and table; a changed value and a deleted constant show in their save. The three constants left at the end are identical to TIA Portal's own export of them (name, tag table, data type, value, comment). What TIA Portal showed at the end (the three constants left, and the number of entries it gives for each tag table, which counts tags, user constants and system constants: 63 and 10) is what tiaconv reports. V16 sample: the connection block its author wrote refers to interface 64, and 64 is the hardware identifier of the CPU's PROFINET interface. All 58 system constants of that PLC (23 hardware identifiers, among them those of two IO devices, the OB constant and 34 process image partitions) have the name, data type and value shown in TIA Portal's "System constants" tab. What a hardware identifier stands for is not shown there; it agrees with the constant's name in every case. **Not checked:** PLCs with PROFIBUS, central modules of an S7-1500 or technology objects, which bring further kinds of identifiers. |
| HMI tags | V21: a test project with a KTP400 Basic panel, one action per save: an internal tag, tags on a data block member (standard and optimized access), on a PLC tag and on a PLC data type, two tags with absolute access on two connections to two PLCs, cycle, acquisition mode, comment and start value changed, a tag deleted, a PLC tag deleted under its HMI tag. Each appears in the save in which it was made. At the end all 7 tags agree with TIA Portal's own export of the tag table (name, tag table, connection, PLC tag, data type, access method, address, start value, comment, acquisition mode and cycle, in the export's order) and with screenshots of the tag table (PLC name per tag, the four members of the structured tag, the broken link shown in red). V19 sample: the 100 tags on a Comfort panel agree with TIA Portal V21's export of that project in every one of those columns, 100 of 100. On the PLC side, every tag with an intact link leads to a tag or member that tiaconv reads from the block interface, with the same data type; where the block has standard access the address stored with the HMI tag is the offset read there (V21 and V15.1). Not checked: limits and linear scaling (never changed from their defaults), multiplexed tags, acquisition mode "On demand", PC runtimes, WinCC Unified, HMI tags in V13 projects. |
| Anonymous structures | Checked on the V19 sample only. |
| V11, V12, V14, V17, V18, V20 projects | **Not tested.** |
| Large projects split over several data files | **Not supported**; a warning is printed. |
| Protected projects, know-how-protected blocks | **Not tested.** tiaconv does not try to remove any protection. |

`tests/tests.cpp` builds small synthetic projects in memory, including tags
and data blocks with nested types, sections and start values.
`tests/fixtures_test.cpp` reads the V21 test projects in `tests/fixtures`:
eight that differ from each other by one known change each, one with a small
program whose tag table and block source were exported from TIA Portal for
comparison, one in which a security setting was changed before each of 23 saves, one in
which the network was built up in the same way (stations, IO systems, cables,
connections), one in which blocks were added, protected and compiled, and
one with user constants;
those are read as they were after every save, and the save history is
compared with the list of actions in `tests/fixtures/README.md`. Corrupted and truncated
files are rejected or read partially with a warning; they must never crash the
tool.

## How it works

A project keeps all its data in `System/PEData.plf`: a list of
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
