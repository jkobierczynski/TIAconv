# s16_stl: how to build it

A new project for S7Forge's STL and the S5 timers and counters: absolute and
indirect addressing, address registers, local data by address, S5 timers
and counters in STL, LAD and FBD, MCR, and one block typed with German
mnemonics. TIA Portal V21, one S7-1500 CPU (the S7-1200 has no STL), one
action per save as in s15_motor. When done, `tests/fixtures/README.md` gets
the table of saves as it really went.

What it answers for S7Forge and tiaconv:

- how the editor shows absolute and indirect operands (`%DBW2`,
  `%DBW[AR1,P#2.0]`, `%T1`), and whether tiaconv's `text` gives the same;
- the names the project stores for the S5 boxes, coils and their pins;
- whether the mnemonic setting (German / International) is stored in the
  project or only changes the display.

## General

- Project name `s16_stl`. Security wizard at its defaults; any password
  a throwaway one.
- **No tags in M** except those listed: the absolute operands below must
  stay absolute (with a tag at the address, the editor shows the tag).
- Every block of STL, and `S16Data`: **Optimized block access off**
  (Properties > Attributes). The absolute operands are refused otherwise.
- Type the STL in the editor, one network at a time, as given (you can paste
  a network's text into the STL editor). International mnemonics, except in
  `StlGerman`.
- If the editor changes what you typed (adds `%`, shows a name instead of an
  address, changes spacing), keep what it made and note it: that is what we
  want to see.
- Save after each numbered step.

## Saves

| save | action |
|---|---|
| 1–2 | new project `s16_stl` (TIA Portal saves on creation and after the first view) |
| 3 | add CPU 1511-1 PN (6ES7 511-1AL03-0AB0, the newest firmware offered), name it `S16PLC` |
| 4 | Default tag table: the tags below |
| 5 | global DB `S16Data`, **number 5** (Properties > General > Number: manual, 5), optimized access off, the members below |
| 6 | FC `StlLogic`, STL, networks 1–3 below |
| 7 | FC `StlAbs`, STL, its temporaries and networks 1–3 |
| 8 | FC `StlS5`, STL, networks 1–3 |
| 9 | the mnemonic set to **German** (Options > Settings > PLC programming > General > Mnemonic), nothing else |
| 10 | FC `StlGerman`, STL, typed in German, network 1 |
| 11 | the mnemonic set back to **International**, nothing else |
| 12 | FB `LadS5`, LAD (optimized access may stay on), networks 1–7, instance DB `LadS5_DB` |
| 13 | FC `FbdS5`, FBD, network 1 |
| 14 | Main (OB 1, LAD): network 1 calls `StlLogic`, 2 `StlAbs`, 3 `StlS5`, 4 `StlGerman`, 5 `LadS5` (instance `LadS5_DB`), 6 `FbdS5` |
| 15 | compile the software (Compile > Software, rebuild all); note errors and warnings |

### Tags (save 4)

| name | type | address |
|---|---|---|
| `Start` | Bool | %I0.0 |
| `Stop` | Bool | %I0.1 |
| `Level` | Bool | %I0.2 |
| `Rst` | Bool | %I0.3 |
| `Up` | Bool | %I0.4 |
| `Down` | Bool | %I0.5 |
| `SetC` | Bool | %I0.6 |
| `Sel` | Byte | %IB1 |
| `Out1` … `Out8` | Bool | %Q0.0 … %Q0.7 |
| `RunOn` | Timer | %T7 |
| `Parts` | Counter | %C3 |

### S16Data (save 5)

`b0` Bool, `b1` Bool, `by` Byte, `w` Int, `d` DInt, `r` Real, `s`
String[4], `arr` Array[1..3] of Int. Start values all default.

### StlLogic (save 6)

Network 1, AND before OR and a bracket:

    A     "Start"
    A     "Stop"
    O
    A     "Level"
    AN    "Rst"
    =     "Out1"
    A     "Start"
    O(
    A     "Level"
    A     "Up"
    )
    =     "Out2"

Network 2, an edge, a jump, a jump list:

    A     "Up"
    FP    %M20.0
    JCN   M001
    L     %MW22
    +     1
    T     %MW22
    M001: L     "Sel"
    JL    LST
    JU    L0
    JU    L1
    LST:  JU    L2
    L0:   L     10
    T     %MW24
    JU    END
    L1:   L     11
    T     %MW24
    JU    END
    L2:   L     12
    T     %MW24
    END:  NOP   0

Network 3, arithmetic, a comparison, RND:

    L     %MW22
    L     3
    *I
    T     %MW26
    L     %MW26
    L     10
    >I
    =     "Out3"
    L     2.5
    RND
    T     %MD28

### StlAbs (save 7)

Temp: `a` Int, `b` Bool, `c` Int, `p` Any, `x` Int.

Network 1, the open DB, absolute and fully qualified:

    OPN   "S16Data"
    SET
    =     %DBX0.1
    L     7
    T     %DBB1
    L     1234
    T     %DBW2
    L     %DB5.DBW2
    +     1
    T     %DB5.DBW18
    L     2.5
    T     %DBD8
    L     DBNO
    T     %MW30

Network 2, AR1, AR2, register- and memory-indirect:

    L     P#20.0
    LAR1
    L     42
    T     %DBW[AR1,P#2.0]
    L     P#M40.0
    LAR2
    L     99
    T     W[AR2,P#0.0]
    L     16
    T     %MD44
    L     %DBW[%MD44]
    T     %MW48

Network 3, local data by address:

    L     5
    T     #a
    L     %LW0
    +     1
    T     %LW4
    SET
    =     %L2.1
    A     %L2.1
    =     %M50.0
    L     42
    T     #x
    L     %LW16
    T     %MW52
    L     P##c
    LAR1
    L     77
    T     W[AR1,P#0.0]
    L     #c
    T     %MW54

### StlS5 (save 8)

Network 1, S5 timers, by address and by tag:

    A     "Start"
    L     S5T#2S
    SD    %T1
    A     %T1
    =     "Out4"
    L     %T1
    T     %MW60
    LC    %T1
    T     %MW62
    A     "Stop"
    L     S5T#1S
    SF    "RunOn"
    A     "RunOn"
    =     "Out5"
    A     "Rst"
    R     %T1

Network 2, an S5 counter by tag:

    A     "Up"
    CU    "Parts"
    A     "Down"
    CD    "Parts"
    A     "SetC"
    L     C#5
    S     "Parts"
    A     "Rst"
    R     "Parts"
    A     "Parts"
    =     "Out6"
    L     "Parts"
    T     %MW64
    LC    "Parts"
    T     %MW66

Network 3, MCR:

    MCRA
    A     "Level"
    MCR(
    A     "Start"
    =     "Out7"
    L     77
    T     %MW68
    )MCR
    MCRD

### StlGerman (save 10, the mnemonic on German)

Network 1 (SE in German is the on delay; A 1.0 is the output Q1.0):

    U     "Start"
    L     S5T#2S
    SE    %T2
    U     %T2
    =     %A1.0
    U     "Level"
    ZV    %Z4
    L     %Z4
    T     %MW76

### LadS5 (save 12, LAD)

1. contact `Start` → box **S_ODT**: T_NO `%T3`, TV `S5T#3S`, R `Rst`;
   Q → coil `Out8`, BI → `%MW70`, BCD → `%MW72`.
2. contact `Level` → coil **SD** (Start on-delay timer coil) on `%T4`,
   time value `S5T#500MS`.
3. contact `%T4` → coil `%Q1.1`.
4. contact `Rst` → coil **R** on `%T4`.
5. box **S_CUD**: C_NO `%C2`, CU `Up`, CD `Down`, S `SetC`, PV `C#7`,
   R `Rst`; CV → `%MW74`, Q → `%Q1.2`.
6. contact `Up` → coil **CU** on `%C5`.
7. contact `SetC` → coil **SC** (set counter value) on `%C5`, value `C#3`;
   then a new network or a branch: contact `%C5` → coil `%Q1.3`.

### FbdS5 (save 13, FBD)

1. box **S_PULSE**: T_NO `%T6`, S `Start`, TV `S5T#1S`; Q → `%Q1.4`.

## References to take

- After save 15: for `StlLogic`, `StlAbs`, `StlS5` and `StlGerman`,
  right-click > Generate source from blocks (only the block), into
  `exports/` (`.awl`). That is the operand text as TIA Portal writes it.
- Screenshots: each network of `StlAbs` as the editor shows it; the seven
  networks of `LadS5` and the one of `FbdS5`; the compile result.
- Note anything the editor refused or changed.

Then copy the project folder (the whole folder; only `System/PEData.plf`
goes into the repository) to where you put s15_motor before, or attach it
here.
