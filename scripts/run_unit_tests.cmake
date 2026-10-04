cmake_minimum_required(VERSION 3.20)
# scripts/run_unit_tests.cmake
#
# Executes the registered ctest cases and refuses to report success for a build that
# contains (almost) no tests.
#
#   cmake -DBUILD_DIR=build -P scripts/run_unit_tests.cmake
#
#   BUILD_DIR       CMake build directory to test (default: ./build)
#   MIN_TESTS       minimum count (default: required suite count); required names
#                   are always checked, even with a lower explicit minimum.
#
# The case list is printed first, the count is checked, and only then is ctest
# executed with --output-on-failure. A quiet "no tests found" run can therefore never
# turn into a green pipeline step. Used by the build jobs in .github/workflows/ci.yml.

if(NOT DEFINED BUILD_DIR)
    set(BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/../build")
endif()
get_filename_component(BUILD_DIR "${BUILD_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${BUILD_DIR}")
    message(FATAL_ERROR "MCO-UNIT-TESTS: build directory not found: ${BUILD_DIR}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/McoExpectedUnitTests.cmake)
if(NOT DEFINED MIN_TESTS)
    list(LENGTH MCO_EXPECTED_UNIT_TESTS MIN_TESTS)
endif()
if(MIN_TESTS LESS 1)
    message(FATAL_ERROR "MCO-UNIT-TESTS: MIN_TESTS must be >= 1 (got ${MIN_TESTS})")
endif()

find_program(MCO_CTEST NAMES ctest REQUIRED)

message(STATUS "MCO-UNIT-TESTS: registered test cases in ${BUILD_DIR}")
execute_process(
    COMMAND ${MCO_CTEST} --test-dir "${BUILD_DIR}" -N
    OUTPUT_VARIABLE _list ERROR_VARIABLE _list_err
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _list_rc)
message(STATUS "${_list}\n${_list_err}")

if(NOT _list MATCHES "Total Tests: +([0-9]+)")
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: 'ctest -N' (exit=${_list_rc}) did not report a test count for "
        "${BUILD_DIR}. A build directory without ctest registration is not a pass; "
        "configure with -DBUILD_TESTING=ON (see cmake/McoTesting.cmake).")
endif()

set(_count "${CMAKE_MATCH_1}")
if(_count LESS MIN_TESTS)
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: only ${_count} test case(s) registered in ${BUILD_DIR}, "
        "expected at least ${MIN_TESTS}. A CI run that executes no test is not a pass "
        "(MCO_EXPECTED_UNIT_TESTS in cmake/McoExpectedUnitTests.cmake is the reference list).")
endif()
# Counts alone cannot detect a missing suite replaced by an unrelated test.
execute_process(
    COMMAND ${MCO_CTEST} --test-dir "${BUILD_DIR}" --show-only=json-v1
    OUTPUT_VARIABLE _json ERROR_VARIABLE _json_err RESULT_VARIABLE _json_rc)
if(NOT _json_rc EQUAL 0)
    message(FATAL_ERROR "MCO-UNIT-TESTS: cannot inspect registered suites: ${_json_err}")
endif()
string(JSON _length LENGTH "${_json}" tests)
set(_names "")
if(_length GREATER 0)
    math(EXPR _last "${_length} - 1")
    foreach(_index RANGE 0 ${_last})
        string(JSON _name GET "${_json}" tests ${_index} name)
        list(APPEND _names "${_name}")
    endforeach()
endif()
foreach(_required IN LISTS MCO_EXPECTED_UNIT_TESTS)
    if(NOT _required IN_LIST _names)
        message(FATAL_ERROR "MCO-UNIT-TESTS: required suite '${_required}' is not registered")
    endif()
endforeach()
message(STATUS "MCO-UNIT-TESTS: ${_count} case(s) registered, minimum is ${MIN_TESTS}")

execute_process(
    COMMAND ${MCO_CTEST} --test-dir "${BUILD_DIR}" --output-on-failure --timeout 120
    RESULT_VARIABLE _ctest_rc)
if(NOT _ctest_rc EQUAL 0)
    message(FATAL_ERROR "MCO-UNIT-TESTS: ctest failed with exit code ${_ctest_rc}")
endif()

message(STATUS "MCO-UNIT-TESTS: all ${_count} test case(s) passed")
