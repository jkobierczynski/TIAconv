# Test projects

TIA Portal V21 projects made for this repository by Jurgen Kobierczynski.
`s00` to `s07` each differ from the previous one by a single change, so a
decoding error shows up at a known step. Only `System/PEData.plf` is kept.

| project | the one change |
|---|---|
| `s00_empty` | new empty project |
| `s01_cpu` | one PLC added: CPU 1212C AC/DC/Rly, 6ES7 212-1BD30-0XB0, firmware V2.2 |
| `s02_name` | PLC renamed to `ZZALPHA` |
| `s03_ip` | PROFINET address 192.168.77.11 / 255.255.255.0, new subnet `PN/IE_1` |
| `s04_router` | router 192.168.77.1 enabled |
| `s05_pnname` | automatic PROFINET device name switched off, name `zzalpha-pn` |
| `s06_module` | signal module SM 1221 DI8 x 24VDC, 6ES7 221-1BF30-0XB0, in slot 2 |
| `s07_second` | second PLC `ZZBRAVO`: CPU 1511-1 PN, 6ES7 511-1AK00-0AB0, address 192.168.77.12, not attached to the subnet (confirmed in TIA Portal). TIA Portal shows the PROFINET device name `zzbravo`; the file stores `plc_1` |

`s08_program` continues from `s07_second` and adds a program to `ZZBRAVO`:

- tag table `Tag MyTagTable` with eight tags, two of them with a comment,
  and four tags in the default tag table,
- PLC data type `User_data_type_1`,
- `DB_Standard` (DB1, optimized block access off) and `DB_Optimized` (DB2)
  with the same members,
- function blocks `Block_1` and `Block_2` with one variable per section, and
  their instance blocks DB3 and DB4, called in OB1.

`s08_program/exports` holds what TIA Portal itself exported from that
project: `PLCTags.xlsx` (the tag table) and `source_from_blocks.awl`
("Generate source from blocks" for `Block_2` and `Block_2_DB`). They are the
reference the test compares with. The test also holds what TIA Portal
displayed in three places, taken from screenshots: the project tree (block
numbers, number of tags per table), the calls of the two function blocks in
OB1, and the editors of `DB_Standard` and `DB_Optimized` with every member,
offset and start value.

`s09_security` continues from `s08_program` and is a different kind of
fixture: **one project, one security setting changed per save**. A project
file is append-only, so the state after every save is still in it, and the
test reads the project as it was after each one. Saves 1 to 28 are the
history of the earlier projects; the steps are:

| save | PLC | action in TIA Portal V21 |
|---|---|---|
| 29 | ZZBRAVO (CPU 1511-1 PN, FW V1.8) | "Permit access with PUT/GET communication" ticked |
| 30 | | access level Read access |
| 31 | | access level HMI access |
| 32 | | access level No access (complete protection); TIA Portal unticked PUT/GET with it |
| 33 | | back to Full access (no protection) |
| 34 | | HMI access again, with passwords of ten characters; PUT/GET ticked again |
| 35 | | PROFINET interface [X1] > Web server access: "Enable Web server via IP address of this interface" ticked |
| 36 | | ... unticked |
| 37 | | Web server > "Activate web server on this module"; TIA Portal ticked the interface box with it |
| 38, 39 | | interface box unticked, ticked |
| 40 | | "Permit access only with HTTPS" ticked |
| 41 | | time synchronisation via NTP, server 192.168.77.50 |
| 42 | | display protection enabled |
| 43 | ZZALPHA (CPU 1212C, FW V2.2) | protection: Write protection |
| 44 | | web server activated |
| 45 | | protection: Write/read protection |
| 46 | ZZCHARLIE (CPU 1511-1 PN 6ES7 511-1AL03-0AB0, FW V4.1) | added; security wizard left at its defaults. Its overview page: protection of confidential PLC data enabled, only secure PG/PC communication, access protection enabled with local user management, anonymous access disabled, legacy access protection "No access (complete protection)" |
| 47 | | OPC UA server activated |
| 48 | | "Only allow secure PG/PC and HMI communication" unticked |
| 49 | | "Protect confidential PLC configuration data" unticked |
| 50 | | access control disabled |
| 51 | | access control enabled, "Use access control via access levels" ticked |

`s10_connections` continues from `s09_security` in the same way, for the
network: one action per save. Any step can be looked at with
`tiaconv --save N tests/fixtures/s10_connections`.

| save | action in TIA Portal V21 |
|---|---|
| 52 | saved without a change |
| 53 | "Save as" `s10_connections` |
| 54 | ZZBRAVO attached to subnet `PN/IE_1` |
| 55 | ZZCHARLIE: address 192.168.77.13, attached to `PN/IE_1` |
| 56 | ET 200SP station added (IM 155-6 PN ST, 6ES7 155-6AU02-0BN0, firmware V6.4), named `ZZIO1`, not assigned to a controller |
| 57 | `ZZIO1` assigned to ZZBRAVO. TIA Portal created "PROFINET IO-System (100)" and gave the station the address 192.168.77.1 |
| 58 | second station of the same type added and assigned to ZZCHARLIE (address 192.168.77.2). It was named `ZZI02`, with a zero |
| 59 | second station reassigned to ZZBRAVO |
| 60 | topology view: ZZBRAVO port 1 connected to port 1 of the first station |
| 61 | topology view: port 2 of the first station connected to port 1 of the second |
| 62 | S7 connection `S7_Connection_1` between ZZBRAVO and ZZCHARLIE. TIA Portal's connection table lists it from both sides, local ID 100 and partner ID 100 (hex) |
| 63 | S7 connection `S7_Connection_2` from ZZBRAVO to an unspecified partner at 192.168.77.99, local ID 101 (hex) |
| 64 | `S7_Connection_1` deleted |
| 65 | fourth PLC added: CPU 1214C AC/DC/Rly, 6ES7 214-1BG40-0XB0, firmware V4.7, not attached to the subnet. Its access control page showed "Enable access control" selected, "Use access control via access levels" not ticked, and the access level table greyed out with "No access (complete protection)" selected |
| 66 | PLC renamed to `ZZDELTA`; "Use access control via access levels" ticked. The access level stayed greyed out: on this CPU it follows from the rights of the Anonymous user and cannot be chosen |
| 67 | a password entered in the access level table |
| 68 | Security settings > Users and roles: role `ZZROLE` created with the runtime right "Read access" on ZZDELTA. The access level shown on ZZDELTA did not move |
| 69 | `ZZROLE` assigned to the Anonymous user. The greyed-out access level of ZZDELTA now showed "Read access" |
| 70 | right of `ZZROLE` on ZZDELTA changed to "HMI access"; the access level showed "HMI access" |
| 71 | ... changed to "Full access"; the access level showed "Full access (no protection)" |

`s11_blocks_a` and `s11_blocks` continue from `s10_connections`, for the list
of blocks. They are **the same project at two moments**: TIA Portal rewrote
the project file during the test and dropped the history, so the copy taken
after save 80 is kept next to the final file. All actions are on ZZBRAVO
unless stated.

`s11_blocks_a` (80 saves):

| save | action in TIA Portal V21 |
|---|---|
| 72, 73 | saved without a change; "Save as" `s11_blocks` |
| 74 | new function block `ZZFC`, language SCL, number assigned by TIA Portal (FB3) |
| 75 | new organization block "Cyclic interrupt", `ZZCYCLIC`, LAD (OB30) |
| 76 | new group in "Program blocks" (it kept the name `Group_1`), `ZZFC` moved into it |
| 77 | `ZZFC`: number set by hand, 77 |
| 78 | `ZZFC` > Properties > Information: title `ZZTITLE`, comment `ZZCOMMENT`, author `ZZAUTH`, family `ZZFAM`, version `1.2`, user-defined ID `ZZID` |
| 79 | `DB_Standard`: title `ZZDBTITLE`, comment `ZZDBCOMMENT` |
| 80 | `DB_Standard` > Attributes: "Data block write-protected in the device" ticked |

Then, each followed by a save that is no longer in any file: a `;` typed as
the code of `ZZFC`; ZZBRAVO compiled (software, only changes); `Block_1`
know-how protected; `ZZFC` write-protected.

`s11_blocks` (9 saves; the file as TIA Portal rewrote it, then appended to):

| save | |
|---|---|
| 1 | nothing but the type model |
| 2 | the state after the four actions above. Screenshots taken then: the project tree (Main [OB1], ZZCYCLIC [OB30], Block_1 [FB1] with a lock, Block_2 [FB2], Block_1_DB [DB3], Block_2_DB [DB4], DB_Optimized [DB2], DB_Standard [DB1], Group_1 > ZZFC [FB77]) and the Protection page of `Block_1` ("The block is protected", write protection not defined, copy protection "No binding") |
| 3 | `Block_2` > Properties > Protection > Copy protection: bound to the serial number of the CPU, entered by hand: `S C-ZZ99887766` |
| 4 | know-how protection of `Block_1` removed (the password was changed first, in the same save) |
| 5 | `ZZCYCLIC` deleted |
| 6 | new global data block `ZZDB` (DB5). Screenshot of its Attributes page: "Only store in load memory" and "Data block write-protected in the device" unticked, "Optimized block access" ticked, the two "accessible" boxes ticked and greyed out (this CPU's firmware has no OPC UA) |
| 7 | on ZZCHARLIE: new global data block `ZZDB2` (DB1) |
| 8 | `ZZDB2` > Attributes: "Data block accessible from OPC UA" unticked |
| 9 | `ZZDB2` > Attributes: "Data block accessible via Web server" unticked |

`s12_constants` continues from `s11_blocks`, for user constants, all on
ZZBRAVO:

| save | action in TIA Portal V21 |
|---|---|
| 10, 11 | saved without a change; "Save as" `s12_constants` |
| 12 | Default tag table > User constants: `ZZCONST_INT`, Int, value 42, comment `My int constant` |
| 13 | same tab: `ZZCONST_REAL`, Real, 3.5 |
| 14 | `Tag MyTagTable` > User constants: `ZZCONST_TIME`, Time, `T#5s` |
| 15 | same tab: `ZZCONST_STR`, String, `'Hello'` |
| 16 | value of `ZZCONST_INT` changed to 43 |
| 17 | `ZZCONST_REAL` deleted |

`s12_constants/exports/PLCTags.xlsx` is TIA Portal's own export of ZZBRAVO's
tags after save 17; its sheet "Constants" lists the three user constants
left, with path (the tag table), data type, value and comment. The test
compares with it.

Two screenshots of the "System constants" tab of ZZBRAVO's default tag table
show all 58 system constants with name, data type and value: 34 process
image constants (None 65535, Automatic update 0, PIP 1 to PIP 31, PIP OB
Servo 32768), `OB_Main` (OB_PCYCLE, 1) and 23 hardware identifiers, from
`Local~Device` (Hw_Device, 32) to the second port of the second IO device
(Hw_Interface, 271). The test holds every row.

A screenshot taken at the end shows the two "User constants" tabs with the
three constants left (`ZZCONST_INT` Int 43 "My int constant"; `ZZCONST_TIME`
Time T#5s; `ZZCONST_STR` String 'Hello') and, in the project tree, "Default
tag table [63]" and "Tag MyTagTable [10]". TIA Portal counts tags and
constants together: 4 tags, 1 user constant and 58 system constants; 8 tags
and 2 user constants.

`s13_hmi` continues from `s12_constants`, for HMI tags: a panel is added and
tags are made on it, one action per save. TIA Portal ran with a trial
licence for WinCC Advanced.

| save | action in TIA Portal V21 |
|---|---|
| 18, 19 | saved without a change; "Save as" `s13_hmi` |
| 20 | HMI device added: KTP400 Basic PN, 6AV2 123-2DB03-0AX0, version 17.0.0.0, named `ZZPANEL`, without the device wizard |
| 21 | the panel attached to subnet `PN/IE_1`; TIA Portal gave it the address 192.168.77.3 |
| 22 | HMI connection from `ZZPANEL` to ZZBRAVO (`HMI_Connection_1`) |
| 23 | HMI tag `ZZINT` in the default tag table: Int, internal tag |
| 24 | `ZZSTD`: `HMI_Connection_1`, PLC tag `DB_Standard.my_int` |
| 25 | `ZZMEM`: PLC tag `MyInt` (a PLC tag of "Tag MyTagTable", %MW20) |
| 26 | `ZZABS`: Word, absolute access, address %MW100 |
| 27 | `ZZSTD`: acquisition cycle 500 ms |
| 28 | `ZZSTD`: acquisition mode "Cyclic continuous" |
| 29 | `ZZINT`: comment `ZZCOMMENT` |
| 30 | `ZZINT`: start value 7 |
| 31 | new HMI tag table `ZZTABLE` with the tag `ZZUDT` on `DB_Standard.my_data_type`, a PLC data type |
| 32 | `ZZOPT` on `DB_Standard.my_real` (meant for `DB_Optimized`) |
| 33 | `ZZOPT` deleted |
| 34 | on ZZBRAVO the PLC tag `MyInt` deleted, which `ZZMEM` still names |
| 35 | second HMI connection, to ZZCHARLIE (`HMI_Connection_2`), and in `ZZTABLE` the tag `ZZCH` on it: Word, absolute access, %MW200 |
| 36 | in `ZZTABLE` the tag `ZZOPT1` on `DB_Optimized.my_real` |

References from TIA Portal, all taken after save 36 unless stated:

- `s13_hmi/exports/HMITags.xlsx`: its export of "Show all tags". Seven rows
  in the order ZZINT, ZZSTD, ZZMEM, ZZABS, ZZUDT, ZZCH, ZZOPT1 with name,
  path (the tag table), connection, PLC tag, data type, access method
  ("Symbolic access", "Absolute access"), address (only for the two with
  absolute access), start value, comment, acquisition mode ("Cyclic in
  operation", and "Continuous" for `ZZSTD`) and acquisition cycle.
- Screenshots of "Show all tags": the same, and a column "PLC name" with
  ZZBRAVO for five tags and ZZCHARLIE for `ZZCH`; the PLC tag of `ZZMEM` on a
  red background; `ZZUDT` opened to its members memberDate (Date),
  memberString (String), memberInt (Int), memberBolean (Bool).
- A screenshot of the Inspector window for `ZZSTD`: acquisition mode "Cyclic
  continuous", acquisition cycle 500 ms.
- A screenshot of the default tag table after save 27: the address column is
  empty for `ZZSTD` and `ZZMEM` (symbolic access) and has %MW100 for `ZZABS`.
- A screenshot of the topology comparison after save 21: `ZZPANEL.IE_CP_1`,
  PROFINET device name `zzpanel`, 192.168.77.3.

The export of the public V19 sample project (github.com/LCC-Automation/
OpenPID-TIA-SCL, opened in TIA Portal V21) was compared in the same way by
hand, 100 rows without a difference; neither that project nor its export is
in this repository.

The following belongs to `s11_blocks`.

Three more screenshots were taken at the end, on ZZBRAVO, and the test holds
what they show:

- Program info > Resources: load memory and work memory of every block
  (Main 4374 / 173 bytes, Block_1 3436 / 82, ZZFC 2906 / 82, DB_Standard
  5824 / 3166, DB_Optimized 5622 / 3268, Block_1_DB 1659 / 180, Block_2_DB
  1645 / 180, User_data_type_1 912), and "?" for `Block_2` and `ZZDB`, which
  have to be compiled again.
- `ZZFC` > Properties > Time stamps, in local time (UTC+2): created 10:35:50
  PM, modified 10:50:22 PM, interface modified 10:39:56 PM, code modified
  10:48:10 PM, load-relevant 10:50:22 PM.
- OB1 `Main` in the editor: three networks (two block calls and an empty
  one), block title `"Main Program Sweep (Cycle)"`, quotes included.

OB1 `Main` of ZZCHARLIE has one network, an empty one, as counted in TIA
Portal.

`s14_code` is a new project for the code of blocks: one S7-1500 CPU
(CPU 1511-1 PN, `ZZPLC`) and blocks in LAD, FBD, SCL and STL, one action per
save, in TIA Portal V21 (STEP 7 Professional trial licence).

| save | action in TIA Portal V21 |
|---|---|
| 3 | S7-1500 station with CPU 1511-1 PN, named `ZZPLC` |
| 4 | PLC tags `ZZA` Bool %M10.0, `ZZB` Bool %M10.1, `ZZC` Bool %M10.2, `ZZOUT` Bool %M11.0, `ZZN1` Int %MW20, `ZZN2` Int %MW22, `ZZN3` Int %MW24 |
| 5 | global data block `ZZDATA` with `run` Bool and `speed` Int |
| 6 | Main, network 1, title `ZZ series`: contact `ZZA`, normally closed contact `ZZB`, coil `ZZOUT` |
| 7 | Main, network 2, title `ZZ parallel`, comment `two branches`: `ZZA` parallel to `ZZC`, set coil on `"ZZDATA".run` |
| 8 | Main, network 3: compare `ZZN1` > 100 (Int), then MOVE `ZZN2` to `"ZZDATA".speed` |
| 9 | Main, network 4: ADD `ZZN1` + 5 to `ZZN3` |
| 10 | Main, network 5: contact `ZZA`, TON with the proposed instance `IEC_Timer_0_DB`, PT `T#5S`, Q to coil `ZZC`; ET was given `"IEC_Timer_0_DB".ET` (not intended) |
| 11 | new FC `ZZFBD` in FBD: AND of `ZZA` and negated `ZZB`, assigned to `"ZZDATA".run` |
| 12 | new FC `ZZSCL` in SCL, empty |
| 13 | `ZZSCL`: the code (see `exports/ZZSCL.scl`) |
| 14 | new FC `ZZSTL` in STL, network 1: `A "ZZA"`, `AN "ZZB"`, `= "ZZOUT"` |
| 15 | `ZZSTL`, network 2: `L "ZZN1"`, `L 5`, `+I`, `T "ZZN3"` |
| 16 | new FB `ZZFB` in LAD, input `in1`, output `out1`: contact `#in1`, coil `#out1` |
| 17 | Main, network 6: call of `ZZFB` with instance `ZZFB_DB`, `in1` `ZZA`, `out1` `ZZC` |
| 18 | Main, network 7: `ZZFBD`, `ZZSCL` and `ZZSTL` in one rung, ENO to EN |
| 19 | compiled; it failed on the ET of the timer (read-only), which was then removed and compiled again |
| 20 | Main, network 1: `ZZB` replaced by `ZZC` |
| 21 | tag `ZZA` renamed to `ZZALPHA`, not compiled |
| 22 | compiled, and network 2 of Main (`ZZ parallel`) deleted |
| 23 | `ZZSCL` know-how protected (throwaway password) |

References from TIA Portal, taken after save 19 (before the edits of saves
20 to 23):

- `s14_code/exports/ZZSCL.scl` and `ZZSTL.awl`: "Generate source from blocks"
  of the two blocks.
- Screenshots of the eight networks of Main, of the networks of `ZZFBD`,
  `ZZFB`, `ZZSCL` and `ZZSTL` (the STL editor shows instruction and operand
  in two columns, whatever blanks were typed).
- Cross-references of `ZZA`: Main networks 1, 2 (ZZ parallel), 5 and 6,
  `ZZFBD` network 1, `ZZSCL` program code, `ZZSTL` network 1.
- Program info > Call structure: Main calls `ZZFB` with `ZZFB_DB` (network 6),
  `ZZFBD`, `ZZSCL` and `ZZSTL` (network 7); data blocks accessed:
  `IEC_Timer_0_DB` (Main network 5), `ZZDATA` (Main networks 2 and 3, `ZZFBD`
  network 1, `ZZSCL`).
- After save 21: a screenshot of Main with `"ZZALPHA"` in networks 1 and 2.

With the know-how protection (save 23) TIA Portal wrote the project file
anew, without the earlier saves, and kept the old file in
`s14_code.backup/2026-10-08.031839.005/2026-10-08.031839.005.zip`.
`s14_code/System/PEData.plf` is the file from that backup.
`s14_code_rewritten` is the project file after that: its first save holds the
whole project, then

| save | action in TIA Portal V21 |
|---|---|
| 2 | the rewritten file closed by the next save: a new network inserted after network 1 of Main, title `ZZ inserted` |
| 3 | that network: contact `ZZC`, coil `ZZOUT` |

The tables above are also the reference for the save history
(`tiaconv --history`): for every save listed in them, the test expects the
history to show that action and what TIA Portal changed along with it, and
nothing else. The seven steps of `s00` to `s07` are saves 3, 5, 7, 9, 11, 13
and 15 of the later files, each followed by the "Save as" that made the next
project. Two saves show no change by design: 67 (a password) and 68 (a role).
For the time of a save there is one reference: the file of `s12_constants`
on the PC was last written at 2026-10-07 20:44:27.140 UTC, and its last save
holds 20:44:27.109 as the latest change.


All passwords in these projects are throwaway test passwords.

`tests/fixtures_test.cpp` checks these facts.
