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

The file is append-only. Saving writes new versions of changed objects at the
end; the **last block with a given (type id, object id) wins**. A deleted
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
change). Reading the block list only up to the n-th of them gives the project
as it was after save n; `ContainerOptions::throughSave` does that. "Save as"
keeps the history; a project archive (`.zap`) of the V19 sample has a single
save, so archiving appears to drop it.

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
  points to cover the segment exactly, with no gap and no overlap, in 39 062
  of 39 064 segments that hold no nested structure (the four public samples,
  `s07_second`, `s08_program`, `s09_security` and `s10_connections`). The
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
| structure, array | 4 | offset (content not decoded by `tiaconv`) |

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

Which relation such a list belongs to is **unknown**. For the inventory this
does not matter: every child names its parent in its keyed single-valued list.
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

`tiaconv` reads none of this. Blobs appear in `--objects` by size only, and
text in any attribute with "password" in its name is replaced by
`{"redacted": true}`.

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
`Part7` byte, `Part8` bit; **inferred**, not used). System constants such as
hardware identifiers are `SimaticConstantTagData` objects in the same tag
table and are not listed; the number TIA Portal shows next to a tag table
counts them too (`Default tag table [49]` in `s08_program` = 4 tags + 45
system constants). Where user constants live is **unknown**.

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
| V21 | v14 | 10 560 | 7 803 | `tests/fixtures` (eleven projects, in this repository) |

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
- User constants.
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
