# The TIA Portal project data file (`PEData.plf`)

Working notes on the file format, as far as `tiaconv` needs it. Nothing here
comes from Siemens documentation. Each statement is marked:

- **verified** – holds for every block or object of all sample projects
  (see [Samples](#samples)), checked by the tool or the scripts in `tools/`.
- **inferred** – consistent with the samples but only seen in a few places, or
  a meaning guessed from names and values.
- **unknown** – present in the file, not understood.

Integers are little-endian. A *varint* is 7 bits per byte, low group first,
high bit set on every byte but the last.

## Credits

The newer layout (block framing, SHA-256, the embedded type model, attribute
sets, basic type ids, expando tables) was first described by Nils Emmerich,
ERNW, in ["TIA Project Parser"](https://insinuator.net/2026/08/tia-project-parser/)
and the BSD-licensed [tia-parser](https://github.com/ernw/tia-parser).
`tiaconv` shares no code with it.

New in these notes: the older layout, the slot count in the block header,
relation lists, the older expando segment, the deleted flag in the older
layout, the rule for which attributes are stored, the page format of large
values, texts in several languages, attributes that carry their own names,
how saves are delimited, and how hardware, network, security settings, tags,
data blocks and comments map onto objects.

## A project on disk

    MyProject/
      MyProject.ap15_1        small XML file: name, icon, version
      System/PEData.plf       all project data
      System/PEData.idx       index into the .plf (not needed for reading)
      ...

A project archive (`.zap15_1`, `.zap19`, ...) is an ordinary ZIP file with the
same tree inside (**verified** for one V19 archive).

Large projects are said to spread their data over several `.plf` files. No
sample has that; `tiaconv` reads `PEData.plf` only and warns about others.

## Two layouts

| | older layout (`v11`) | newer layout (`v14`) |
|---|---|---|
| seen in | V13 | V15.1, V16, V19, V21 |
| file header | 46 bytes | 98 bytes |
| bytes 0..3 | `40 00 01 00` | `40 00 00 00` |
| block header | 28 bytes | 44 bytes (adds a 16-byte UUID) |
| after each block | nothing | SHA-256 of the block |
| system objects | type id 0, told apart by object id | type ids `0x70000`.. |
| save markers | bare `$$COMMIT$$` / `##CLOSE###` records | `Commit` / `Close` system objects |

Which TIA versions use which layout is **inferred** from the samples; V11,
V12, V14, V17 and V18 files have not been looked at.

### File header

Older layout, 46 bytes: `40 00 01 00`, u32 `1`, a 16-byte GUID, 21 bytes
**unknown**, `FF`.

Newer layout, 98 bytes: `40 00 00 00`, u32 `1`, a 16-byte GUID, 41 bytes
**unknown**; bytes 65..96 are the SHA-256 of bytes 0..64 (**verified**), byte
97 is `FF`.

### Blocks

The rest of the file is a list of blocks, one per object version (**verified**:
all files split into blocks exactly to the last byte, and in the newer layout
every block hash matches).

| offset | size | |
|---|---|---|
| 0 | 4 | block size `s`, counted from offset 0, excluding the hash |
| 4 | 4 | object type id |
| 8 | 8 | object id |
| 16 | 8 | **unknown** |
| 24 | 2 | flags; bit `0x04` = deleted |
| 26 | 1 | slot count (see below) |
| 27 | 1 | **unknown** (5 in V19 and V21, 2..10 in V13) |
| 28 | 16 | UUID – newer layout only |
| 28 / 44 | | object data |
| `s` | 32 | SHA-256 of bytes 0..`s`-1 – newer layout only |

Saving normally appends: new versions of changed objects are written at the
end, and the **last block with a given (type id, object id) wins**. A deleted
object gets a tombstone block that is just a header with the deleted flag:
33 bytes in the older layout, 49 in the newer (**verified**: 654 objects of
the V13 sample and 4 objects of the V21 test projects end that way).

In the older layout two bare 20-byte records can appear between blocks, written
when a save completes: `0A "$$COMMIT$$"` or `0A "##CLOSE###"`, an 8-byte
timestamp, `FF` (**verified**). They have no size field and must be recognised
by content.

In the newer layout every save ends with one system object of type `0x7000C`
(**verified**: a V21 project saved 23 times in a row, one change per save,
gained exactly one per save, and the blocks before each one hold exactly that
change; later extended to 85 saves, see [Save history](#save-history)).
Reading the block list only up to the n-th of them gives the project as it
was after save n; `ContainerOptions::throughSave` does that. The object
carries counters, among them the number of the save, and **no time stamp**.
After the last one a file has one or two more system objects.

The first of these objects follows the type model directly: **save 1 of a
file holds no project**. In a new project the second save is the empty
project (344 objects in V21).

"Save as" keeps the history. Two samples were **written in one go**: the
project archive (`.zap19`) of the V19 sample and the V15.1 sample have that
first save object after the type model and then the whole project, with no
save object after it. The present state of such a file is therefore not the
state after its last save; `Container::objectsAfterLastSave()` counts the
object blocks written there. What writes a file that way, besides
archiving, is **unknown**.

**TIA Portal sometimes rewrites the whole file**, and then the history is
gone. Seen once (V21): a file of 2.9 MB with 80 saves was 1.2 MB with two
save markers a few saves later, the first right after the type model, the
second at the end, and only the current version of every object in between.
In those saves a block was edited, the program compiled, one block know-how
protected and one write-protected. In a later test project the file was
rewritten right after a know-how protection and nothing else, so that is
very likely the cause; see [Know-how protection](#know-how-protection). The saves after it were appended
again, including the one that removed the protection.

Timestamps are .NET `DateTime` values: 100 ns ticks since 0001-01-01 in the low
62 bits, kind in the top two (1 = UTC).

## System objects

Their data is `u32 length`, `length` bytes of payload, `FF`.

- **Type model** (`0x70000`; object id 1 in the older layout): a zlib stream
  holding an XML document, root element `MetaInfo`. Described below.
- **Resolved storage layout** (older layout only, several documents): a zlib
  stream holding an XML document, root element `StorageMetaInfoXML`. For
  every object type it lists the attribute sets with, per attribute,
  `isConstant`, `isIntrinsic` and `mappedRelationId`: the outcome of the rule
  described under [Which attributes are stored](#which-attributes-are-stored).
  `tiaconv` does not read it; it is what the rule was checked against.
- **Expando key table** (`0x70011`; in the older layout recognised by its
  structure): names and types of the dynamically named attributes of one
  object type.

      u32 length          (of this payload)
      u32 object type id
      u32 count, u32 count
      count x { u32 key, string name, u32 value type id }

  Strings in system tables are a varint length followed by that many bytes
  (the length does **not** count itself). One table exists per object type;
  older versions of a table hold a subset of the keys (**verified**).
- **Expando object table** (`0x70002`; object id 7 in the V13 sample): `u32
  count` and `count` × { object type id, table type id, table object id }. Not
  needed, since every key table names its object type.
- Others (named objects, packages, modification data, commit, close):
  see ERNW's notes; not used here.

## The type model

The XML lists packages, namespaces and in them:

- `Enumeration` (`base` type, constants),
- `AttributeSet` – an interface with typed attributes,
- `ObjectType` – `Base` types, the attribute sets it `Implements`, `Relation`s,
- `Structure` and `Array`.

Type and relation ids in the file refer to the `id` attributes in this XML.
Ids differ between TIA versions, so everything is looked up by name.

### Which attributes are stored

Per attribute set, an attribute is stored unless it is `constant` or
`intrinsic`. An `<Implements>` element of an object type can restate an
attribute with `constant="true"` or `constant="false"`, and types inherit from
several bases, so the question is whose statement counts:

1. List the type and its bases depth first, bases in the order they are
   declared.
2. Where a type appears more than once, keep only its **last** position. A
   base then always comes after every type that derives from it.
3. Go through that list. The first `<Implements>` that states `constant` for
   the attribute decides. An `<Attribute>` without `constant` decides nothing.
4. If no type states it, the attribute set's own `constant` applies.
5. `intrinsic` attributes are never stored.

Attributes whose type is an object type are stored as relations and take no
space in the segment.

**Verified** in two independent ways (`tools/check_storage_rule.py`):

- The V13 sample carries `StorageMetaInfoXML` documents with the resolved
  result. The rule gives the same answer for every attribute of all 2 629
  object types listed there.
- With the rule, the fixed part of a segment plus the strings and blobs it
  points to cover the segment exactly, with no gap and no overlap, in 48 234
  of 48 236 segments that hold no nested structure (the four public samples,
  `s07_second`, `s08_program`, `s09_security`, `s10_connections`, the two
  `s11_blocks` files and `s12_constants`). The
  two others are one `HmiAuditTrailLogData` object each in the V15.1 and V19
  samples; why they differ is **unknown**.

Simpler rules ("first statement met in a depth-first walk", breadth first,
"any `false` wins") each fail one of the two checks, and a C3 linearisation
does not exist for some of the hierarchies. Version
0.2 of `tiaconv` used the first of those; it decoded the hardware objects
correctly but shifted the fields of some other types (`ITagAddress`, for
one), so `--objects` output of 0.2 should not be relied on for those.

## Object data

Object data starts with a **slot table** of `slot count` u32 offsets, measured
from the start of the block; 0 means absent.

1. One slot per attribute set, **sorted by the set's qualified name**.
2. A keyed relation list for single-valued relations.
3. A keyed relation list for multi-valued relations.
4. In objects with many relations, further slots (**inferred**: one per
   relation).

So `slot count` is at least the number of attribute sets plus two. For most
objects it is exactly that (**verified**); some types (texts, blocks, the CPU)
have more.

### Attribute-set segment

    u32 length (including this field)
    fixed part: the stored attributes in declaration order
    variable part: strings, blobs

| type | size | stored as |
|---|---|---|
| `xs:boolean`, `xs:unsignedByte` | 1 | value |
| `xs:short`, `xs:ushort`, `pe:CharT` | 2 | value |
| `xs:int`, `xs:uint`, `xs:float` | 4 | value |
| `xs:long`, `xs:ulong`, `xs:double`, `xs:dateTime` | 8 | value |
| `pe:GuidT` | 16 | value |
| enumeration | size of its base | value |
| `xs:string` | 4 | offset from the segment start to: varint length **counting itself**, then bytes (UTF-8) |
| `pe:BlobT`, `pe:XmlT`, `pe:MapT` | 4 | offset to length-prefixed bytes |
| `pe:CoreTextAttributeT` | 4 | offset to a text in several languages, see below |
| structure, array | 4 | offset to the structure or array, see below |

### Structures and arrays

A type the model declares as `<Structure>` with `<Element>` children is
stored like a small segment of its own, at the offset the attribute holds:

    size (including this field): u32 in the newer layout, u16 in the older
    one field per element, in the order of the type model
    what the fields point to

Fields have the sizes of the table above. All offsets are **from the size
field of the structure**; 0 means the value is not there.

| element | field | points to |
|---|---|---|
| string | u32 offset | varint length counting itself, bytes (an empty string has an offset and the length 1) |
| blob | u32 offset | the blob **without a length in front**: its kind byte and what follows (see Large values). The blob says itself how long it is |
| structure | u32 offset | the structure, starting with its own size |
| array | u32 offset | the array, see below |

An `<Array type="...">`:

    u32 size (including this field)
    u32 count
    plain values:        the values, one after the other
    strings, structures: count x u32 offset from the size field of the array,
                         then the strings or structures

The same holds for a structure or array that is the value of an expando
attribute; it sits in the variable data of the expando segment like a
string.

`tiaconv` reads a value this way only when everything fits exactly: the
fields and what they point to cover the structure without gap or overlap,
and the elements cover the array. In seven project files of both layouts
that holds for all but two values: 14 472 structures of 40 kinds and 7 640
arrays, 1 651 of them of plain values and 580 of strings (**verified** in
that sense). The two exceptions are the same attribute in two files, a list
of earlier passwords that the project keeps in protected form; they stay
unread, as does anything else that does not fit.

### Text in several languages

Comments and other user texts (`pe:CoreTextAttributeT`):

    varint length (counting itself)
    u32 total size        from here to the end, spare room included
    u32 used size         from the total-size field to the end of the last text
    u32 0xFFFFFFFF
    u32 count
    count x u16 language  Windows language id: 0x0409 en-US, 0x0407 de-DE;
                          0xFFFF = the text without a language
    count x u32 offset    from the total-size field
    at each offset: u32 length, UTF-8 bytes

**Verified**: all 3 861 such values in five newer-layout files (the V15.1,
V16 and V19 samples, `s07_second`, `s08_program`) parse this way, with the
last text ending exactly at the used size. A value without text has count 0.
In the values looked at, the entry `0xFFFF` repeats the text of one of the
languages; showing it gives the comments of the V19 sample exactly as its
author's source export has them. The bytes between the used and the total
size are leftovers.

The V13 sample stores no value of this type: there, texts are separate
`CoreText` objects reached by relations (not decoded).

### Large values (blobs)

The bytes of a `pe:BlobT` value, after the length prefix, start with a kind
byte:

    kind 4:   varint length, bytes

    kind 0 or 1:
      u64 total size
      u32 unknown
      u16 page size
      u32 page count
      varint n, n x u32     bitmap, one bit per page: page is present
      pages                 kind 0: page-size bytes each (the last one shorter)
                            kind 1: varint length, zlib stream of one page

Pages whose bit is clear are not stored and read as zeros (**verified**: every
interface document of the samples decodes to well-formed XML this way; other
kinds have not been seen there).

### Expando segment

Sets declared `expando` hold attributes that are named at run time.

    u32 used length
    u32 capacity
    u24 count, u8 flags         flags 0x01 seen when space is reserved before the data
    older layout: u32 object type id
    newer layout: u8 kind, u8 unknown, u32 (kind 1: offset of the slots; kind 2: unknown)

    kind 2, and the older layout:
      count x u32 key           -> name and type via the expando key table
      count x u32 slot
    kind 1:
      count x u32 value type id
      count x u32 offset of the attribute name (UTF-16, zero-terminated, stored here)
      names
      count x u32 slot          at the offset given in the header
    variable data               starts right after the slots

A slot holds the value itself for fixed-size types of up to 4 bytes; otherwise
an offset into the variable data, `0xFFFFFFFF` for "no value" (**verified**:
no unknown key and no decode failure in any sample).

Kind 1 is used by one attribute set, `ICommentsExpandoAttributeSet`, which
holds the comments of the members of a block: the attribute *name* is the
member's ID path (`51`, `55:52`) and the value a text in several languages.
Version 0.3 read such a segment as kind 2 and reported an unknown key.

### Relation lists

Keyed list (the first two relation slots):

    u16 size, u16 count
    count x { u32 relation id, u32 target type id, u64 target object id }

Further slots hold lists marked by `0x7FFFFFFF`:

    u32 size, u32 0x7FFFFFFF, u32 count
    count x { u32 target type id, u64 target object id }

Which relation such a list belongs to is **unknown** in general. Where the
relations of an object have different target types they can be told apart by
those (the parts of a block's reference table, below). A list can have empty
places (target type and id zero): some relations are indexed by position.
For the inventory the question does not matter: every child names its parent
in its keyed single-valued list.
For data blocks it could matter in large projects: if the list of interface
parts of a block were moved to such a slot, `tiaconv` would report the nested
members as not followed. That has not happened in the samples.
One more list form (`u32 size, u32 count, u32 count`, 12-byte entries) appears
in the CPU object and is not read.

## Hardware and network

All in the `Siemens.Simatic.HwConfiguration.Model` and
`Siemens.Automation.DomainModel` namespaces.

| what | object type | where |
|---|---|---|
| project | `ProjectData` | `ICoreAttributes`: Name, Author, CreationTime, ModifiedTime, LastModifiedBy |
| station / device | derives from `BaseDeviceData` | `ICoreAttributes.Name`, `Subtype` |
| rack, module, CPU, interface, port | derives from `BaseDeviceItemData` | `IDeviceItemData`: OrderNumber, FwVersion, InvariantTypeName, PositionNumber, DeviceItemType |
| network node | `NodeData` | `INodeData`: NodeID, NodeType; addresses in expando attributes |
| subnet | `SubnetData` | `ICoreAttributes.Name`, `ISubnetData.NetType` |

Relations used:

- `BaseDeviceItemData.Parent` – the device, or the enclosing item,
- `BaseDeviceItemData.Container` – what the item is plugged into (rack, CPU),
- `NodeData.DeviceItem` – the interface a node belongs to,
- `NodeData.Subnet` – the subnet a node is attached to,
- `CoreObject.Environment` – what the object belongs to. A device of the
  project has the project (`ProjectData`) here; the copies kept for library
  types and versions have something else.
- `BaseDeviceData.ParentProject` – the project, for PLC stations. A
  distributed IO station does **not** have it: it has
  `FolderElementData.AggregatingFolder` instead, pointing at the folder TIA
  Portal shows as "Ungrouped devices" (`AdditionalDevicesFolder`). Testing
  `ParentProject` alone therefore misses every IO station; `tiaconv` did
  that up to version 0.5.0.

Node expando attributes. The V21 test projects change one setting per step,
which **verifies** the address, mask, router and PROFINET name rows; the rest
is **inferred**.

| attribute | type | meaning |
|---|---|---|
| `NodeIPAddress`, `NodeIPSubnetMask`, `NodeIPDefaultRouterAddress` | `xs:long` | IPv4 address as a number, 192.168.0.1 = `0xC0A80001` |
| `NodeIPDefaultRouterAddressUsed` | bool | "use router"; the router address keeps a default (192.168.0.1) while this is false |
| `NodeIPProtocolUsed` | bool | |
| `NodeIPAddresseSetByUser` | bool | V21: false on a new CPU, true once an address is typed. The V13 sample has it false on 192.168.1.2, so not reliable there |
| `NodeGetsAddressAutomatically` | bool | V21: true on a new CPU, false once an address is typed. Not "address set at the device" |
| `NodeIPConfiguration` | int | 0 in all projects but one; 3 in the V16 sample whose documentation says the PLC really uses another address (`PnPnIpSuiteViaOtherPath` is true there too) |
| `PnPnNoSAutoGenerate` | bool | "generate PROFINET device name automatically" |
| `PnNameOfStation` | string | the PROFINET device name when the automatic option is off. While it is on, the value is **not updated**: it stays `plc_1` after the PLC is renamed |
| `NodeMacAddress` | `xs:long` | `08-00-06-01-00-0x` in every sample: a placeholder, not the real MAC |
| `NodePBMpiAddress` | int | PROFIBUS / MPI station address |

`INodeData.NodeType` is 3 on Ethernet nodes and 2 on MPI/PROFIBUS nodes
(**inferred**).

`DeviceItemType` is a bit field (**inferred** from the samples): `0x1` rack,
`0x2` module, `0x4` submodule, `0x8` set on CPUs, `0x4000` port, `0x80000`
bus adapter.

## IO systems, ports and connections

All of this is **verified** with `tests/fixtures/s10_connections`, one action
per save, unless marked otherwise.

### IO systems

An IO system is a `MastersystemData` object: name "PROFINET IO-System",
`IDeviceItemData.ConfigObjectTypeName` = `IOSystem_PROFINET`, expando
`PositionNumber` = the number TIA Portal shows (100).

| relation | target |
|---|---|
| `MastersystemData.Master` | the controller's item named `IOController_PROFINET`; its `Parent` chain leads to the interface and the CPU |
| `MastersystemData.HeadModules` | one `IODeviceModuleData` per assigned IO device; its `Parent` is the interface item of the device, then the head module, then the station |
| `MastersystemData.Subnet` | the subnet |

- Assigning a station to a controller creates the system (if the controller
  has none yet) and the head module entry, and attaches the station to the
  subnet. TIA Portal picked the address 192.168.77.1 for the first device,
  the router address of another node: it does not check that.
- Reassigning a station moves its entry to the other system's
  `HeadModules`. The system it left **stays, without devices**.
- An unassigned station has no entry anywhere and no subnet.

PROFIBUS master systems are presumably the same object type with another
`ConfigObjectTypeName`; **not checked**.

### Port connections

A cable in the topology view is the relation `DeviceItemBaseData.PortToPorts`
on both ports, each naming the other. A port names its interface with
`DeviceItemBaseData.Interface`. The label is the port's
`IDeviceItemData.InvariantTypeName` (`X1 P1`; `X1 P1R`, `X1 P2R` on an
ET 200SP bus adapter).

### Connections

Each end of a configured connection is an object that derives from
`ConnectionPointData`:

| | object type | `ConfigObjectTypeName` |
|---|---|---|
| S7 connection | `BlockConnectionPointData` | `S7ConnectionPoint` |
| HMI connection | `HmiConnectionPointData` | `HmiConnectionPoint` |

| relation | target |
|---|---|
| `Parent`, `Target` | the CPU the end belongs to (the HMI application on a panel) |
| `ConnectionPointData.Conn2Nodes` | the local node (`NodeData`) |
| `ConnectionPointData.Conn2Conn` | the other end's object, when the partner has one (S7 connection between two PLCs) |
| `ConnectionPointData.Conn2RemoteNodes` | the partner's node, when the partner is in the project but has no object of its own (HMI connection) |
| `ConnectionPointData.Conn2RemoteTargets` | the partner's CPU, same case |

| expando attribute | meaning |
|---|---|
| `ConnId` | local ID as a number; TIA Portal shows it in hexadecimal (256 is `100`) |
| `ConnS7TcpIp` | S7 over TCP/IP |
| `ConnOneWay` | "one-way" |
| `ConnEstablishment` | this end sets the connection up |
| `ConnRemoteAddress` | the address typed for an unspecified partner |
| `ConnRemoteEndPointName` | name of the partner; a row of 25 characters `1` on a connection to an unspecified partner, written by TIA Portal itself |

- An S7 connection between two PLCs of the project is **two objects**, one
  per PLC, pointing at each other with `Conn2Conn`; both have the same name
  and `ConnId`, and `ConnEstablishment` is true on one of them. Deleting the
  connection deletes both.
- A connection to an unspecified partner is one object with
  `ConnRemoteAddress` and none of the three partner relations.
- An HMI connection is one object on the panel's side with the PLC in
  `Conn2RemoteTargets` and `Conn2RemoteNodes`. **Seen in the V15.1 and V19 samples only**, where it matches the
  two devices and addresses of each project; no test project has one
  (an HMI device needs a WinCC licence in TIA Portal).

Not looked at: the TSAP attributes (`ConnS7TsapId`, `ConnRemoteTsapId`,
`ConnRemoteTsap`, ...), `ConnCharacteristics`, connections of other types (TCP, ISO-on-TCP,
UDP configured in the network view), routed connections, connections over
PROFIBUS or MPI.

## Controller security settings

All of these are expando attributes, and all are **stored only once changed
from the default**: a CPU whose settings were never touched has none of them.
A setting that was switched on and off again stays, as `false`.

The CPU itself is the `S7ControllerTargetData` object; the items below it
(relation `Parent`) are plain `DeviceItemData` objects.

| attribute | on | values |
|---|---|---|
| `ProtectionLevel` | item named like the PLC | access level, see below |
| `EnablePutGetConnections` | same item | bool: "Permit access with PUT/GET communication from remote partner" |
| `AccessControlAtRuntime` | same item | 0 / 1: access control disabled / enabled (current CPUs) |
| `EnableLegacyAccessControlViaAccessLevel` | same item | bool: "Use access control via access levels" |
| `OmsCommunicationMode` | same item | 0 = legacy PG/PC and HMI communication permitted; absent = only secure |
| `ProtectPlcConfiguration` | same item | bool: "Protect confidential PLC configuration data" |
| `IsMasterSecretConfigured` | same item | bool: a password for that protection is set |
| `WebServerActive` | the CPU | bool: "Activate web server on this module" |
| `WebServerSSLOnly` | the CPU | bool: "Permit access only with HTTPS" |
| `EnableWebServerAccess4IE` | interface item | bool: "Enable Web server via IP address of this interface" |
| `TimeSyncRole` | interface item | 2 = time synchronisation via NTP |
| `TimeSyncNtpServer1` .. | interface item | server address, as text |
| `OpcUaEnableServer` | item `OPC UA_1` | bool |
| `EnableDisplayProtection` | item `CPU display_1` | bool |

`ProtectionLevel`:

| value | S7-1500 (firmware V1.8, V4.1), S7-1200 (firmware V4.7) | S7-1200 (firmware V2.2) |
|---|---|---|
| absent, 1 | Full access (no protection) | No protection |
| 2 | Read access | Write protection |
| 3 | HMI access | Write/read protection |
| 4 | No access (complete protection) | – |

Everything in these two tables is **verified** with `s09_security`: each row
was changed in TIA Portal V21 in a save of its own (see
`tests/fixtures/README.md`). The S7-1200 with firmware V4.7 is from
`s10_connections`. Two things TIA Portal does by itself showed up in
`s09_security`: choosing "No access" switches PUT/GET off in the same save, and
activating the web server on the module ticks the interface box.

Also seen, not interpreted: `OmsCertificateId`, `LastLoadedOmsCertificateId`,
`ServerCertificateId`, `AccountLockedFor`, `NoOfFailedLoginAttempts`,
`TimeBetweenFailedLoginAttempts`, `EnableLockUserAccountAtRunTime`, the web
server user containers and `PkiContainerCache` on the CPU.

### CPUs with user management

A CPU with user management has one `DeviceFunctionRightSet` object
(namespace `Siemens.Automation.Umac.Model.AccessControl.Administration`)
whose relation `DeviceFunctionRightSetParent` names the CPU, and one
`SystemDeviceFunctionRightProxy` per right the CPU knows
(`PlcDeviceFunctionRights.ProtectionLevelFullAccess`, `...ReadAccess`,
`...HMIAccess`, `...WebReadTags`, ...). The set's
`DeviceFunctionRightSetId` names the firmware generation:

| CPU | `DeviceFunctionRightSetId` |
|---|---|
| S7-1500 firmware V3.1 (V19 sample) | `PlcDeviceFunctionRights.UmacFunctionRights_S71500V31` |
| S7-1500 firmware V4.1 | `PlcDeviceFunctionRights.UmacFunctionRights_S71500V41` |
| S7-1200 firmware V4.7 | `PlcDeviceFunctionRights.S71200V4_7_1` |

The S7-1500 with firmware V1.8 and the S7-1200 with firmware V2.2 have
neither. An S7-1500 with firmware V2.6 (V15.1 sample) has a small set,
`PlcDeviceFunctionRights.S71500V2_6`, with rights for OPC UA users only. So
the sign that users and roles decide about access is not the set but a right
named `...ProtectionLevelFullAccess` whose `DeviceFunctionRightParent` is the
CPU; `tiaconv` uses that.

On these CPUs (**verified** in TIA Portal V21 on firmware V4.1 and V4.7):

- `AccessControlAtRuntime` absent means access control is **enabled**; that
  is what a new CPU shows. The V19 sample stores 0, disabled.
- The access level cannot be chosen. TIA Portal shows it greyed out and
  says that the level without a password "is configured via the function
  rights of the Anonymous user". With `EnableLegacyAccessControlViaAccessLevel`
  the password column of the table becomes editable, the level does not.
- A new CPU stores `ProtectionLevel` 4 and TIA Portal shows "No access
  (complete protection)".
- **`ProtectionLevel` follows the rights of the Anonymous user** (verified on
  the S7-1200 with firmware V4.7, one save each): a role with the right
  "Read access" on the CPU changes nothing; assigning that role to the
  Anonymous user sets the level to 2 and TIA Portal shows "Read access";
  changing the role's right to "HMI access" gives 3, to "Full access" 1.

How this is stored, as far as it is visible without reading anything
protected:

| object | relations |
|---|---|
| `CustomRole` | `RoleToDevice` → the CPU; `AssignedDeviceFunctionRight` → the right; `RoleToAssociation` → the association below |
| `SystemDeviceFunctionRightProxy` (the right) | `DeviceFunctionRightAssignedToRole` → the role |
| `CustomProjectUser` | `UserRoles` → the association |
| `RoleToUserAssociation` | `Parent` → the user; `AssociationToRole` → the role; `RoleAndUserAssociationToDevice` → the CPU |

The names of roles and users, and everything else about them, are attributes
in protected form (`NameEncrypted`, `PasswordEncrypted`,
`AssignedEngineeringFunctionRightsEncrypted`, ...).

The user management of the project hangs below one `UmacRootData` object:
the password policy, the system roles (`SystemRoleProxy`, with `ExternalId`
`PLCAdministrator`, `PLCFAdministrator`, `PLCOperator`), the roles and the
users. `tiaconv` reads none of it and does not work on that protection.

### Passwords

`EncryptedPassword1` .. `3` and `EncryptedPasswordFailsafe` (one
`EncryptedPassword` on the S7-1200 with firmware V2.2,
`EncryptedDisplayPassword` on the display) are blobs that exist whether or
not a password is set; their size depends on the firmware. Setting the access
level back to "Full access" writes one new value into the three slots that
differs from the value they had before any password was set, so an empty slot
cannot be told from a used one by looking at it. Which slot belongs to which
access level is **unknown** (the observations on two CPUs contradict each
other). Text attributes such as `DisplayPassword` hold one placeholder
character per character of the password, not the password.

Protecting a block adds to it (V21): `ICoreAttributes.Password`, and the
expando attributes `ProtectionSalt`, `ProtectionIV`, `VerificationTag`,
`ProtectionVersionId`, `KHP_BLOCK_MODEL_VERSION` for know-how protection;
`WriteProtectionPassword`, `WriteProtectionSalt`, `WriteProtectionIV`,
`WriteProtectionData` for write protection. Removing the know-how protection
resets `Protection` and removes the version attributes.

`tiaconv` reads none of this. Blobs appear in `--objects` by size only, and
text in any attribute with "password", "Salt", "ProtectionIV" or
"VerificationTag" in its name is replaced by `{"redacted": true}`.

## Save history

`CorePersistenceInfo` (one per project) has the attribute set
`IPersistenceInfoAttributes` with two arrays of structures:

- `History`, an array of `HistoryEvent`: `CurrentVersion`, `Date` (UTC),
  `FeedbackId`, `LogFile`, `OldVersion`, `ServiceId`. TIA Portal's own list
  of what happened to the project. Seen: `ProjectHistoryUserCreated`
  (`CurrentVersion` "V21", "V19 Update 3", "V16", "V15.1", "V11 SP2 Update
  2"), and in the V13 sample `ProjectSave` ("V13") followed five seconds
  later by `ProjectHistoryConverted` (`CurrentVersion` "V13", `OldVersion`
  "11.0.0.0", `LogFile` "ConversionLog.xml"). Ordinary saves add nothing to
  it. `FeedbackId` is the key of a message text; the wording `tiaconv`
  prints for the three is its own.
- `ProjectSavePoint`, an array of `TiaPersistenceSavePoint`
  (`AdditionalInfo`, `RelativeDirectoryPath`, `SavePointId`): the other
  folders of the project (`IM\SearchIndex`, `XRef`, `Vci`, ...) with an id
  each.

The object is written in every save except the first of a file, also in a
save without a change (**verified**: all 80 + 17 saves of the two longest
test files; the four saves made without a change wrote this object and
nothing else).

**When** a save was made:

- Older layout: the commit record of the save carries the time (see
  [Blocks](#blocks)). It has no time zone; in the V13 sample three of the
  four are within a second after the UTC "modified" time of the project
  object, so they are UTC.
- Newer layout: nothing in the save object. What a save does hold is
  `ICoreAttributes.ModifiedTime` of every object it wrote, and the latest of
  them is a lower bound: `tiaconv` reports that. In 69 of the 76 saves of
  the longest test file that have a time at all, the latest is the project
  object (`ProjectData`), seconds to minutes after the other objects, which
  looks like TIA Portal setting it when it saves. For the last save of one
  test project the file time on disk is known: 20:44:27.140, the project
  object has 20:44:27.109 (**one observation**). But it is not a rule: four
  saves did not write the project object at all (a PLC renamed, a router
  address, a PROFINET name, a signal module added), and in three the project
  object is older than other objects of the same save, by 4 seconds to 6
  minutes (two PLCs added, and a session in which tags were entered).
- `ICoreAttributes.LastModifiedBy` is the user name. Not every object has
  one. In the V13 sample the project object changes from one name to
  another between saves 2 and 3.

**What** a save changed: `tiaconv --history` builds its whole report once
per save and compares. The test projects document 85 saves with the action
taken in TIA Portal before each, and for each the difference is that action
(`tests/fixtures/README.md`, `tests/fixtures_test.cpp`). Things learned from
that:

- TIA Portal generates an instance data block anew when its function block
  changes: the old object is deleted and a new one with the same name and
  number is written (V13 sample).
- Reassigning an IO device to another controller deletes its hardware
  identifiers on the old controller and creates new objects with the same
  values on the new one.
- Entering a password for an access level writes the CPU item and nothing
  that `tiaconv` reports; creating a role writes `CustomRole`,
  `UmacRootData` and a `SystemDeviceFunctionRightProxy`.

## HMI tags

**Verified** on a V21 project made for the purpose (`s13_hmi`, a KTP400
Basic panel, one action per save) against TIA Portal's own export of the
tag table and screenshots, and on the V19 sample (a Comfort panel, 100 tags)
against TIA Portal V21's export of that project. The V15.1 sample stores the
same.

A tag of an HMI device is an `HmiTagData` object. The tags the device makes
itself (`@CurrentUser`) are `HmiSystemTagData`, the members of a tag of a
structured type `HmiStructureMemberTagData`, reached through the relation
`Members`.

| | |
|---|---|
| name, comment | `ICoreAttributes` |
| data type | `IStructureItem.DisplayTypeName` (`Real`, `String[20]`, the name of a PLC data type). A `String` shows as `String[254]` in a member |
| HMI device | relation `Target` to the runtime item (`HmiApplicationData`, "HMI_RT_1"), whose parent is the device |
| tag table | relation `TagTable` to `HmiTagTableData` |
| connection | relation `IHmiTagAttributes_Connection` to `HmiConnectionData`; none for an internal tag |
| acquisition cycle | relation `IHmiTagAttributes_AcquisitionCycle` to `HmiCycleData`, whose name is the cycle ("1 s", "500 ms") |
| acquisition mode | `IHmiTagAttributes.AcquisitionTriggerMode`: `Visible` is the default, "Cyclic in operation" in the editor and in the export. `Continuous` is what the editor calls "Cyclic continuous" and the export "Continuous". The model also has a `CyclicContinuous`, which is **not** what that choice stores, and `OnDemand`, `OnChange` and others not seen |
| start value | `IHmiTagStructureAttributes.StartValue` |
| limits, scaling, coding | `IHmiTagStructureAttributes`; the defaults read 10 / 0 / 100 / 0 for the scaling ends, as in the export. Never seen changed |
| address | `ITagAddress.LogicalAddress` and `AddressMode`, see below |

In the file the tags of a device are in the order of their object ids, which
is the order of TIA Portal's export.

`HmiConnectionData` is the panel's view of a connection. Its relation
`ConnectionPoint` leads to the `HmiConnectionPointData` object of the
hardware configuration (see Connections), and through that to the PLC that
the tag table shows as "PLC name". Its attributes `PhysicValue` and
`ProtocolValue` are arrays of name and value pairs with the settings of the
driver. One of the names is `Password`; `tiaconv` does not write the value
that goes with it.

**Three kinds of access**, as the editor and the export name them:

- internal tag: no connection.
- absolute access: a connection and an address (`LogicalAddress`
  `%MW100`, `AddressMode` `Fixed`), no link (see next).
- symbolic access: a connection and a link to a PLC tag.

**The PLC tag** of a symbolic tag is not on the tag. Each tag table has a
`ScopedLinkMaintainerData` object (relation `InverseScopedLinkMaintainer`
to the table, `InverseConsumer` from each tag that has a link) whose
attribute `IScopedLinkMaintainerData.Links` is an array of
`LinkInformation`:

| element | |
|---|---|
| `Handle.Index` | a number; the tag has the same one in its expando attribute `LinkHandle.Index` |
| `QuotedNamePath` | the PLC tag as TIA Portal writes it: `DB_Standard.my_int`, `MyInt` |
| `NamePath` | the members below the block, an array of strings; empty for a PLC tag |
| `ConsumerIndex`, `ProviderIndex` | positions in the object's relation lists `Consumer` (the HMI tags) and `Provider` (a `DataBlockData` or the PLC tag itself, `EAMTZTagData`). Those lists keep empty entries so that the positions stay fixed |
| `IsConnected` | false once the PLC tag is gone: deleting a PLC tag under its HMI tag sets it, and TIA Portal then shows the PLC tag on a red background |
| `Coid` | a blob of 39 bytes for a block member, 29 for a PLC tag |

`tiaconv` takes the PLC from the connection and looks the name up in that
PLC's tags and data blocks; it does not use `Provider`, because in a link
object with many links the relation lists are of the kind that carries no
relation id (see Relation lists). For every tag with an intact link in the
three projects (4 + 100 + 3) the name leads to a tag or member there, and
the data type of the HMI tag is the data type found (a PLC data type is
written in quotes on the PLC side and without on the HMI side).

When a PLC tag is deleted that an HMI tag still names, TIA Portal writes a
new object of the tag's type (`EAMTZTagData`) with the name and nothing
else, and makes it the `Provider`. It hangs under the tag table by the
relation `TRefParent` ("textual reference") instead of `TagTable`. Objects
with a `TRefParent` are not tags or blocks of the project; the V19 sample
has four code blocks of that kind.

The tag also has an expando attribute `TypeSafeAddress` (`CRC`, `Rid`, and
`LidPath`, the member IDs from the block down with flags in the top bits);
not used.

**The address.** TIA Portal shows and exports an address for absolute
access only. For symbolic access `LogicalAddress` still holds one, and it is
right where the PLC tag has an address: `%DB1.DBW264` for a member at offset
264 of a block with standard access, `%MW20` for a PLC tag, `%DB1.DBX0.0`
for a structure at the start of the block (V21); the four tags of the V15.1
sample likewise. For members of optimized blocks it is empty with
`AddressMode` `Invalid` in the V21 project, and in the V19 sample often
left over from something else (`%DB1.DBD0` for a Bool, mode `Fixed`).
`tiaconv` therefore reports an address for a symbolic tag only when the PLC
side says the tag or member has one. After the PLC tag was deleted the HMI
tag still stores `%MW20`.

**Not seen**: limits, linear scaling, multiplexed tags, acquisition modes
other than the two above, HMI tags in the older layout, PC runtimes, WinCC
Unified devices.

## Blocks

Everything TIA Portal lists under "Program blocks" and "PLC data types" is an
object with the attribute set `IGeneralBlockSourceData` (`IGeneralBlockData`
in V13):

| object type | what |
|---|---|
| `CodeBlockData` | OB, FB, FC; also the system functions in use (SFB, SFC) |
| `DataBlockData`, `TechnologicalDataBlockData` | DB |
| `UserTypeData` | PLC data type (UDT) |
| `SystemDatatypeData` | system data type in use (SDT) |

Relations `Target` (the CPU) and `Environment` (the project; copies kept for
library types have something else) as for tags.

| attribute | meaning |
|---|---|
| `ICoreAttributes.Name`, `.Subtype` | name; `OB.ProgramCycle`, `OB.CyclicInterrupt`, `FB`, `DB`, `FB.TO.PID.Compact.Compact_3.0` |
| `ICoreAttributes.Comment` | the block **title** (multilingual text) |
| relation `GeneralBlockSourceData.BlockComment` → `CoreText` object, `ICoreTextRepository.Text` | the block **comment**; the object exists once a comment was entered |
| `IGeneralBlockSourceData.BlockType` | `OB`, `FB`, `FC`, `DB`, `UDT`, `SFB`, `SFC`, `SDT` (V13: `IGeneralBlockData.DosType`) |
| `.Number`, `.AutoNumber` | block number; whether TIA Portal assigns it |
| `.BlockLanguage` | `LAD_CLASSIC`, `FBD_CLASSIC`, `STL`, `SCL`, `GRAPH`, ...; `DB`, `UDT`, `SDT` for what is not code. TIA Portal shows the first two as LAD and FBD |
| `.OnlySymbolicAccess` | optimized block access |
| `.CompileArtifacts` | why the block has to be compiled again: `UpToDate`, or flags such as `Binary` (V13: `IGeneralBlockData.CompileStatus`) |
| `IPlcHeaderData.HeaderAuthor`, `.HeaderFamily`, `.HeaderVersion`, `.HeaderName` | author, family, version and "user-defined ID" of the block properties |
| `ICoreAttributes.Protection` | `NoProtection`, `KnowHowProtection`, `SystemKnowHowProtection` (protected blocks of Siemens libraries); the type model also has `WriteProtection`, not seen in use |
| expando `WriteProtection` | bool: the write protection of a code block |
| `IGeneralBindingData.CopyProtectionMode`, `.CopyProtectionAssignment`, `.CopyProtectionSerialNumber` | copy protection: `BindToPLC` / `BindToSDCard`, `Manual` / `Auto`, the serial number entered (V13: the same names in `IGeneralBlockData`). Stored once it is set |
| `ICoreAttributes.CreationTime`; `ITimestampData.Modified`, `.CodeModified`, `.InterfaceModified` | time stamps, UTC |
| relation `FolderElementData.AggregatingFolder` → `FolderData` | the folder; follow it upwards for the path |
| relation `Parent4LoadableBinaryData.LoadableBinaries` → `LoadablePlus...BlockData` | result of the last compilation: `IGeneralBlockResultData.CompileTime`, `.DownloadTime`, `.LoadMemoryRequired`, `.WorkMemoryRequired` (V13: the times are on the block, the sizes in `ILoadableObjectOmspData`) |
| expando `DownloadHistory` | `FB3-638682897237619877;FB3-...;FB?-0`: one entry per download, newest first, at most 20 seen: the block's address then, and the time as .NET ticks. `FB?-0` = never. The newest entry equals `DownloadTime` to the millisecond |
| relation `Parent4SourceData.Sources` → `CompileUnitData` | one per network (one in all for a block written as text); see [Block code](#block-code) |
| expando `IsWriteProtectedInAS` | data block: "Data block write-protected in the device" |
| expando `DBAccessibleFromOPCUA`, `DBAccessibleFromWebserver` | data block: absent while the two "accessible from" boxes are ticked, as on a new block; `false` once unticked. In the V19 sample every data block has the first one as `false` |
| expando `Unlinked`, `NonRetain` | data block: presumably "Only store in load memory" and the retain setting; **not checked** |

`FolderData` has `ICoreAttributes.Subtype` `ProgramBlocksFolder` (Program
blocks), `ProgramBlocksFolder.Subfolder` (a group, named by the user),
`SystemBlocksFolder` and `ProgramResourcesFolder` (System blocks > Program
resources), `ControllerDataTypeFolder` (PLC data types), `SystemDataTypeFolder`,
`TechnologicalParamFolder` (Technology objects).

What a know-how or write protection adds to the block is described under
[Passwords](#passwords): `tiaconv` reads the two settings and nothing else,
and does not read the code of a know-how protected block.

**Verified** with `tests/fixtures/s11_blocks_a` and `s11_blocks`, one action
per save in TIA Portal V21: new FB and OB with type, number, language and
kind of OB; a group and a block moved into it; a number set by hand; title,
comment, author, family, version and user-defined ID; title and comment of a
data block; write protection of a data block in the device; `CompileArtifacts`
going from `UpToDate` to `Binary` on a change and back on compiling; know-how
protection set and removed; write protection; copy protection with a serial
number; the two "accessible from" options of a data block. Independently: title, comment, author, family and version of the
five function blocks of the V19 sample are those of the SCL sources published
with it.

Also compared with TIA Portal at the end of that test: the four time stamps
of a block with its "Time stamps" page (which shows them in local time; the
file has UTC, and `LoadRelevantModified` is the fifth one shown there); the
number of `Sources` with the networks of two OBs (3 and 1); and the memory
sizes with Program info > Resources, for every block of a PLC. For those:

- the load memory TIA Portal lists is the expando `LoadMemorySize` of the
  block. It equals `LoadMemoryRequired` of the compilation result, except
  for a PLC data type, where the result says 0;
- TIA Portal removes `LoadMemorySize` when the block changes and shows "?"
  until it is compiled again. The old compilation result stays;
- "Code work-memory" and "Data work-memory" are `WorkMemoryRequired` of a
  code block and of a data block.

**Not verified:** the download times (no test project was ever downloaded;
in the four public samples they lie within the project's lifetime and the two
places that store them agree). `Modified` and `CodeModified` are not "last
edited": compiling sets them too (seen on an OB that had only to be
recompiled).

## Block code

A code block has one `CompileUnitData` object per network, in the relation
`Sources`; a block written as text (SCL) has one in all. The order of the
list is the order of the networks (V21: a network inserted after the first
is second in the list).

| where | what |
|---|---|
| `ICompileUnitData.ProgrammingLanguage` | `LAD_CLASSIC`, `FBD_CLASSIC`, `STL`, `SCL`; a block can mix them (an SCL network in an FBD block) |
| `ICompileUnitData.RefID` | a number for the network that stays when networks are inserted or deleted (a new network gets the next free number); the reference table (below) names networks by it |
| `ICompileUnitData.Data` | the code: a blob with an XML document in UTF-8 with byte-order mark; empty (a blob of 20 bytes) for an empty network |
| `ICompileUnitData.AdditionalData` | layout of the editor: which boxes are collapsed. Not read |
| `ICoreAttributes.Comment` | the network **title** (text in several languages) |
| relation `CompileUnitComment` → `CoreText`, `ICoreTextRepository.Text` | the network **comment**; the object exists once a comment was entered |
| relation `ElementComments` → `CoreText` | the comments in several languages inside SCL code, by position (below) |
| `ICoreAttributes.Protection`, `.IsKnowHowProtected` | as on the block; `tiaconv` does not read a unit that has either set |

### SCL

    <SCLSource Version="3.3.0.0">
      <Symbols> ... </Symbols>
      <RootStatements Version="3.3.0.0"> ... </RootStatements>
    </SCLSource>

`RootStatements` is the source text as a tree of tokens, in the order of the
text. Writing out every token gives the text back, blanks and line breaks
included:

| element | text |
|---|---|
| `BL`, attribute `NumBLs` (default 1) | that many blanks |
| `NL` | line break |
| `LC TE="..."` | `//` and the text |
| `BC` ... `BCL TE="..."` ... `BCE` | `(*`, the lines of a block comment, `*)` |
| `MLC DictId="n"` | a comment in several languages, `(/*` text `*/)`; the text is the `n`-th target (counted from 1, empty places included) of the unit's relation `ElementComments` |
| `Statement TE="IF"` (also `REGION`, `FOR`, `CASE`, `RETURN`, ...) | the keyword, then its children |
| `Const TE="..."` | the constant as written |
| `SymVa`, `Sub`, `SymDB` with `ODN="..."` | a variable, block or instruction name as written, with `#` or quotes |
| `SymPa ODN="..." FormalName="..."` | the name of a parameter in a call; `ODN=""` with `V="0"` where the source leaves the name out |
| `OpAs` `:=`, `OpPa` `=>`, `FiSt` `;`, `BracO` `(`, `BracC` `)`, `BoxO` `[`, `BoxC` `]`, `Dot` `.`, `Comma` `,`, `Colon` `:` | |
| `OpPl` `+`, `OpMi` `-`, `OpMu` `*`, `OpDi` `/`, `OpG` `>`, `OpL` `<`, `OpE` `=`, `OpU` `<>`, `OpLE` `<=`, `OpGE` `>=`, `OpAND` `AND`, `OpOR` `OR`, `OpNOT` `NOT` | |
| `KwTHEN`, `KwELSE`, `KwELSIF`, `KwENDIF` `END_IF`, `KwTO`, `KwBY`, `KwDO`, `KwENDFOR` `END_FOR`, `KwOF`, `KwENDC` `END_CASE`, `KwEndRegion` `END_REGION` | |
| `Expression`, `Statements`, `Fold`, `FctCa`, `InstCa`, `Param`, `CaseElem`, `CaseRange` | grouping only |

Other keywords and operators exist (`WHILE`, `REPEAT`, `XOR`, `MOD`, `**`,
...): they are **not in the samples**, `tiaconv` writes `{?Name}` for an
element it does not know.

V13 (`Version="1.4"`) has the same tree with three differences: the tokens
carry no names (`ODN`), a line comment has its line break as a child, and an
array element is followed by one more `SymVa SI="VarElem"` that stands for
the element as a whole and has no text. Names come from `Symbols`: each
`Symbol` has a `SymID`, which the tokens name as `SyId`, and either a `Name`
(parameters of instructions, constants), a `BILibName` (an instruction of the
library) or a `RefId` into the block's reference table. After a `Dot` only the
last name of the referenced path is meant. A conversion is the instruction
`CONVERT` with the two types in `Template0="src_type Char"` and
`Template1="dest_type Word"`; the source text for it is `CHAR_TO_WORD`
(in V15.1 and V19 the token has that name in `ODN` next to the same
templates).

**Verified:** in V21 (`s14_code`), the body of an SCL block with `IF`/`ELSE`,
arithmetic, `NOT`, a block comment `(* *)` and a line comment is identical to
the source TIA Portal generated for it. The bodies of the eight SCL blocks of the V19 and V15.1 samples
that have a source file in their repositories come out identical to those
files, 852 lines, compared after the leading tab that TIA Portal puts in
front of every body line of a generated source. That covers `IF`/`ELSIF`/
`ELSE`, `FOR` with `BY`, `CASE`, `REGION`, `RETURN`, calls of functions,
function blocks and multi-instances with and without parameter names, array
and structure access, slices (`.%B0`), line and block comments.
**Not verified:** comments in several languages (`MLC`: the form `(/* */)` is
from memory of TIA Portal's sources, the samples that have such comments
have no source file), and the whole of V13, where the text reads as
plausible SCL and no source exists to compare with.

### STL

    <Statements Version="14.0.0.0">
      <Statement UId="21" TokenProperty="1">
        <Token Kw="1" DispName="A" />
        <OpdAccess NumBLs="1" RefId="1" UId="23" />
      </Statement>
      <Statement UId="22" TokenProperty="1">
        <Token NumBLs="6" Kw="2" DispName="AN" />
        <OpdAccess NumBLs="4" RefId="3" UId="24" />
      </Statement>
      <Statement UId="25" TokenProperty="1"><Token Kw="210" DispName="+I" /></Statement>
      <Statement UId="28" TokenProperty="1" />
    </Statements>

One `Statement` per line. `Token` is the instruction: `DispName` as written,
`Kw` a number for it (1 `A`, 2 `AN`, 9 `=`, 16 `L`, 17 `T`, 210 `+I`).
`OpdAccess` is the operand, an entry of the reference table; a constant
(`L 5`) is an entry too. `NumBLs` is the number of blanks typed in front of
the piece; the editor does not show them, it puts instructions and operands
in two columns (`tiaconv` does the same). A statement without children is an
empty line. Seen in one V21 test project with seven statements, which
agree with the editor and with the STL source TIA Portal generated for the
block (`A "ZZA";`: one blank, and a `;`); labels, jumps, comments and
everything else STL has are **not in the samples**.

### LAD and FBD

    <FlgNet xmlns="http://www.siemens.com/automation/2015/FunctionLadderDiagram"
            Version="14.0.0.2" Lang="LAD_CLASSIC" Routed="true">
      <Parts>
        <Part UId="23" Gate="Contact" />
        <Part UId="27" Gate="Contact"><Negated PinName="operand" /></Part>
        <Part UId="31" Gate="Coil" />
        <ORef UId="24" RefId="1" />
        <ORef UId="28" RefId="2" />
        <ORef UId="32" RefId="3" />
      </Parts>
      <Wires>
        <Wire UId="21"><Powerrail /><PCon UId="23" PinName="in" /></Wire>
        <Wire UId="22"><OCon UId="24" /><PCon UId="23" PinName="operand" /></Wire>
        <Wire UId="25"><PCon UId="23" PinName="out" /><PCon UId="27" PinName="in" /></Wire>
        ...
      </Wires>
    </FlgNet>

This is close to the network format of TIA Portal's XML export (Openness),
with references into the block's reference table where the export writes the
operands out.

| element | what |
|---|---|
| `Part Gate="..."` | a contact, coil or box. Children: `TemplateValue Name="SrcType" Type="Type"` (the data type a box was set to; its text content), `TemplateValue Name="Card" Type="Cardinality"` (number of inputs), `Negated PinName="..."` (the pin is negated: a normally closed contact has its `operand` negated). Attribute `DisableENO` |
| `CRef RefId` | the call of a block. `RefId` names the entry for the call's interface; the children `CodeBlock RefId` and `Instance RefId` the block and its instance |
| `LRef RefId` | an instruction that has an instance (a timer): `RefId` names the instruction, the child `Instance` its instance data block or multi-instance |
| `ORef UId RefId` | an operand: an entry of the reference table. Without `RefId`: a pin nothing was entered for |
| `Wire` | a connection. Its first child is where it comes from, the others where it leads: `Powerrail`, `OCon UId` (an operand), `PCon UId PinName` (a pin of a part), `Openbranch` |

So `<OCon/><PCon operand/>` puts an operand on an input, and
`<PCon out1/><OCon/>` an output on an operand; an in-out parameter of a call
is written the first way.

Gates and their pins seen: `Contact` (`in`, `operand`, `out`), `Coil`,
`SCoil`, `RCoil` (the same three), `O` and `A` (`in1`, `in2`, ..., `out`),
`Eq`, `Ne` and the other comparisons (`pre` in LAD, `in1`, `in2`, `out`),
`PBox` (`in`, `bit`, `out`; drawn as P_TRIG), `Move` (`en`, `in`, `out1`,
`eno`), `S_Move` (`en`, `in`, `out`). Calls and instructions have `en`,
`eno` and the parameter names as pins. The parts are stored in the order
they have on the screen, top to bottom and left to right (compared with a
screenshot of a network of 17 parts in the V15.1 sample's repository).

V13 has `Version="12.0.0.0"`, no namespace and an extra `<Labels />`; the
sample has only empty networks in LAD.

### The reference table

The XML of a network names operands and blocks by number. The names are in a
table that the block keeps of everything its code refers to, which is also
what TIA Portal's cross-reference list is made from.

V14 and later: relation `CoreObject2IdentContainer` of the block →
`IdentContainerData`, relation `IdentParts` → `IdentPartData` objects, one per
kind of entry. `IIdentPartData.PayLoad` is a blob with an XML document:

    <IdentXmlPart xmlns="http://schemas.siemens.com/Simatic/ES/14/IdentManager/IdentXmlPart.xsd">
      <GlobalAccess>
        <ID N="" S="Global" RID="12" IS="1">
          <CS><C NID="2" UID="46" OID="9" AK="Write" /></CS>
        </ID>
        <OD S="1"><TD TDF="OST" T="Bool:33554433:Bool" /></OD>
        <SSD S="S" AO="0" MID="51">
          <AOS>
            <AO N="control" RIDI="" BO="0" BS="16" />
            <AO N="Q_resetNewIdFlag" RIDI="" BO="0" BS="1" />
          </AOS>
        </SSD>
      </GlobalAccess>
      ...
    </IdentXmlPart>

| | |
|---|---|
| element name | the kind of entry: `GlobalAccess` (a data block member), `InterfaceAccess` (a parameter or variable of the block itself), `SimpleAccess` (a PLC tag), `LiteralConstant`, `LocalConstant`, `FBBlock`, `FCBlock`, `OBBlock` (a called block), `AufDBBlock` (an instance data block), `DepDBBlock` (a data block that is accessed), `MultInstAccess`, `Instruction` (of the library), `BlockInterfaceInfo` (the parameters of a called block as they were when the call was made), `Expression` |
| `ID/@RID` | the reference number (`RefId` in the code) |
| `ID/@N` | the name, for entries that have one: a tag, a block, a constant, an instruction. TIA Portal keeps it current: in the V19 sample a renamed data block has its new name here and the old one in the type (`OD/TD/@T`) |
| `ID/@S` | `Global`, `Local`, `Constant`, `Instruction`, ... |
| `ID/CS/C` | one use: `NID` the network (`RefID` of its `CompileUnitData`; 0 for the block interface), `UID` the element of the network, `AK` the kind of access (`Write`, `RW`, `Call`, `InstanceDB`, `Multiinstance`, `ArrayBoundary`, `None`; **absent for a reading access**), `XH="1"` for a use the cross-reference list does not show |
| `OD/TD/@T` | the data type as `kind:number:name` |
| `SSD/AOS/AO` | the path of an access, one `AO` per name: `"control".Q_resetNewIdFlag`. `RIDI` names the entries that are the indices of an array element (`"control".I_uid[3]`: `RIDI` of `I_uid` names the entry of the constant 3) |
| `SSD/@AM` | a part of the variable: `b0` for `.%B0` |
| `CD/CB/@SV` | a constant as written, with characters that are not letters written as `_xHHHH_` (`T_x0023_5s` is `T#5s`) |

**The names in the parts can be out of date.** TIA Portal writes them when the
block is opened or compiled. V21: after a tag was renamed and the project
saved without compiling, the block open in the editor had the new name, the
three others still the old one, in the parts and in the SCL tokens (`ODN`).
The current object is reached through links. The container has, besides
`PayLoad`, a document `FilcMetaPayload`:

    <FILCMetaInfo xmlns="http://schemas.siemens.com/Simatic/ES/14/IdentManager/ICFilcMetaPayload.xsd">
      <FILC RelId="2">
        <Idx Value="0"><Id RefId="1" Type="17" /></Idx>
        <Idx Value="2"><Id RefId="3" Type="17" /></Idx>
      </FILC>
      <FILC RelId="3">
        <Idx Value="0"><Id RefId="5" Type="9" /></Idx>
      </FILC>
    </FILCMetaInfo>

Each `Id` says that the entry `RefId` is the object at position `Value` of a
relation list of the container: `Type="17"` a PLC tag, in the relation
`SimpleAccessDataToTagData`; `Type="4"` an instruction, in
`InstructionToInstrProxy`; other types (6, 7, 8, 9: FB, FC, instance and
global data block, presumably) a block or data block, in
`TypeOperandToProgramObj`. The lists keep empty places (target zero) so
that positions stay: in V21 the place of a tag that was no longer used was
empty. `RelId` 2, 3 and 4 went with those three lists in every container
seen; `tiaconv` goes by `Type`. With many entries the relations of the
container are stored as lists without relation ids
([Relation lists](#relation-lists)); `tiaconv` tells them apart by the kind
of object they lead to. For a data block member, `BAD/@BIRID` names the entry
of its data block, whose link gives the current name of the block. A renamed
member of a data block cannot be followed this way.

V13 keeps the same information as objects, one per entry, in the relation
`RelatedIdents` of the block: `InterfaceAccData`, `GlobalAccData`,
`ConstantData`, `FBBlockData`, `AufDBBlockData`, `DepDBBlockData`,
`InstructionData`, ... with `IIdentData.RefId`, `.Name`, `.Scope`,
`IIdentXRefLocations.IdentXRefLocationsArray` (structures with `NetID`, `UID`,
`AccessKind`, `XRefHidden`; here a reading access is `Read`),
`ISimaticStorageData.AccessObj` (the path: structures with `Name` and
`Index`) and `IConstantData.StringValue`. Watch tables use the same kind of
object for the tags they list.

**Verified:** in the V21 test project `s14_code`, the eight networks of an
OB, an FBD network, an FB's network and two STL networks against
screenshots; the places of one tag against TIA Portal's cross-reference list
(seven, in four blocks: the use in an SCL block counts once per network, as
there); calls and data block accesses against the call structure. In the
V15.1 sample, the call in network 3 of `Main` (22
parameters, among them ten array elements) and network 5 (a comparison, an
edge detection, three string moves, a timer, eight coils) agree with the two
screenshots of those networks in the sample's repository, operand by
operand, apart from one contact that was added to the project after the
screenshot was taken. **Not verified:** the kinds of access against TIA
Portal's cross-reference list; a use that is hidden there; entries of kinds
not in the samples (global constants, PLC data types, technology objects).

### Know-how protection

`tiaconv` does not look at the compile units or the reference table of a
block whose `Protection` is `KnowHowProtection` or `SystemKnowHowProtection`.
How protected code is stored was not examined.

A project file keeps the earlier versions of its objects until TIA Portal
rewrites it, so a file can hold the code of a block as it was before the
block was protected.

**Setting know-how protection makes TIA Portal write the file anew.** Seen
twice in V21: right after a save that protected a block, the file had shrunk
to the current version of every object, without the earlier saves, and
written in one go (one save object after the type model, then the whole
project; the next save closes it). Before doing so, TIA Portal keeps a copy
of the old file in the folder `<project>.backup/<date>.<time>/` next to the
project, as `<date>.<time>.zip` with `PEData.plf` and `PEData.idx`. That copy
still holds the block from before its protection. In the test project, the
backup's last save is the one that set the protection. When an earlier state of the project is read (`--save`,
`--history`), a block is therefore not read if its version in that state is
the last protected version in the file or an older one.

## Tags

A PLC tag is an `EAMTZTagData` object (**verified** against TIA Portal's
export of a tag table in the V21 test project `s08_program`; also read in the
V13 and V15.1 samples).

| what | where |
|---|---|
| name | `ICoreAttributes.Name` |
| address | `ITagAddress.LogicalAddress`, as text: `%M1.0`, `%MB1` |
| comment | `ICoreAttributes.Comment`, a text in several languages (V14 and later) |
| data type | `IStructureItem.DisplayTypeName` |
| tag table | relation `TagTableContentData.TagTable` to an `EAMTZTagTableData` |
| PLC | relation `CoreObject.Target` |
| project or library copy | relation `CoreObject.Environment`: a `ProjectData` object for things in the project tree |

`ITagAddress` also has the address in parts (`Part4` area, `Part6` size,
`Part7` byte, `Part8` bit; **inferred**, not used).

## Constants

System constants and user constants are both `SimaticConstantTagData`
objects, in a tag table like tags (relations `TagTable`, `Target`,
`Environment` as above). The number TIA Portal shows next to a tag table
counts them too: `Default tag table [49]` in `s08_program` is 4 tags and 45
system constants.

| what | where |
|---|---|
| name | `ICoreAttributes.Name`: `Local~PROFINET_interface_1`, `OB_Main`, `PIP 1`, or the name the user gave |
| data type | `IStructureItem.DisplayTypeName`: `Hw_Interface`, `Hw_SubModule`, `Hw_Device`, `Hw_IoSystem`, `Hw_Hsc`, `Hw_Pwm`, `Port` (V13); `OB_PCYCLE`, `OB_STARTUP`; `Pip`; `Int`, `Real`, `Time`, `String`, ... |
| value | `IDefaultStrategyData.DefaultValue`, as text: `64`, `T#5s`, `'Hello'` |
| comment | `ICoreAttributes.Comment` |
| made by TIA Portal | `IStructureRoot.IsSystemDefined` |
| what a hardware identifier stands for | relation `DeviceItem` to the module, interface or port |
| the block of an OB constant | relation `ConstantOf2` (`ConstantOf` in V13) to the `CodeBlockData` |

The last two relations are not declared on the constant. They are the other
direction of `DeviceItemData.ConstantTags` and `CodeBlockData.OBConstant`,
declared in place in the type model:

    <Relation name="ConstantTags" id="0x0010202e" ...>
      <Target ref="...ConstantTagData"/>
      <Inverse name="DeviceItem" id="0x0010202f" cardinality="1"/>
    </Relation>

A relation entry of an object can carry such an inverse id; the V21 type
model declares 185 of them. (`<Inverse ref="..."/>` only names an existing
relation.)

The hardware identifiers of the IO devices assigned to a PLC are constants of
that PLC (`IO_device_1~PROFINET_interface~Port_1`), with `DeviceItem` pointing
into the IO device's station.

**Verified:** user constants with `s12_constants` (TIA Portal V21, one action
per save: four constants of different types in two tag tables, a changed
value, a deletion; the result is identical to TIA Portal's export of the
constants and matches a screenshot of the two tabs). For
system constants: the count per tag table, in two projects (49 as above; 63 in
`s12_constants` = 4 tags, 1 user constant, 23 hardware identifiers including
those of two IO devices, 1 OB constant, 34 process image partitions);
and in the V16 sample the connection block written by its author has
`InterfaceId := 64`, which is the value of `Local~PROFINET-Schnittstelle_1`.
All 58 system constants of one PLC of `s12_constants` have the name, data
type and value TIA Portal's "System constants" tab shows (two screenshots).
The tab does not show what an identifier stands for; the `DeviceItem` target
agrees with the constant's name in all 23 cases.

## Data blocks

A data block is an object whose type derives from `DataBlockData`
(**verified** V13 to V19; the namespace changed between versions, the name did
not).

| what | V14 and later | V13 |
|---|---|---|
| number | `IGeneralBlockSourceData.Number` | `IGeneralBlockData.Number` |
| kind | `IGeneralDataBlockSourceData.Type`: `SharedDB`, `IDBofFB`, ... | `IGeneralDatablockData.Type` |
| instance of | `OfName` in the same set | same |
| access | `IGeneralBlockSourceData.OnlySymbolicAccess` | `IIecplObjectData.OnlySymbolicAccess` |
| interface | relation `BlockInterfaceBaseData.CurrentInterface` | relation `BlockInterfaceBaseData.Source` |

`OnlySymbolicAccess` = true means "optimized block access" (**verified** in
`s08_program`: two global blocks that differ in that option, and a block whose
generated source says `S7_Optimized_Access := 'TRUE'`). The block's own
comment is `ICoreAttributes.Comment`.

### Interface parts

The interface is a small tree of objects, each holding one XML document in a
blob attribute, plus an ordered list of further parts.

| | V14 and later | V13 |
|---|---|---|
| object types | `InterfaceVersionRootData`, `InterfacePartData` | `BlockInterfaceXmlPartData` |
| document | `IInterfacePartData.Payload` | `IXmlPartData.PayLoad` |
| part kind | `IInterfacePartData.Kind`: `BlockSource`, `DataTypeSource`, `InputSection`, `OutputSection`, `InOutSection`, `StaticSection`, `TempSection`, `ConstantSection`, `ReturnSection`, `Structure`, `Values`, `Comments` | `IBlockInterfaceXmlPartData.XmlPartKind`: 1, 2, 6 root, 3 external types, 4 structure, 5 section, 100 offsets, 110 and 120 values, 111 comments |
| list of parts | relation `InterfaceVersionRootData.UsedParts` | relation `XmlPartData.ReferencedXmlParts` |

The list keeps its empty positions (target type and id 0), because members
refer to parts by position.

A root document:

    <Root ...>
      <Member ID="51" Name="Run"   RID="0x02000001" StdO="0"  LID="9"/>
      <Member ID="52" Name="Speed" RID="0x02000005" StdO="16" LID="10"/>
      <Member ID="53" Name="Cfg"   RID="0x93010000" SubPartIndex="1" StdO="32"/>
      <Member ID="54" Name="Motor" RID="0x02000300" Type="&quot;MotorUDT&quot;" SubPartIndex="2" StdO="64"/>
      <Values><V p="52" v="1500"/><V p="53:51" v="true"/></Values>
    </Root>

- `ID` identifies a member inside its structure. IDs of user members start at
  51.
- `RID`: for elementary and named types the top byte is `0x02` and the low 16
  bits are a data type number; `0x02000001` is Bool. An anonymous `Struct` has
  `0x91`/`0x93` (`0x99` for an array of structures) on top and no type number
  (**inferred** from the samples).
- The names of the data type numbers are in the project itself: objects
  deriving from `DataTypeItemData`, with `IDataTypeContent.DataTypeID`,
  `Scope` (`S71200`, `S71500`, `S7300400`) and `ICoreAttributes.Name`. Seen:
  1 Bool, 2 Byte, 4 Word, 5 Int, 8 Real, 19 String, 48 LReal, 52 USInt,
  53 UInt, 67 DTL. They agree with the numbers the S7CommPlus protocol uses.
- `Type` is present where the number is not enough: arrays, strings with a
  length, PLC data types, function blocks.
- `StdO` is the offset in bits in the standard (not optimized) layout,
  relative to the enclosing structure or section (**verified**: the twelve
  offsets of `DB_Standard` in `s08_program`, a PLC data type inside it
  included, are those of the Offset column in TIA Portal's block editor).
- `SubPartIndex` is a position in the list of parts of the nearest enclosing
  root: the members of an anonymous structure (a `<Member>` document), or the
  root of a PLC data type or function block. In V13, named types are looked
  up in the list of the root's *external types* part instead (the part whose
  document is `<ExternalTypes>`), anonymous structures in the root's own list
  (**verified** on the V13 sample only).
- A function block root has one member per section, with fixed low IDs and no
  type: 2 Input, 3 Output, 4 InOut, 5 Static, 6 Temp, 8 Constant. Its members
  are inline or in a part of their own. In a block with standard access the
  section carries a `StdO` too, and its members count from there (seen in
  the V13 sample only).
- An instance data block's own interface object holds only values; the first
  entry of its list of parts is the root of the function block's interface
  (V14 and later). In V13 the block has a copy of the function block's root.

### Start values

`<Values>` with `<V p="path" v="value"/>` (V14 and later). The document is
found in one of three places: inside a root, in a part of its own (kind
`Values`), or, for the defaults of a PLC data type, in an expando attribute
named `Values` on the root object of the type (next to `ValuesCRC` and
`ValuesUndoVersion`). `<Value Path="path" Value="value"/>` in parts of kind
110 and 120 in V13. The path is the chain of member IDs separated by `:`, sections
not counted. A type root carries the defaults of the type; the block's own
values override them using the path from the block root (**verified** against
the SCL sources of the V19 sample and against the Start value column of TIA
Portal V21's block editor for `s08_program`, defaults of a PLC data type
included).

### Member comments

V14 and later. The comments of the members of one interface root are the
attributes of an `ICommentsExpandoAttributeSet` segment (see
[Expando segment](#expando-segment)), named by the member's ID path inside
that root, sections not counted: the same path the start values use.

- Blocks and data types of the project: the owner (code block, data block,
  PLC data type; anything deriving from `BlockInterfaceBaseData`) points to
  its interface root with `CurrentInterface` and to an
  `InterfaceCommentsPartData` object with `InterfaceComments`. An instance
  data block has no comments of its own; they are those of its function
  block, found through the function block's root.
- Library blocks: the interface root object carries the segment itself.

**Verified** against the block source generated by TIA Portal V21
(`s08_program`) and against the SCL sources of the V19 sample (93 comments).

In V13 a `CommentsXmlPartData` part maps each path to an index into a list of
`CoreText` objects; not decoded.

Other attributes seen and not understood: `LID`, `MFlags`, `v`, `h`,
`Remanence`, `Accessibility`, and the `<Offsets>` element of every document.

## Samples

Projects used to check all of the above. The public ones are not
redistributed with `tiaconv`; the V21 test projects were made for it.

| TIA version | layout | blocks | objects decoded | source |
|---|---|---|---|---|
| V13 | v11 | 11 886 | 10 450 | github.com/RoverURC/FolderPLC |
| V15.1 | v14 | 1 921 | 1 837 | github.com/majorBien/Inveo-RFID-Reader---Tia-Portal-Sample-programs-and-external-blocks |
| V16 | v14 | 926 | 825 | github.com/rossmann-engineering/EasyModbusTCP.PY (examples/example1) |
| V19 | v14 | 3 187 | 3 107 | github.com/LCC-Automation/OpenPID-TIA-SCL (`.zap19`) |
| V21 | v14 | 20 777 | 15 627 | `tests/fixtures` (seventeen project files, in this repository) |

Checked against statements outside the project files:

- V16: the example's source says "CPU 1211C DC/DC/DC (6ES7 211-1AE40-0XB0) –
  FW V4.1.3"; decoded: same type and order number, firmware `V4.1`.
- V19: the sources say "Target System: CPU 1510SP-1"; decoded: `CPU 1510SP-1 PN`.
- V19: the repository has the SCL sources of the author's function blocks.
  All 150 members of the five instance data blocks of those blocks, nested
  instances and structures included, come out with the name, data type,
  section, start value and comment of the declarations in the sources.
- V21, `s08_program`: TIA Portal's export of the tag table (eight tags) and
  the source it generated for one function block and its instance block are
  kept in `tests/fixtures/s08_program/exports`; the decoded tags and members
  are identical to them, comments included.
- V16: the example's client program connects to port 502; the connection
  block in the project has `LocalPort := 502`.
- V13: the tags `System_Byte`, `DiagStatusUpdate`, `AlwaysTRUE`,
  `AlwaysFALSE` are at `%MB1`, `%M1.1`, `%M1.2`, `%M1.3`, where TIA Portal
  puts the system memory bits of byte 1.

- V21: the eight test projects were made one change at a time (see
  `tests/fixtures/README.md`). Device name, IP address, mask, router, PROFINET
  name and the new subnet each appear in the decoded output at exactly the
  step where they were entered.

Confirmed in TIA Portal by the author of the test projects: the second PLC of
`s07_second` is not attached to the subnet, as the file says, and TIA Portal
shows `zzbravo` as its automatically generated PROFINET device name while the
file still stores `plc_1`. The order number, type and firmware version of
all three configured items (both CPUs and the signal module) are as decoded.

## Open questions

- Layout of V11, V12, V14, V17, V18, V20 projects.
- Projects with several `.plf` files.
- Protected (encrypted) projects and know-how-protected blocks.
- The meaning of `NodeIPConfiguration` values.
- The exact rule TIA Portal uses to derive an automatic PROFINET device name
  from the device name (`ZZBRAVO` gives `zzbravo`, `PLC_1` gives `plc_1`).
- Relation lists beyond the two keyed ones.
- Offsets in instance blocks with standard access (section offset plus member
  offset) have only been seen in the V13 sample.
- The `h` attribute of a start value.
- Access level numbers of an S7-1200 with firmware V4.0 to V4.6 (assumed to
  be those of V4.7) and of other CPU families; whether the level follows the
  Anonymous user on an S7-1500 as it does on the S7-1200; other values of `OmsCommunicationMode` and `TimeSyncRole`.
- Constants of structured types, if TIA Portal allows them, and system
  constants of kinds not in the samples.
- Whether anything besides know-how protection makes TIA Portal rewrite the
  project file; what writes a file in one go in the V15.1 sample.
- When exactly TIA Portal sets the "modified" time of the project object.
- Other `FeedbackId` values of the project history (upgrades between V14+
  versions, library updates), and whether TIA Portal's "Project history" tab
  shows the same entries.
- HMI tags: see the list at the end of that section. Screens, alarms,
  scripts and recipes are not looked at; a screen item names the tags it
  uses in expando attributes (`Dyn.ProcessValue.Tag#`).
- Which relation a relation list without ids belongs to (`Consumer` and
  `Provider` of a link object, in that order, when there are many links).
- Blocks: fail-safe blocks, GRAPH and other languages not in the samples,
  blocks that are instances of library types (relation `IsInstanceOf`),
  download times against a real download.
- Block code: STL beyond the simplest statements (labels, jumps, comments,
  calls); SCL keywords and operators not in the samples; LAD and
  FBD gates not in the samples; the declarations of a code block's
  parameters and variables as source text; how the code of a know-how
  protected block is stored (not examined, and not to be read).
- PROFIBUS master systems, I-devices, shared devices, MRP domains, IO systems
  with more than one controller interface.
- Connection types other than S7 and HMI; TSAPs; an HMI connection in a
  project made for the purpose.
- Whether the older layout marks saves the same way (the V13 sample has four
  markers and its modification date steps back with `--save`, but nobody
  recorded what was done in each).
- Comments in V13 projects.
- Which language TIA Portal treats as the one without a language id.
- Technology objects and other blocks that derive from `DataBlockData`: they
  are listed like any block, their meaning is not looked at.
- How V13 marks members of a named type inside a global data block (the
  `<Externals>` element of its root); the sample has no such block.
- The meaning of the second header field of a kind 2 expando segment.
