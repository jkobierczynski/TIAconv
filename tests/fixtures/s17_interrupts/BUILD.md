# s17_interrupts: how to build it

A new project for the interrupts of S7Forge: a cyclic interrupt with its
priority, phase and events to be queued changed, a time-delay interrupt
started from SCL and from LAD, two time-of-day interrupts (one set in its
properties, one by the program), a hardware interrupt on an input of a DI
module, and the time error OB. TIA Portal V21, one S7-1500 CPU, one action
per save as in s15_motor and s16_stl. When done, `tests/fixtures/README.md`
gets the table of saves as it really went.

What it answers for tiaconv and S7Forge:

- the event names, event classes, priorities and "events to be queued" the
  project stores for the time delay, time of day, hardware and time error
  OBs (only program cycle, cyclic and startup are known now);
- where a time-of-day OB stores the execution, start date and time set in
  its properties;
- where the hardware interrupt of an input channel is stored, and how it
  names its OB (so that S7Forge can raise it from an input edge);
- how calls of SRT_DINT, CAN_DINT, QRY_DINT, SET_TINTL, ACT_TINT,
  QRY_TINT and RD_SYS_T are stored (the kind of their target, their
  parameters), in SCL and as a LAD box with EN.

No program in this project needs to run anywhere: tiaconv only reads it.

## General

- Project name `s17_interrupts`. Security wizard at its defaults; any
  password a throwaway one.
- Keep TIA Portal's defaults for everything not named here. Where a
  property is named differently in V21, or is not where this sheet says,
  set what comes closest and note what you found: that is what we want to
  know.
- Save after each numbered step.

## Saves

| save | action |
|---|---|
| 1–2 | new project `s17_interrupts` (TIA Portal saves on creation and after the first view) |
| 3 | add CPU 1511-1 PN (6ES7 511-1AL03-0AB0, firmware V4.1), name it `S17PLC` |
| 4 | in the device view, slot 2: DI 16x24VDC HF (6ES7 521-1BH00-0AB0, or the DI 16 HF the catalog offers); its input addresses from 10 (Properties > I/O addresses > Start address 10) |
| 5 | Default tag table: the tags below |
| 6 | global DB `S17Data`, optimized, the members below |
| 7 | OB `Cyc100`: Add new block > Organization block > Cyclic interrupt, number 30, cycle time 100000 µs (the default); code below |
| 8 | `Cyc100` properties: Cyclic interrupt > Phase shift **20000** µs; General (or Attributes) > Priority **10**; Events to be queued **3** (if offered); nothing else |
| 9 | OB `Delay`: Time delay interrupt, number 20; code below |
| 10 | OB `TodProp`: Time of day, number 10; properties > Time of day: Execution **Every minute**, Start date **2026-01-01**, Time of day **08:00:00**, Local time (the default); code below |
| 11 | OB `TodProg`: Time of day, number 11; properties left at their defaults (Execution: Never, or what is offered); code below |
| 12 | OB `HwIn`: Hardware interrupt, number 40; code below |
| 13 | the DI module, Properties > Inputs > Channel 0: **Hardware interrupt: rising edge** on (Enable rising edge detection), event name left as TIA Portal makes it, Hardware interrupt OB **HwIn** |
| 14 | OB `TimeErr`: Time error interrupt (OB 80); code below |
| 15 | FC `Irq`, SCL; code below |
| 16 | Main (OB 1, LAD): network 1 calls `Irq`; network 2: a contact `StartLad` → box **SRT_DINT** (OB_NR `Delay`, DTIME `T#500ms`, SIGN `16#2`), RET_VAL → `RetLad` |
| 17 | compile the software (Compile > Software, rebuild all); note errors and warnings |

### Tags (save 5)

| name | type | address |
|---|---|---|
| `Btn` | Bool | %I10.0 |
| `StartDelay` | Bool | %M0.0 |
| `CancelDelay` | Bool | %M0.1 |
| `SetTod` | Bool | %M0.2 |
| `StartLad` | Bool | %M0.3 |
| `RetLad` | Int | %MW12 |

### S17Data (save 6)

`cycN` Int, `delayN` Int, `delaySign` Word, `todPropN` Int, `todProgN`
Int, `hwN` Int, `teN` Int, `ret` Int, `status` Word, `todStatus` Word,
`start` DTL, `now` DTL.

### The code of the OBs (saves 7, 9–12, 14), SCL

Each OB: change its language to SCL when you add it (Add new block,
Language SCL).

`Cyc100`:

    "S17Data".cycN := "S17Data".cycN + 1;

`Delay` (its Input `Sign` is made by TIA Portal; if it is not there, note it):

    "S17Data".delayN := "S17Data".delayN + 1;
    "S17Data".delaySign := #Sign;

`TodProp`:

    "S17Data".todPropN := "S17Data".todPropN + 1;

`TodProg`:

    "S17Data".todProgN := "S17Data".todProgN + 1;

`HwIn`:

    "S17Data".hwN := "S17Data".hwN + 1;

`TimeErr`:

    "S17Data".teN := "S17Data".teN + 1;

### Irq (save 15), SCL

    IF "StartDelay" THEN
        "S17Data".ret := SRT_DINT(OB_NR := "Delay", DTIME := T#2s, SIGN := 16#7);
        "StartDelay" := FALSE;
    END_IF;
    IF "CancelDelay" THEN
        "S17Data".ret := CAN_DINT(OB_NR := "Delay");
        "CancelDelay" := FALSE;
    END_IF;
    "S17Data".ret := QRY_DINT(OB_NR := "Delay", STATUS => "S17Data".status);
    IF "SetTod" THEN
        "S17Data".ret := SET_TINTL(OB_NR := "TodProg", SDT := "S17Data".start,
                                   LOCAL := TRUE, PERIOD := 16#1001, ACTIVATE := TRUE);
        "SetTod" := FALSE;
    END_IF;
    "S17Data".ret := QRY_TINT(OB_NR := "TodProg", STATUS => "S17Data".todStatus);
    "S17Data".ret := RD_SYS_T(OUT => "S17Data".now);

`OB_NR := "Delay"` names the OB by the system constant TIA Portal makes for
it (the editor offers it); if it wants a number, write `20` (and `11` for
`TodProg`) and note it.

## References to take

- Screenshots: the properties of `Cyc100` (cyclic interrupt, and where the
  priority and the events to be queued are), of `TodProp` (time of day),
  of channel 0 of the DI module (hardware interrupt), network 2 of Main, and
  the compile result.
- After save 17: right-click `Irq` > Generate source from blocks (only the
  block), into `exports/Irq.scl`.
- Note anything the editor refused or changed.

Then copy the project folder (only `System/PEData.plf` goes into the
repository) or attach it here as a zip, as for s16.
