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
| `organization_blocks` | each OB with its `event`, `priority`, for a cyclic interrupt `cycle_us` and `phase_shift_us` (microseconds), `event_class` (1 program cycle, 30 cyclic interrupt, 100 startup), `max_buffered_events`, and `process_image`, the partition it updates (null for none) |
| `runtime` | how the CPU runs the program (below) |
| `blocks` | every OB, FB and FC with its interface and its code |
| `instructions` | the instructions the code uses (`TON`, `CTU`, `PID_Compact`) with their library version: a simulator has to provide these |
| `call_graph` | one entry per call in the code: `from`, `from_type`, `network`, `kind`, `to`, `to_type`, `instance_kind`, `instance` (below) |
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
| `statements` | the code as a tree, the same form for SCL, LAD, FBD and STL (below); null when the network was not read |
| `tree_notes` | what of the tree could not be read (none in the test projects and samples) |
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
an operand), `data_block`, `name` (of the tag, block or data block named),
`scope`, `path` (an access as its names: `["MotorData", "starts"]` for
`"MotorData".starts`, `["delay", "Q"]` for `#delay.Q`, each step with the
`ref_id`s of its array indices), `modifier` (the part meant: `b0`, `x3`), `declared_as` (a
multi-instance: the member declared with this block as its type). The
`ref_id` of an operand or element points into it.

### The code tree

`statements` is what an interpreter runs: the code of the network as
statements and expressions, the same for every language, with every
operand named by its `ref_id` in the block's `references`. A statement is

`{"op", "name", "exprs", "branches", "body", "else"}`

| op | meaning |
|---|---|
| `assign` | `exprs[0]` := `exprs[1]` |
| `call` | `exprs[0]`, a call |
| `if` | `branches`, one per IF and ELSIF, each `{"exprs": [condition], "body"}`; `else` the ELSE part (null without one) |
| `case` | `exprs[0]` the selector; `branches` with the labels in `exprs` (a `range` for `1..5`) and their `body`; `else` |
| `for` | `exprs`: the variable, from, to and, when given, by; `body` |
| `while` | `exprs[0]` the condition; `body` |
| `repeat` | `body`, until `exprs[0]` |
| `return`, `exit`, `continue`, `empty` | |
| `eval` | `exprs[0]` evaluated for its edges: LAD or FBD logic that leads nowhere |
| `region` | `name` and `body`: only a grouping |
| `stl` | one STL statement: `name` the instruction (`A`, `AN`, `=`, `L`, `T`, `+I`, `JC`, `CALL`), `exprs` its operands; a CALL's parameters as `in`, `out`, `inout`; `text` the operand as the editor shows it (`"ZZN1"`, `M001`, `"ZZFB","ZZFB_DB"`), also for operands the tree has no node for: absolute and indirect addresses (`DBW 2`, `MW [MD 10]`, `[AR1,P#2.0]`), S5 timers and counters (`T 5`), pointers (`P#M10.0`) |
| `label` | `name`: an STL label, before the statement it marks |
| `unknown` | `name` says what (not met in the test projects and samples) |

An expression is `{"op", "ref_id", "instance_ref_id", "name", "type", "value", "args"}`:

| op | meaning |
|---|---|
| `ref` | an operand: `ref_id` |
| `const` | a literal: `value` parsed (see Values) |
| `member` | `args[0]`.`name`; `ref_id`: the entry for the whole access (`#delay.Q`), so an interpreter can take either |
| `index` | `args[0]`[`args[1]`, ...]; `ref_id` as for `member` |
| `slice` | a part of `args[0]`, `name` `%X3`, `%B1`, `%W0` |
| `not`, `neg` | one operand |
| `add` `sub` `mul` `div` `mod` `pow` `and` `or` `xor` `eq` `ne` `lt` `le` `gt` `ge` | two operands, left to right |
| `call` | `name`; `ref_id` the block or instruction called (null for a built-in function such as `INT_TO_WORD` or `ABS`); `instance_ref_id` its instance data block or multi-instance; `args` its parameters; `target` what it runs, resolved (below) |
| `in`, `out`, `inout` | a parameter: `name` (null when given by position), `args[0]` the operand |
| `edge_pos`, `edge_neg` | LAD and FBD: the rising or falling edge of `args[0]`, with `args[1]` the edge memory bit it updates |
| `wire` | LAD and FBD: the value of a pin of another part, `name` `<uid>.<pin>`, set by an earlier statement of the same network |
| `range` | `args[0]`..`args[1]`, in CASE labels |
| `builtin` | `ENO` of the block itself |
| `missing` | nothing entered; `unknown`: `name` says what |

`type` is the data type of the result where the project stores it (every
expression of SCL).

**SCL** is stored as a syntax tree already, with the kind of each statement
and expression; the tree is taken from it as it is (FORMAT.md, "SCL as a
tree").

**LAD and FBD** become statements in the order the parts are evaluated:
the order on the screen (the order the project stores the parts in), each
part as soon as the parts whose outputs it reads are done.

- Contacts, `A`, `O`, `X`, `Not`, comparisons and edge contacts become
  expressions in the statement of the part that reads them. Logic read by
  more than one part is computed once, into a `wire`, as the PLC computes
  it once at the branch: an edge must not be evaluated twice, and a box on
  one branch may change an operand the other branch reads. Logic that
  leads nowhere becomes `eval` when it has an edge to update.
- A coil is an `assign` (with NOT for a negated coil), `S` and `R` coils an
  `if` that sets the operand TRUE or FALSE. The power that flows on past a
  coil is its input as it was before the coil wrote: a `wire`.
- `Move` and `S_Move` become `assign`s, `Add`, `Sub`, `Mul`, `Div`, `Mod`
  an `assign` of the operation with the inputs in the order of their pins
  (`in1`, `in2`, ...), `PBox` and `NBox` (P_TRIG, N_TRIG) an edge. Their
  ENO is TRUE when they ran: an overflow is not reported.
- Calls of blocks and instructions become `call`, with the direction of
  each parameter from the called block's interface or, for an
  instruction, from the members of its instance. Their ENO is a parameter
  `out ENO`: what the block or instruction sets.
- A box with an EN runs inside an `if`; when it does not run its ENO is
  FALSE and its outputs keep their values. A negated output goes into its
  wire as it is; its readers and operands get NOT.
- A box tiaconv has no rule for is written as a `call` with that box's name
  and a note in `tree_notes` (none in the test projects and samples).

For an interpreter: wires belong to one network (their names repeat from
network to network); a wire read before it was written in this run of the
network has no defined value in the PLC either (TIA Portal keeps it in
temporary memory). `and` and `or` must evaluate every operand, as an edge
in one of them updates its memory bit.

The same logic gives the same tree: network 1 of `MotorLAD` (LAD) and the
first statement of `MotorSCL` (SCL) in `s15_motor` are both
`#run := (((#start OR #run) AND #stop) AND #interlock)`.

`tools/program_tree.py` prints the trees of a file as SCL-like text.

### Runtime

`runtime` says what an interpreter runs when:

| key | what |
|---|---|
| `startup` | the startup OBs, in the order of their numbers: run once, before the cycle |
| `program_cycle` | the program cycle OBs (OB 1 and those from 123 on), in the order of their numbers: run one after the other, again and again |
| `interrupts` | every other OB (cyclic interrupts and the rest), highest priority first: run when its event comes, interrupting an OB of lower priority. The event is in `organization_blocks` (a cyclic interrupt every `cycle_us`, shifted by `phase_shift_us`) |
| `process_images` | the partitions the I/O and the OBs use: `number`, `name` (the system constant of the PLC: "Automatic update", "PIP 1"), `updated`, `obs`, and the `inputs` and `outputs` in it (as in `io`) |

`updated` is `program_cycle` for partition 0: its inputs are read into the
process image before each program cycle and its outputs written after it.
`with_ob` for a partition an OB is assigned to (`obs`): read before that OB,
written after it. `instructions_only` for a partition no OB is assigned to:
only UPDAT_PI and UPDAT_PO update it. This is how S7-1200 and S7-1500 handle
partitions; the assignment of a partition to an OB has not been seen in a
project yet (every partition in the test projects is 0).

Not in the export because no project stores them: the maximum cycle time
(watchdog), a minimum cycle time, the cycle load for communication, the
startup mode. TIA Portal's defaults are 150 ms maximum cycle time, no
minimum, and a warm restart.

### Calls

Every `call` expression, and every STL `CALL` statement, has `target`:
what it runs, found in this file, so an interpreter does not look it up
by name.

| key | what |
|---|---|
| `kind` | `block`: an FB or FC of this PLC, in `blocks`; `instruction`: from a Siemens library (`TON`, `CTU`, `PID_Compact`, `MB_SERVER`), in `instructions`, which the interpreter provides; `builtin`: a function of the language (`INT_TO_WORD`, `ABS`); `unknown`: not found (none in the test projects and samples; listed under `not_simulatable` as "calls ..., which is not in the program") |
| `block_type`, `name`, `number` | `FB` or `FC`, the name without quotes, the number |
| `version` | an instruction's library version |
| `instance` | null for an FC, a built-in function, an instruction without one. Otherwise `kind` `data_block` (`name`, `number`, `instance_of`: an entry of `data_blocks`) or `multi_instance` (`path`: the member of the caller's interface, with the `ref_id`s of array indices; `data_type`: its type, `"ZZFB"` or `TON_TIME`) |
| `parameters` | the formal parameters: `name`, `section` (`Input`, `Output`, `InOut`, `Return`), `data_type`, `given` (whether the call gives it). From the block's interface; for an instruction from its instance; for the IEC timers and counters, whose instance types have no sections, their documented parameters (`TON`: `IN`, `PT`, `Q`, `ET`; `CTU`: `CU`, `R`, `PV`, `Q`, `CV`; ...). A parameter not given keeps what the instance holds (inputs and outputs of an FB) |

`call_graph` lists the same calls flat, in the order of the blocks and
their networks: `{"from": "Main", "from_type": "OB", "network": 6, "kind":
"block", "to": "ZZFB", "to_type": "FB", "instance_kind": "data_block",
"instance": "ZZFB_DB"}`; a multi-instance as `#inner`. The tests check
for every call of every test project that the block is in `blocks`, an FB
has an instance, an instance data block is one of that FB, a
multi-instance a member of that FB's type, and every parameter given is a
formal parameter.

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
