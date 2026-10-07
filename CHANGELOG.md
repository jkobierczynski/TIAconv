# Changes

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
