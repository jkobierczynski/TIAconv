# The program export (`--program-json`)

```
tiaconv --program-json DIR project
```

writes the program of each PLC of the project as one JSON file into `DIR`,
named after the PLC (`ZZPLC.json`; characters a file name cannot have
become `_`). The file is meant as the input of a simulator: everything
needed to run the program outside TIA Portal that the project file holds,
and a list of what it does not hold. With `--save N` it is the program as it
was after save `N`.

The exact form is the JSON Schema [`program.schema.json`](program.schema.json)
(draft 2020-12). Every object in it lists all its keys as required and
allows no others, so the schema describes the files completely; the tests
check the export of every PLC of every test project against it.

Like every output of tiaconv the export only reads: nothing of a know-how
protected block is in it (no interface, no code, also not from an earlier
save of the block), and no passwords.

## Versions

`schema_version` is 2. It goes up with every change of the format, also
when a key is only added, since the schema allows no unknown keys. The
changes are listed in the CHANGELOG.

Version 2 is being extended toward a format an interpreter can load
without parsing TIA Portal's notation (DEVELOPMENT.md, "Objectives: the
export as input of an interpreter"); it is fixed when it is released. So
far it adds to version 1 the type descriptors with the `types` table, and
values parsed (`start`, `parsed`, `value`).

## The file

| key | what |
|---|---|
| `format` | always `"tiaconv-program"` |
| `schema_version` | `2` |
| `tool` | `name` (`"tiaconv"`) and `version` |
| `source` | `file` as given, `project` name, `save` (null: the project as it is now) |
| `plc` | `name`, `station`, the CPU's `type`, `order_number`, `firmware` |
| `io` | the I/O addresses of the modules in the PLC's station and in the IO devices it controls |
| `tags` | the PLC tags: `name`, `table`, `data_type`, `type` (below), `address` (`%I0.0`, `%MW20`), `comment` |
| `constants` | user constants and the system constants TIA Portal creates: `name`, `table`, `data_type`, `value` as written and `parsed` (below), `kind` (`hardware`, `ob`, `pip`, `user`, `system`), `system`, `comment` |
| `types` | the PLC data types and the system types the members use, with their members |
| `data_blocks` | global and instance data blocks with their members |
| `organization_blocks` | each OB with its `event`, `priority`, and for a cyclic interrupt `cycle_us` and `phase_shift_us` (microseconds) |
| `blocks` | every OB, FB and FC with its interface and its code |
| `instructions` | the instructions the code uses (`TON`, `CTU`, `PID_Compact`) with their library version: a simulator has to provide these |
| `not_simulatable` | what keeps a block from being simulated from the file: `block`, `type`, `reason` |

Texts that are not stored are `null`, not `""`. Lists are always present,
possibly empty.

### I/O

`{"station", "module", "item", "area": "I"|"Q", "start_bit", "length_bits",
"process_image", "text"}`. `start_bit` counts from the start of the area:
`I 64...67` is `start_bit` 512, `length_bits` 32. `item` is the part of
the module that has the address (`AI 2_1` of a CPU 1215C). A
`process_image` of 0 is automatic update. ET 200SP modules are not read
yet (see DEVELOPMENT.md).

### Members

Data block members and block interfaces have the same form:

| key | what |
|---|---|
| `name`, `data_type` | `Array[0..9] of Int`, `String[20]`, `"MyUdt"`, `TON_TIME` as TIA Portal writes them |
| `type` | the data type taken apart (below) |
| `section` | `Input`, `Output`, `InOut`, `Static`, `Temp`, `Constant`, `Return`; null in a global data block and below the top level |
| `offset`, `offset_bits` | `"12.3"` and 99, only for standard (not optimized) access |
| `start_value` | as written in the project; null: the default of the type (or of an enclosing structure) |
| `start` | `start_value` parsed (below); null with it |
| `retain` | |
| `accessible_from_hmi`, `writable_from_hmi`, `setpoint` | as limited by an enclosing structure |
| `unresolved` | the nested type could not be followed |
| `comment` | |
| `members` | of a structure, a PLC data type, a multi-instance |

A member of a PLC data type, a system type or an FB (a multi-instance)
has its members as well, so a data block can be laid out without the
`types` table. Arrays are not expanded into their elements: the members of
an array of a structured type are those of one element; an array of a
structured type has not been seen in a test project yet.

### Types

`type` takes `data_type` apart, so that a simulator does not parse it:

| key | what |
|---|---|
| `kind` | `elementary`, `string`, `array`, `struct` (an anonymous structure: the members are in the member), `plc_type` (a PLC data type, in `types`), `system_type` (a type the system provides: `TON_TIME`, `DTL`, `IEC_TIMER`, also in `types` when it has members), `instance` (an FB: the members are its interface), `parameter` (`Variant`, `Any`, `Pointer`, `Block_FB`, ...), `void`, and `named` or `unknown` for what tiaconv could not place (not met in the test projects and samples) |
| `name` | `Int`, `String`, `Array`, `Struct`, the name of the type or FB without quotes |
| `bits` | elementary: the size, `Bool` 1, `Int` 16, `LReal` 64, `Date_And_Time` 64; also the system types that are numbers (`Hw_SubModule`, `OB_Cyclic`, `Pip`, `DB_Any`: 16) |
| `length` | string: the maximum number of characters, 254 when not given |
| `dims` | array: per dimension `low`, `high` and as written `low_text`, `high_text`. A bound written as a constant (`#TIME_LAG_MAX`, a local constant of the block or of the FB of a multi-instance; `"MAX"`, a user constant of the PLC) has its value; `null` when it cannot be known (`Array[*]`) |
| `element` | array: the element's `type` |

`types` lists `{"name", "kind": "plc_type"|"system_type", "members"}`: every
PLC data type of the PLC from its definition (with its own start values),
and the PLC data types whose definition is protected (Siemens library
types) and the system types as met in a member of that type
(`TON_TIME`: `ST`, `PT`, `ET`, `RU`, `IN`, `Q`).

### Blocks

`{"type": "OB"|"FB"|"FC", "name", "number", "language", "optimized",
"protection", "simulatable", "interface", "code"}`. `interface` has every
section of the interface (Temp, Constant and Return too); `interface` and
`code` are null for a protected block. `simulatable` is false when the
block is listed under `not_simulatable`.

`code` is `{"networks": [...], "references": [...]}`. A network:

| key | what |
|---|---|
| `number` | position in the block, 1 = first |
| `network_id` | the number the project knows it by |
| `title`, `comment`, `language` | |
| `content` | `scl`, `stl`, `graphic` (LAD, FBD), `empty`, `unread` |
| `lines` | SCL, STL: the source text, as the editor shows it; graphic: tiaconv's listing (README) |
| `elements` | graphic: the parts and their wires |
| `notes` | why something was not read |

An element is a part of a LAD or FBD network: `uid`, `kind` (`gate`: a
contact, coil or box; `call` of a block; `instruction` with an instance),
`name` (the gate as stored, `Contact`, `Coil`, `Add`, `Move`, `PBox`; the
block called; the instruction), `ref_id` (call, instruction),
`instance`, `instance_ref_id`, `data_type` (the type a box was set to;
null with the option `AutomaticTyped`: left on Auto), `options` (`SrcType`,
`Card`: the number of inputs, `DisableENO`, `negated` with the pin), and
`pins`.

A pin: `name`, `direction` (`in`, `out`), `connected` (an operand as
written in code, `"power rail"`, or another part's pin as
`"<uid>.<pin>"`), and `operands`, the operands among `connected`:

| key | what |
|---|---|
| `text` | as written in code |
| `ref_id`, `linked` | its entry in the block's `references`, and whether that was found |
| `kind` | `tag`, `data block member`, `local`, `constant`, `local constant`, `user constant`, `multi-instance` |
| `data_type` | the entry's; a literal stored without a type takes the pin's (`data_type_from_pin`) |
| `data_block` | of a data block member |
| `address` | a tag's; a member of a data block with standard access as `%DB1.DBX60.0`, `%DB2.DBW4`, `%DB3.DBD8` (elementary types up to 4 bytes) |
| `pin_type` | the type the pin takes where tiaconv knows it (contacts, coils, typed and Auto boxes, the parameters of a called block, IEC timers and counters); null otherwise |
| `value` | a literal, a local constant (`#MAX`) or a user constant (`"MAX"`): its value parsed (below); null for anything else |

How each of these is derived is in [FORMAT.md](FORMAT.md) ("Operand links").

`references` is the block's table of what its code refers to: `ref_id`,
`kind`, `kind_stored`, `text`, `data_type`, `value` (a constant's, as for
an operand), `data_block`, `declared_as` (a
multi-instance: the member declared with this block as its type). The
`ref_id` of an operand or element points into it.

### Values

Start values, constants and the literals in code are parsed, so a
simulator does not read TIA Portal's notation. A value is

`{"kind", "text", "bool", "int", "real", "ns", "days", "string"}`

with the field of its kind set and the others null:

| kind | field | from |
|---|---|---|
| `bool` | `bool` | `TRUE`, `false`, `BOOL#1`; `1` and `0` for a Bool |
| `int` | `int` | `5`, `-5`, `16#FF`, `2#1010_1010`, `W#16#8600`, `DW#16#...`, `INT#-7`; also the hardware identifiers and other numbers. A ULInt or LWord above 2^63-1 is written as the unsigned number |
| `real` | `real` | `0.1`, `-3.402822E+38`, `REAL#2`; a whole number for a Real or LReal (`1` is 1.0) |
| `duration` | `ns`, nanoseconds | `T#5s`, `T#1D_2H_3M_4S_5MS`, `T#-1.5S`, `LTIME#10US_5NS`, `S5T#10S`; a whole number for a Time counts milliseconds, for an LTime nanoseconds |
| `date` | `days` since 1990-01-01, as the PLC keeps a Date | `D#1990-01-31` (30) |
| `time_of_day` | `ns` since midnight | `TOD#12:30:15.250`, `LTOD#...` |
| `date_and_time` | `ns` since 1970-01-01 | `DT#1990-01-01-00:00:00`, `LDT#...`, `DTL#...` |
| `string` | `string`, as UTF-8 | `'Hello world'`; the escapes `$'`, `$$`, `$N`/`$L`, `$R`, `$T`, `$P` and `$hh` (`$hhhh` in a WString) decoded |
| `char` | `int` (the code) and `string` | `' '` for a Char, `CHAR#'A'` |
| `unknown` | none | anything else; `text` has it as written |

The type of the member, constant or pin settles what the text alone does
not (`1` for a Real, `5000` for a Time, `'A'` for a Char). A local constant
whose value equals the default of its type has none stored; its `value`
is that default, with an empty `text`. In the test projects and samples all
of the 1400+ values parse.

### Not simulatable

One entry per reason. The reasons:

- `know-how protected`, and `know-how protected later; this version is not
  read` for an earlier save of a block protected since;
- `system protected: an instruction from a Siemens library` (PID_Compact,
  MB_SERVER): the simulator has to provide it, as for `instructions`;
- `GRAPH`, `fail-safe`: from the block's language (not yet seen in a test
  project);
- `interface not read`, `network N not read: ...`, `code not found`;
- for a data block, `members not read: ...`.

## What is not in it yet

- The retentive memory area of the tags (how many memory bytes are
  retentive) is not read; data block members have `retain`.
- I/O of ET 200SP modules; the hardware identifiers are in `constants`.
- Pin types of `Move`, `S_Move` and of instructions other than the IEC
  timers and counters; absolute addresses inside arrays and of types longer
  than 4 bytes.
- Instances of library types (which type and version a block is an
  instance of).
