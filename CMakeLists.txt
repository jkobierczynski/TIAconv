cmake_minimum_required(VERSION 3.16)
project(tiaconv VERSION 0.13.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TIACONV_STATIC "Link the executable statically where the platform allows it" OFF)

# With Visual C++ a static build needs the static runtime library, and every
# target has to agree on it: the libraries, the executable and the tests.
if(MSVC AND TIACONV_STATIC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# miniz (MIT): inflate for the embedded type model, zip for project archives.
add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_ARCHIVE_WRITING_APIS)

add_library(tiacore STATIC
  src/container.cpp
  src/history.cpp
  src/inventory.cpp
  src/meta.cpp
  src/output.cpp
  src/program.cpp
  src/code.cpp
  src/project.cpp
  src/sha256.cpp
  src/source.cpp
  src/xml.cpp)
target_include_directories(tiacore PUBLIC src)
target_link_libraries(tiacore PUBLIC miniz)

if(MSVC)
  target_compile_options(tiacore PRIVATE /W4 /utf-8)
else()
  target_compile_options(tiacore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tiaconv src/main.cpp)
target_link_libraries(tiaconv PRIVATE tiacore)
target_compile_definitions(tiaconv PRIVATE TIACONV_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(tiaconv PRIVATE /W4 /utf-8)
else()
  target_compile_options(tiaconv PRIVATE -Wall -Wextra -Wpedantic)
  if(TIACONV_STATIC AND NOT APPLE)
    target_link_options(tiaconv PRIVATE -static)
  endif()
endif()

install(TARGETS tiaconv RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_executable(tiaconv_tests tests/tests.cpp)
  target_link_libraries(tiaconv_tests PRIVATE tiacore)
  add_test(NAME unit COMMAND tiaconv_tests)

  add_executable(tiaconv_fixture_tests tests/fixtures_test.cpp)
  target_link_libraries(tiaconv_fixture_tests PRIVATE tiacore)
  target_compile_definitions(tiaconv_fixture_tests PRIVATE
    TIACONV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME fixtures COMMAND tiaconv_fixture_tests)
  if(MSVC)
    target_compile_options(tiaconv_tests PRIVATE /utf-8)
    target_compile_options(tiaconv_fixture_tests PRIVATE /utf-8)
  endif()

  add_test(NAME cli_csv COMMAND ${CMAKE_COMMAND}
    -DTIACONV=$<TARGET_FILE:tiaconv>
    -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/s08_program
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_csv
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_csv.cmake)

  add_test(NAME cli_diff COMMAND ${CMAKE_COMMAND}
    -DTIACONV=$<TARGET_FILE:tiaconv>
    -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/s14_code
    -DREWRITTEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/s14_code_rewritten
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_diff
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_diff.cmake)

  add_test(NAME cli_save COMMAND ${CMAKE_COMMAND}
    -DTIACONV=$<TARGET_FILE:tiaconv>
    -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/s10_connections
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_save.cmake)

  add_test(NAME cli_history COMMAND ${CMAKE_COMMAND}
    -DTIACONV=$<TARGET_FILE:tiaconv>
    -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/s10_connections
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cli_history
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_history.cmake)
endif()
