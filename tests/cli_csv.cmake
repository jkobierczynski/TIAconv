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
  COMMAND "${TIACONV}" -q --csv "${OUT}/hw.csv" --tags-csv "${OUT}/tags.csv" --blocks-csv "${OUT}/blocks.csv"
          --block-list-csv "${OUT}/list.csv" "${PROJECT}"
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

# the list of blocks: mark, header, and one row per block of the project
first_bytes("${OUT}/list.csv" list)
file(STRINGS "${OUT}/list.csv" rows)
list(LENGTH rows count)
list(GET rows 3 fb1)
if(NOT list STREQUAL "efbbbf706c63" OR NOT count EQUAL 10 OR NOT fb1 MATCHES "^ZZBRAVO,FB,1,Block_1,,,FBD,")
  message(FATAL_ERROR "unexpected list of blocks: ${list}, ${count} lines, ${fb1}")
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

# constants: on standard output, header first, hardware identifiers of the first PLC next
execute_process(COMMAND "${TIACONV}" -q --constants-csv - "${PROJECT}" OUTPUT_VARIABLE constants RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT constants MATCHES "^plc,kind,table,name,data_type,value,comment,stands_for,stands_for_device\r?\nZZALPHA,hardware,Default tag table,Local,Hw_SubModule,50,,ZZALPHA,S7-1200 station_1")
  message(FATAL_ERROR "unexpected constants CSV")
endif()

# HMI tags: a project without an HMI device gives the header and nothing else
execute_process(COMMAND "${TIACONV}" -q --hmi-tags-csv - "${PROJECT}" OUTPUT_VARIABLE hmi RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT hmi MATCHES "^hmi,table,name,data_type,access,connection,plc,plc_tag,address,acquisition_cycle,acquisition_mode,comment,start_value,members,plc_tag_found,plc_data_type,address_stored\r?\n$")
  message(FATAL_ERROR "unexpected HMI tags CSV: ${hmi}")
endif()

# standard output never gets the mark
execute_process(COMMAND "${TIACONV}" -q --tags-csv - "${PROJECT}" OUTPUT_VARIABLE piped RESULT_VARIABLE rc)
string(SUBSTRING "${piped}" 0 4 start)
if(NOT rc EQUAL 0 OR NOT start STREQUAL "plc,")
  message(FATAL_ERROR "unexpected start of CSV on standard output: ${start}")
endif()

# cross-reference: header, and the two calls in OB1 of the second PLC
execute_process(COMMAND "${TIACONV}" -q --xref-csv - "${PROJECT}" OUTPUT_VARIABLE xref RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT xref MATCHES "^plc,block,block_name,network,network_title,access,kind,item,data_type,data_block\r?\n"
   OR NOT xref MATCHES "\nZZBRAVO,OB1,Main,1,,call,block,\"\"\"Block_1\"\"\",Block_1,\r?\n")
  message(FATAL_ERROR "unexpected cross-reference CSV: ${xref}")
endif()
