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

An HMI connection is missing: adding an HMI device, a Basic panel included,
needs a WinCC licence that was not available.

All passwords in these projects are throwaway test passwords.

`tests/fixtures_test.cpp` checks these facts.
