# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs `tiaconv diff` on the test project and checks the exit codes and the
# start of the outputs.
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<s14_code> -DREWRITTEN=<s14_code_rewritten> -DOUT=<dir> -P cli_diff.cmake
file(MAKE_DIRECTORY "${OUT}")

# the same save twice: no difference, exit code 0 also with --exit-code
execute_process(COMMAND "${TIACONV}" diff --exit-code --old-save 20 --new-save 20 "${PROJECT}" "${PROJECT}"
                OUTPUT_VARIABLE same RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT same MATCHES "No differences in what tiaconv reports")
  message(FATAL_ERROR "diff of a save with itself: ${rc} ${same}")
endif()

# a rename: exit code 1 with --exit-code, 0 without
execute_process(COMMAND "${TIACONV}" diff --exit-code --old-save 20 --new-save 21 "${PROJECT}" "${PROJECT}"
                OUTPUT_VARIABLE renamed RESULT_VARIABLE rc)
if(NOT rc EQUAL 1 OR NOT renamed MATCHES "~ tag ZZPLC / Default tag table / ZZALPHA: name: ZZA -> ZZALPHA")
  message(FATAL_ERROR "diff with a rename: ${rc} ${renamed}")
endif()
execute_process(COMMAND "${TIACONV}" diff -q --old-save 20 --new-save 21 "${PROJECT}" "${PROJECT}"
                OUTPUT_VARIABLE quiet RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT quiet STREQUAL "")
  message(FATAL_ERROR "diff -q: ${rc} ${quiet}")
endif()

# JSON and CSV on standard output, the text report then goes to standard error
execute_process(COMMAND "${TIACONV}" diff -j - --old-save 19 "${PROJECT}" "${REWRITTEN}"
                OUTPUT_VARIABLE json ERROR_VARIABLE text RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT json MATCHES "^\\{\n  \"tool\": \\{\"name\": \"tiaconv\"" OR NOT json MATCHES "\"same_lineage\": true"
   OR NOT json MATCHES "\"substantial_changes\": 5" OR NOT text MATCHES "^Old: ")
  message(FATAL_ERROR "diff JSON: ${rc} ${json}")
endif()
execute_process(COMMAND "${TIACONV}" diff -q --csv - --old-save 19 "${PROJECT}" "${REWRITTEN}"
                OUTPUT_VARIABLE csv RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT csv MATCHES "^change,kind,item,part_of,attribute,from,to,description,time_stamp\r?\n"
   OR NOT csv MATCHES "\nadded,network,ZZPLC / Main \\[OB1\\] / network 2,,,,\"1: \"\"ZZOUT\"\" := \"\"ZZC\"\"\",\"ZZ inserted, LAD\",\r?\n")
  message(FATAL_ERROR "diff CSV: ${rc} ${csv}")
endif()

# mistakes
execute_process(COMMAND "${TIACONV}" diff "${PROJECT}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "diff needs two projects")
  message(FATAL_ERROR "diff with one project: ${rc} ${err}")
endif()
execute_process(COMMAND "${TIACONV}" diff --new-save 99 "${PROJECT}" "${PROJECT}" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 1 OR NOT err MATCHES "records 23 saves, not 99")
  message(FATAL_ERROR "diff with a save that is not there: ${rc} ${err}")
endif()
