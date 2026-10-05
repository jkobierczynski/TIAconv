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

`tests/fixtures_test.cpp` checks these facts.
