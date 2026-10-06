# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the executable with --save on the project in which a connection was
# made in save 62 and deleted in save 64, and checks that the report follows.
# Usage: cmake -DTIACONV=<exe> -DPROJECT=<project> -P cli_save.cmake
execute_process(COMMAND "${TIACONV}" --save 63 "${PROJECT}" OUTPUT_VARIABLE then RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tiaconv --save 63 failed: ${rc}")
endif()
if(NOT then MATCHES "S7_Connection_1 +S7 " OR NOT then MATCHES "shown as it was after save 63")
  message(FATAL_ERROR "--save 63 does not show the connection that existed then")
endif()

execute_process(COMMAND "${TIACONV}" --save 64 "${PROJECT}" OUTPUT_VARIABLE later RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR later MATCHES "S7_Connection_1 +S7 " OR NOT later MATCHES "S7_Connection_2 +S7 ")
  message(FATAL_ERROR "--save 64 should show only the second connection")
endif()

execute_process(COMMAND "${TIACONV}" --save 61 "${PROJECT}" OUTPUT_VARIABLE before RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR before MATCHES "\nConnections:")
  message(FATAL_ERROR "--save 61 should show no connections")
endif()

# a save the file does not have, and a value that is not a number
execute_process(COMMAND "${TIACONV}" --save 100000 "${PROJECT}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "--save beyond the last save should be a usage error, got ${rc}")
endif()
execute_process(COMMAND "${TIACONV}" --save x "${PROJECT}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "--save x should be a usage error, got ${rc}")
endif()
