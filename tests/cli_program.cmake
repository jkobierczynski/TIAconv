# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the executable with --program-json on s10_connections (four PLCs):
# one file per PLC, each with the format and schema version; and on an
# earlier save, which the file records.
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<project> -DOUT=<dir> -P cli_program.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${TIACONV}" -q --program-json "${OUT}/now" "${PROJECT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --program-json failed: ${rc}")
endif()
foreach(plc ZZALPHA ZZBRAVO ZZCHARLIE ZZDELTA)
  set(f "${OUT}/now/${plc}.json")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "no ${f}")
  endif()
  file(READ "${f}" got)
  string(FIND "${got}" "\"format\": \"tiaconv-program\",\n  \"schema_version\": 2," at)
  if(NOT at EQUAL 4)
    message(FATAL_ERROR "${plc}.json does not start with the format and schema version")
  endif()
  string(FIND "${got}" "\"plc\": {\n    \"name\": \"${plc}\"," at)
  if(at LESS 0)
    message(FATAL_ERROR "${plc}.json is not the program of ${plc}")
  endif()
endforeach()

execute_process(COMMAND "${TIACONV}" -q --save 20 --program-json "${OUT}/save20" "${PROJECT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --save 20 --program-json failed: ${rc}")
endif()
file(GLOB written "${OUT}/save20/*.json")
list(LENGTH written n)
if(n LESS 1)
  message(FATAL_ERROR "nothing written for save 20")
endif()
list(GET written 0 first)
file(READ "${first}" got)
string(FIND "${got}" "\"save\": 20}" at)
if(at LESS 0)
  message(FATAL_ERROR "the file does not record save 20")
endif()

# standard output is not a folder
execute_process(COMMAND "${TIACONV}" -q --program-json - "${PROJECT}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "--program-json - should be a usage error, got ${rc}")
endif()
