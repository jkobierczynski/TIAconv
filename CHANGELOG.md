# Changes

## 0.13.0

- STL: comments (`//` on a line of its own or after an operand), jump
  labels (`M001: NOP 0`), the comma of `CALL "FB", "DB"`, and the
  parameters of a `CALL`, listed as the editor shows them
  (`in1  :="ZZB"`), with their names from the called block's interface.
- SCL: `END_WHILE`, `UNTIL`, `END_REPEAT` and ranges in `CASE` (`1..5`).
  The comment in several languages `(/* */)` is now checked against the
  editor.
- LAD: edge contacts as `P(operand, edge bit)` / `N(...)` in the condition
  instead of a box of their own.
- Cross-reference: jump labels (kind `label`, access `definition` and
  `jump`, as TIA Portal lists them) and names the project does not know
  (kind `undefined name`).
- The test project `s14_code_rewritten` has a second series of 22 saves with
  these constructs, a negated coil, and a counter and a timer as
  multi-instances, all checked against the editor and TIA Portal's
  cross-reference lists.

## 0.12.1

- Cross-reference: the kinds of access in TIA Portal's words: the instance
  data block of a call is `single instance` (was `instance`), the actual
  parameter of an InOut `read and write` (was `read/write`), a multi-instance
  `multiple instance` (was `multi-instance`). A data block has a row of its
  own only in networks where it is used as a whole, not where only a member
  of it is used. Checked against TIA Portal's full cross-reference list of
  the test project.
- Cross-reference: the multi-instances an FB declares in its interface
  have a row, as in TIA Portal's list (`#inner (data type)`). The interfaces
  of FBs are read for this; know-how protected ones are not.
- The test project `s14_code_rewritten` has five more saves: an InOut
  parameter, whose actual parameter TIA Portal and tiaconv list as
  `read and write`; an FB with a multi-instance, called from Main; compiling.

## 0.12.0

- `tiaconv diff OLD NEW`: what differs between two projects, or two
  versions of one, in the terms of the save history: hardware, addresses,
  security settings, connections, blocks and their code, data block
  members, tags, HMI tags, constants. `--old-save` / `--new-save` take a side
  as it was after one of its saves; `-j` and `--csv` write the differences;
  `--exit-code` gives 1 when the two differ in more than time stamps and
  compiling. Versions of one project are paired by the identities of their
  objects, projects made apart by name. Know-how protected blocks are
  compared without their code, on both sides.

## 0.11.0

- Block code: `--code` adds the networks of every OB, FB and FC to the text
  report and the JSON (`code`), with title, comment and language. SCL and
  STL come out as source text; LAD and FBD as a listing in text form, in a
  notation of tiaconv's own (see the README), with the parts and their pins
  in the JSON; a list of who calls whom follows.
- `--xref-csv` writes the cross-reference: which block reads, writes or
  calls which tag, data block member, data block or block, per network.
- The save history lists networks: added and removed ones with their code,
  changed ones with the lines taken out and put in.
- Know-how protected blocks and the protected blocks of Siemens libraries
  are not read, and neither is an earlier, unprotected version of a block
  that the file still holds from before it was protected (`--save`,
  `--history`).
- Names in code are the current ones: a tag or block renamed since the
  block was last compiled shows its new name, as in TIA Portal's editor.
- Checked against a new V21 test project with code in LAD, FBD, SCL and STL
  (`s14_code`): screenshots of every network, the generated sources, the
  cross-references of a tag and the call structure.
- Found: know-how protection makes TIA Portal write the project file anew,
  without its earlier saves; it keeps the old file in `<project>.backup`.
- The XML reader keeps the text inside elements.

## 0.10.0

- HMI tags: the tags of every HMI device with tag table, data type,
  connection, the PLC at the other end, the PLC tag or data block member the
  tag stands for, the address, and the acquisition cycle; internal tags and
  tags with absolute access as well. In the text report, the JSON
  (`hmi_tags`) and the new `--hmi-tags-csv`, in the order of TIA Portal's
  own export; the save history lists them too. Checked against TIA Portal
  V21's export of the tag table of a new test project (`s13_hmi`) and of a
  public sample project with 100 tags.
- What TIA Portal keeps in place of a tag or block that was deleted while
  something still names it is no longer listed as a tag or block.
- **`--objects` in 0.9.0 could write a password.** 0.9.0 began to read
  structures, and the settings of an HMI connection are a list of name and
  value structures, one of them named `Password`. Its value would have been
  written as it is stored. It is empty in the two sample projects, so no
  password is known to have been written; the report, the JSON and the CSV
  files were not affected. Fixed: a value that a structure itself names as a
  password is not written, and nothing below an attribute named like one, at
  any depth. If you kept an `--objects` file made with 0.9.0 from a project
  with an HMI device, check it for `"Name": "Password"`.
- Structures inside structures, arrays of numbers and of strings, and
  structures in expando attributes are decoded. Nearly every structured
  value of the sample projects now reads.

## 0.9.0

- Save history: `--history` adds to the text report and the JSON what was
  added, removed or changed with each save, with a time and a user name, as
  far as the file still holds its earlier saves; `--history-csv` writes one
  row per change. Compared is what tiaconv reports: hardware, addresses,
  security settings, IO systems, port connections, connections, blocks, data
  block members, tags and constants. A save that changed something else is
  listed with the kinds of object it wrote.
- The list TIA Portal keeps itself (project created, converted from an older
  version) is read: `project.events` in the JSON, and at the top of the
  history.
- Structures and arrays of structures in attribute values are decoded, where
  the stored data follows the rule exactly; `--objects` shows them as JSON
  objects and arrays instead of a size.
- A project file that goes on after its last save marker (a project archive)
  is reported as such in the last line of the report, and in the JSON as
  `source.objects_after_last_save`. `--save N` on such a file shows the
  state at the marker, as before.
- Documentation: a file starts with a save that holds only the type model;
  the description of archives and rewritten files was corrected accordingly.

## 0.8.0

- Constants: the hardware identifiers of each PLC with the module, interface
  or port each one stands for, and user constants with data type, value,
  comment and tag table. The JSON (`constants`) and the new
  `--constants-csv` also have the other system constants (OB numbers,
  process image partitions).
- The type model reader now knows the relations that are declared as the
  other direction of a relation of another type (185 of them in a V21
  project), so `--objects` shows their names.
- Build: with Visual C++, `-DTIACONV_STATIC=ON` now applies the static
  runtime to every target. The test programs failed to link before.

## 0.7.1

- Memory sizes of blocks as TIA Portal lists them under Program info >
  Resources: the load memory of a PLC data type was reported as 0, and a
  block that has to be compiled again kept the sizes of its last
  compilation where TIA Portal shows none.

## 0.7.0

- List of blocks per PLC: OB, FB, FC, DB and PLC data types with number,
  name, kind, language, folder, know-how, write and copy protection, whether
  the block is compiled, time of the last change and of the last download.
  The JSON and the new `--block-list-csv` also have title, comment, author,
  family, version, user-defined ID, the other time stamps, the download
  history, memory sizes, and for data blocks whether they are write-protected
  in the device and accessible from OPC UA and the web server.
- Data blocks: what was called `comment` was the block's title. The JSON now
  has `title` and `comment`; the text report shows the title.
- `--objects` no longer prints the salt, initialisation vector and
  verification tag stored with a protected block.
- Corrected: a project file is not only ever appended to. TIA Portal can
  rewrite it, and then `--save` has nothing to go back to.

## 0.6.0

- PROFINET IO systems: which IO device is assigned to which controller.
- Port connections (the cabling of the topology view).
- Configured S7 and HMI connections, including connections to a partner
  outside the project.
- `--save N` shows the project as it was after its N-th save.
- Fixed: distributed IO stations (ET 200SP and the like, under "Ungrouped
  devices") were only listed with `--all-devices`. They are devices of the
  project and are now listed by default, in all outputs.
- CPUs with user management (S7-1500 with firmware V3.1 and V4.1, S7-1200
  with firmware V4.7 in the samples) are recognised. For them the report
  says that users and roles decide about access and that those are not
  read, and prints the stored access level as "Access without login": it is
  the level TIA Portal derives from the rights of the Anonymous user.
  "Access control: disabled" now says that the CPU has no access protection.
- The access levels of an S7-1200 with firmware V4 or later are named.
- Hardware CSV: new columns `io_controller`, `io_system` and
  `access_protection` at the end. JSON: new top-level `io_systems`,
  `port_links`, `connections`; `source.shown_save`; per controller
  `access_protection`, `user_management`, `function_right_set`.

## 0.5.0

- Security settings of each CPU: access level, PUT/GET, web server, OPC UA
  server, NTP, display protection, access control, legacy communication,
  protection of configuration data.
- The number of saves a file records.
- Interfaces that exist only inside TIA Portal ("Virtual ...") are hidden
  unless `--all-items` is given.

## 0.4.2

- CSV files start with a UTF-8 byte-order mark; `--no-bom` leaves it out.

## 0.4.1

- Fixed: default values of a PLC data type were not shown for members of
  that type in a data block.

## 0.4.0

- Comments of tags, data blocks and data block members.

## 0.3.0

- PLC tags and data blocks: members, data types, start values, offsets.

## 0.2.0

- Hardware and network inventory; text, JSON and CSV output.
