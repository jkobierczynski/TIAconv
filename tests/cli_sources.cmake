# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the executable with --sources on s08_program and compares the source
# of FB "Block_2" with the one TIA Portal generated for it (the first block
# of exports/source_from_blocks.awl, which has no byte-order mark).
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<project> -DOUT=<dir> -P cli_sources.cmake
file(REMOVE_RECURSE "${OUT}")
execute_process(COMMAND "${TIACONV}" -q --no-bom --sources "${OUT}" "${PROJECT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --sources failed: ${rc}")
endif()
set(ours "${OUT}/ZZBRAVO/Program blocks/Block_2.awl")
if(NOT EXISTS "${ours}")
  message(FATAL_ERROR "no ${ours}")
endif()
file(READ "${ours}" got HEX)
file(READ "${PROJECT}/exports/source_from_blocks.awl" theirs HEX)
string(FIND "${theirs}" "454e445f46554e4354494f4e5f424c4f434b0d0a0d0a" at)  # END_FUNCTION_BLOCK CR LF CR LF
if(at LESS 0)
  message(FATAL_ERROR "the export has no END_FUNCTION_BLOCK")
endif()
math(EXPR len "${at} + 44")
string(SUBSTRING "${theirs}" 0 ${len} expected)
if(NOT got STREQUAL expected)
  message(FATAL_ERROR "Block_2.awl differs from TIA Portal's source")
endif()
foreach(f "ZZALPHA/Program blocks/Main.txt" "ZZBRAVO/Program blocks/Main.txt" "ZZBRAVO/Program blocks/Block_1.txt")
  if(NOT EXISTS "${OUT}/${f}")
    message(FATAL_ERROR "no ${f}")
  endif()
endforeach()

# standard output is not a folder
execute_process(COMMAND "${TIACONV}" -q --sources - "${PROJECT}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "--sources - should be a usage error, got ${rc}")
endif()
