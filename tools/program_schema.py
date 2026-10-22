#!/usr/bin/env python3
# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes docs/program.schema.json, the JSON Schema of --program-json.
# Every object is closed (additionalProperties false) and lists all its keys
# as required. Usage: python3 tools/program_schema.py docs/program.schema.json
import json,sys
S=lambda d=None: {"type":"string", **({"description":d} if d else {})}
NS=lambda d=None: {"type":["string","null"], **({"description":d} if d else {})}
I=lambda d=None: {"type":"integer", **({"description":d} if d else {})}
NI=lambda d=None: {"type":["integer","null"], **({"description":d} if d else {})}
B=lambda d=None: {"type":"boolean", **({"description":d} if d else {})}
NB=lambda d=None: {"type":["boolean","null"], **({"description":d} if d else {})}
A=lambda items,d=None: {"type":"array","items":items, **({"description":d} if d else {})}
R=lambda n: {"$ref":"#/$defs/"+n}
def O(props,d=None):
    o={"type":"object","additionalProperties":False,"required":list(props.keys()),"properties":props}
    if d: o["description"]=d
    return o
defs={}
defs["type"]=O({
 "kind":{"enum":["elementary","string","array","struct","plc_type","system_type","instance","parameter","void","named","unknown"],
         "description":"named: a quoted name that is not a PLC data type or FB of this PLC"},
 "name":NS("Int, String, Array, Struct, the name of the PLC data type, system type or FB, Variant, ..."),
 "bits":NI("elementary: the size in bits"),
 "length":NI("string: the maximum number of characters"),
 "dims":A(O({"low":NI("null when not known: a variable bound (*) or a constant not found"),"low_text":S("as written: 0, #MAX, \"MAX\", *"),
             "high":NI(),"high_text":S()}),"array: one entry per dimension"),
 "element":{"oneOf":[R("type"),{"type":"null"}],"description":"array: the type of an element"},
},"a data type taken apart")
defs["value"]=O({
 "kind":{"enum":["bool","int","real","duration","date","time_of_day","date_and_time","string","char","unknown"]},
 "text":S("as written (empty for the default value of a constant that stores none)"),
 "bool":NB(),
 "int":NI("int: the value, also of a bit string (16#FF: 255); char: its code"),
 "real":{"type":["number","null"]},
 "ns":NI("duration in nanoseconds; time_of_day: since midnight; date_and_time: since 1970-01-01"),
 "days":NI("date: since 1990-01-01"),
 "string":NS("string, char: the characters, escapes ($', $N, $hh) decoded, as UTF-8"),
},"a value parsed")
defs["member"]=O({
 "name":S(),
 "data_type":NS("as TIA Portal writes it: Bool, Int, Array[0..9] of Real, String[20], \"MyUdt\", TON_TIME"),
 "type":R("type"),
 "section":NS("Input, Output, InOut, Static, Temp, Constant, Return; null in a global data block and below the top level"),
 "offset":NS("byte or byte.bit from the start of the data block or instance, \"12\", \"12.3\"; only with standard (not optimized) access"),
 "offset_bits":NI("the same in bits"),
 "start_value":NS("as written in the project; null: the default of the type (or of an enclosing structure)"),
 "start":{"oneOf":[R("value"),{"type":"null"}],"description":"start_value parsed; null with it"},
 "retain":B(),
 "accessible_from_hmi":B("Accessible from HMI/OPC UA/Web API, as limited by an enclosing structure"),
 "writable_from_hmi":B("Writable from HMI/OPC UA/Web API, as limited by an enclosing structure"),
 "setpoint":B(),
 "unresolved":B("the nested type could not be followed; members may be missing"),
 "comment":NS(),
 "members":A(R("member"),"members of a structure, UDT, multi-instance or array element type"),
})
defs["operand"]=O({
 "text":S("as written in code: \"Tag\", \"DB\".member, #local, 5, T#5S"),
 "ref_id":NI("the entry of the block's references"),
 "linked":B("the entry was found"),
 "kind":NS("tag, data block member, local, constant, local constant, user constant, ..."),
 "data_type":NS(),
 "data_type_from_pin":B("data_type is the pin's: a literal stored without a type"),
 "data_block":NS(),
 "address":NS("a tag's address (%M10.0); a member of a data block with standard access: %DB1.DBX60.0, %DB2.DBW4"),
 "pin_type":NS("the type the pin takes, where tiaconv knows it"),
 "value":{"oneOf":[R("value"),{"type":"null"}],"description":"a constant (literal, local or user constant): its value"},
})
defs["pin"]=O({
 "name":S(),
 "direction":{"enum":["in","out"]},
 "connected":A(S(),"an operand as written in code, \"power rail\", or the pin of another part as \"<uid>.<pin>\""),
 "operands":A(R("operand"),"the operands among connected, in that order"),
})
defs["element"]=O({
 "uid":I(),
 "kind":{"enum":["gate","call","instruction"]},
 "name":S("the gate as stored (Contact, Coil, Add, Move, ...), the block called, or the instruction"),
 "ref_id":NI("call, instruction: its entry of the references"),
 "instance":NS(),
 "instance_ref_id":NI(),
 "data_type":NS("the type a box was set to; null when not set (an AutomaticTyped option: Auto)"),
 "options":A(O({"name":S(),"value":S()}),"what the part was set to: SrcType, Card (number of inputs), DisableENO, negated (a negated pin), AutomaticTyped"),
 "pins":A(R("pin")),
})
defs["expr"]=O({
 "op":{"enum":["ref","const","member","index","slice","not","neg","add","sub","mul","div","mod","pow","and","or","xor",
               "eq","ne","lt","le","gt","ge","call","in","out","inout","edge_pos","edge_neg","wire","range","builtin",
               "missing","unknown"]},
 "ref_id":NI("ref, const: the entry of the block's references; member, index: the entry for the whole access; call: the block or instruction called"),
 "instance_ref_id":NI("call: the instance (data block or multi-instance)"),
 "name":NS("member: the member; slice: %X3, %B1, ...; call: the name of what is called; in, out, inout: the parameter; wire: <uid>.<pin>; builtin: ENO; unknown: what it is"),
 "type":NS("the data type of the result, where the project gives it"),
 "value":{"oneOf":[R("value"),{"type":"null"}],"description":"const: the literal parsed"},
 "args":A(R("expr"),"the operands, in order"),
},"an expression of the code tree")
defs["branch"]=O({"exprs":A(R("expr"),"if: the condition; case: the labels"),"body":A(R("stmt"))})
defs["stmt"]=O({
 "op":{"enum":["assign","call","if","case","for","while","repeat","return","exit","continue","label","region",
               "empty","eval","stl","unknown"]},
 "name":NS("region, label: the name; stl: the instruction; unknown: what it is"),
 "exprs":A(R("expr"),"assign: target, value; call: the call; eval: an expression evaluated for its edges; case: the selector; for: variable, from, to, [by]; while, repeat: the condition; stl: the operands"),
 "branches":A(R("branch"),"if: one per IF/ELSIF; case: one per list of labels"),
 "body":A(R("stmt"),"for, while, repeat, region"),
 "else":{"oneOf":[A(R("stmt")),{"type":"null"}],"description":"if, case: the ELSE part; null without one"},
},"a statement of the code tree")
defs["network"]=O({
 "number":I("position in the block, 1 = first"),
 "network_id":I("the number the project knows it by"),
 "title":NS(),"comment":NS(),
 "language":NS(),
 "content":{"enum":["scl","stl","graphic","empty","unread"]},
 "lines":A(S(),"scl, stl: the source text; graphic: tiaconv's listing of the elements"),
 "elements":A(R("element"),"graphic: the parts and their wires"),
 "statements":{"oneOf":[A(R("stmt")),{"type":"null"}],"description":"the code as a tree: SCL, LAD, FBD and STL; null when the network could not be read"},
 "tree_notes":A(S(),"what of the tree could not be read or is written in a way of its own"),
 "notes":A(S()),
})
defs["reference"]=O({
 "ref_id":I(),"kind":S(),"kind_stored":NS(),"text":S(),"data_type":NS(),
 "value":{"oneOf":[R("value"),{"type":"null"}],"description":"a constant (literal, local or user constant): its value"},
 "data_block":NS(),
 "declared_as":NS("a multi-instance: the member declared with this block as its type"),
})
defs["code"]=O({"networks":A(R("network")),"references":A(R("reference"),"what the block's code refers to")})
defs["block"]=O({
 "type":{"enum":["OB","FB","FC"]},
 "name":S(),"number":NI(),"language":NS(),"optimized":NB(),
 "protection":NS("know-how, write, know-how, write, system"),
 "simulatable":B("nothing listed for it under not_simulatable"),
 "interface":{"oneOf":[A(R("member")),{"type":"null"}],"description":"every section; null for a protected block or when not read"},
 "code":{"oneOf":[R("code"),{"type":"null"}],"description":"null for a protected block"},
})
schema={
 "$schema":"https://json-schema.org/draft/2020-12/schema",
 "$id":"https://github.com/jkobierczynski/TIAconv/docs/program.schema.json",
 "title":"tiaconv program export",
 "description":"The program of one PLC, as tiaconv --program-json writes it. See docs/PROGRAM-JSON.md.",
 **O({
  "format":{"const":"tiaconv-program"},
  "schema_version":{"const":2},
  "tool":O({"name":{"const":"tiaconv"},"version":S()}),
  "source":O({"file":S(),"project":NS(),"save":NI("the project as it was after this save; null: as it is now")}),
  "plc":O({"name":S(),"station":NS(),"type":NS(),"order_number":NS(),"firmware":NS()}),
  "io":A(O({"station":S(),"module":S(),"item":NS(),"area":{"enum":["I","Q"]},"start_bit":I(),"length_bits":I(),
            "process_image":I("0: automatic update"),"text":S("\"I 64...67\"")}),
        "the I/O addresses of the modules of the PLC's station and of the IO devices it controls"),
  "tags":A(O({"name":S(),"table":NS(),"data_type":NS(),"type":R("type"),"address":NS(),"comment":NS()})),
  "constants":A(O({"name":S(),"table":NS(),"data_type":NS(),"value":NS(),"parsed":{"oneOf":[R("value"),{"type":"null"}]},"kind":NS("hardware, ob, pip, user, system"),"system":B(),"comment":NS()})),
  "types":A(O({"name":S(),"kind":{"enum":["plc_type","system_type"]},"members":A(R("member"))}),
            "PLC data types (from their definition where it was read) and the system types met in the members (TON_TIME, DTL), with their members"),
  "data_blocks":A(O({"name":S(),"number":NI(),"kind":NS("global, instance"),"instance_of":NS(),"optimized":NB(),
                     "notes":A(S()),"members":A(R("member"))})),
  "organization_blocks":A(O({"name":S(),"number":NI(),"event":NS("ProgramCycle, CyclicInterrupt, Startup, ..."),
                             "priority":NI(),"cycle_us":NI(),"phase_shift_us":NI()})),
  "blocks":A(R("block")),
  "instructions":A(O({"name":S(),"version":NS("major.minor of the library version")}),"the instructions the code uses"),
  "not_simulatable":A(O({"block":S(),"type":S(),"reason":S()})),
 }),
 "$defs":defs,
}
json.dump(schema,open(sys.argv[1],'w'),indent=2,ensure_ascii=False); open(sys.argv[1],'a').write('\n')
