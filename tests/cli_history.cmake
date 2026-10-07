# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the executable with --history on the project in which an S7 connection
# was made in save 62 and deleted in save 64 (tests/fixtures/README.md), and
# checks the three ways the history is written.
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<project> -DOUT=<scratch dir> -P cli_history.cmake
file(MAKE_DIRECTORY "${OUT}")

execute_process(COMMAND "${TIACONV}" "${PROJECT}" OUTPUT_VARIABLE plain RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR plain MATCHES "Save history:")
  message(FATAL_ERROR "the history must only be printed when asked for")
endif()

execute_process(COMMAND "${TIACONV}" --history --save 64 "${PROJECT}" OUTPUT_VARIABLE text RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --history failed: ${rc}")
endif()
if(NOT text MATCHES "\nSave history:\n" OR NOT text MATCHES "Project created with TIA Portal V21")
  message(FATAL_ERROR "--history: heading or TIA Portal's own entry missing")
endif()
if(NOT text MATCHES "  Save 62  2026-10-06T12:44:44.292Z  by PC  -  13 objects written\n      [+] connection S7-1500/ET200MP station_1 / S7_Connection_1  [(]S7 to ")
  message(FATAL_ERROR "--history: save 62 should show the new connection")
endif()
if(NOT text MATCHES "  Save 64  [^\n]*, 2 of them as deleted\n      - connection S7-1500/ET200MP station_1 / S7_Connection_1 ")
  message(FATAL_ERROR "--history: save 64 should show the connection removed")
endif()
if(text MATCHES "\n  Save 65 " OR NOT text MATCHES "Note: covers saves 1 to 64 of 71")
  message(FATAL_ERROR "--history --save 64 should stop at save 64 and say so")
endif()

# CSV: byte-order mark, header, one row per change
execute_process(COMMAND "${TIACONV}" -q --history-csv "${OUT}/history.csv" "${PROJECT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --history-csv failed: ${rc}")
endif()
file(READ "${OUT}/history.csv" head LIMIT 3 HEX)
if(NOT head STREQUAL "efbbbf")
  message(FATAL_ERROR "history CSV: no byte-order mark")
endif()
file(READ "${OUT}/history.csv" csv)
if(NOT csv MATCHES "save,time,by,objects_written,objects_deleted,change,kind,item,part_of,attribute,from,to,description")
  message(FATAL_ERROR "history CSV: header missing")
endif()
if(NOT csv MATCHES "\n64,2026-10-06T13:06:09.906Z,PC,9,2,removed,connection,S7-1500/ET200MP station_1 / S7_Connection_1,")
  message(FATAL_ERROR "history CSV: the removed connection of save 64 is missing")
endif()
if(NOT csv MATCHES "\n71,[^\n]*,changed,module,S7-1200 station_2 / ZZDELTA,,access level,HMI access,Full access [(]no protection[)],")
  message(FATAL_ERROR "history CSV: the access level change of save 71 is missing")
endif()

# JSON: the history is one more key, and the text report moves to standard error
execute_process(COMMAND "${TIACONV}" --history --json - "${PROJECT}"
                OUTPUT_VARIABLE json ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT json MATCHES "\"history\": {\"saves_in_file\": 71, \"events\": \\[{\"date\": \"2026-10-05T13:00:10.135Z\"")
  message(FATAL_ERROR "--history --json: history key missing")
endif()
if(NOT json MATCHES "{\"change\": \"added\", \"kind\": \"connection\", \"item\": \"S7-1500/ET200MP station_1 / S7_Connection_2\", \"description\": \"S7 to 192.168.77.99\", \"part_of\": null}")
  message(FATAL_ERROR "--history --json: the connection of save 63 is missing")
endif()
if(json MATCHES "Save history:" OR NOT err MATCHES "Save history:")
  message(FATAL_ERROR "--json -: the text report belongs on standard error")
endif()
execute_process(COMMAND "${TIACONV}" -q --json - "${PROJECT}" OUTPUT_VARIABLE json RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR json MATCHES "\"history\"")
  message(FATAL_ERROR "JSON without --history must not have the history key")
endif()
