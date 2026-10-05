# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the executable on a test project and checks how the CSV files start:
# with a UTF-8 byte-order mark by default, without one for --no-bom.
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<project> -DOUT=<dir> -P cli_csv.cmake
file(MAKE_DIRECTORY "${OUT}")

function(first_bytes file out)
  file(READ "${file}" hex HEX LIMIT 6)
  set(${out} "${hex}" PARENT_SCOPE)
endfunction()

execute_process(
  COMMAND "${TIACONV}" -q --csv "${OUT}/hw.csv" --tags-csv "${OUT}/tags.csv" --blocks-csv "${OUT}/blocks.csv" "${PROJECT}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv failed: ${rc}")
endif()
# ef bb bf, then the first letters of the header line
first_bytes("${OUT}/hw.csv" hw)
first_bytes("${OUT}/tags.csv" tags)
first_bytes("${OUT}/blocks.csv" blocks)
if(NOT hw STREQUAL "efbbbf646576" OR NOT tags STREQUAL "efbbbf706c63" OR NOT blocks STREQUAL "efbbbf706c63")
  message(FATAL_ERROR "byte-order mark missing: ${hw} ${tags} ${blocks}")
endif()

execute_process(
  COMMAND "${TIACONV}" -q --no-bom --csv "${OUT}/hw2.csv" --tags-csv "${OUT}/tags2.csv" --blocks-csv "${OUT}/blocks2.csv" "${PROJECT}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --no-bom failed: ${rc}")
endif()
first_bytes("${OUT}/hw2.csv" hw)
first_bytes("${OUT}/tags2.csv" tags)
first_bytes("${OUT}/blocks2.csv" blocks)
if(NOT hw STREQUAL "646576696365" OR NOT tags STREQUAL "706c632c7461" OR NOT blocks STREQUAL "706c632c626c")
  message(FATAL_ERROR "--no-bom still writes something before the header: ${hw} ${tags} ${blocks}")
endif()

# standard output never gets the mark
execute_process(COMMAND "${TIACONV}" -q --tags-csv - "${PROJECT}" OUTPUT_VARIABLE piped RESULT_VARIABLE rc)
string(SUBSTRING "${piped}" 0 4 start)
if(NOT rc EQUAL 0 OR NOT start STREQUAL "plc,")
  message(FATAL_ERROR "unexpected start of CSV on standard output: ${start}")
endif()
