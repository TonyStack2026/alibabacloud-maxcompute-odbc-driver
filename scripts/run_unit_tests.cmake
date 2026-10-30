# scripts/run_unit_tests.cmake
#
# Executes the ctest cases of a build directory and refuses to report success when
# the build does not actually contain the suites the project claims to test.
#
#   cmake -DBUILD_DIR=build -P scripts/run_unit_tests.cmake
#
#   BUILD_DIR       CMake build directory to test (default: ./build)
#   EXPECTED_FROM   file that declares MCO_EXPECTED_UNIT_TESTS (default:
#                   ../test/CMakeLists.txt next to this script). The required suite
#                   names are read from there at run time, so the list lives in one
#                   place instead of being duplicated as a number in the workflows.
#   MIN_TESTS       optional extra floor. Default: the number of names parsed out of
#                   EXPECTED_FROM -- deliberately not hardcoded here or in CI.
#
# Two checks, both fatal: every required suite name must show up in `ctest -N`, and
# the registered count must reach MIN_TESTS. The first one is what catches a missing
# or renamed add_test() line that a count alone would wave through (registering an
# unrelated case keeps the count identical). Used by the build jobs in
# .github/workflows/ci.yml and by the release workflow.

if(NOT DEFINED BUILD_DIR)
    set(BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/../build")
endif()
get_filename_component(BUILD_DIR "${BUILD_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${BUILD_DIR}")
    message(FATAL_ERROR "MCO-UNIT-TESTS: build directory not found: ${BUILD_DIR}")
endif()

if(NOT DEFINED EXPECTED_FROM)
    set(EXPECTED_FROM "${CMAKE_CURRENT_LIST_DIR}/../test/CMakeLists.txt")
endif()
get_filename_component(EXPECTED_FROM "${EXPECTED_FROM}" ABSOLUTE)
if(NOT EXISTS "${EXPECTED_FROM}")
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: the required-suite source does not exist: ${EXPECTED_FROM}\n"
        "Without that list this script could only count cases, and a count is not a contract.")
endif()

file(READ "${EXPECTED_FROM}" _mco_expected_source)
if(NOT _mco_expected_source MATCHES "set\\(MCO_EXPECTED_UNIT_TESTS([^)]*)\\)")
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: no MCO_EXPECTED_UNIT_TESTS block found in ${EXPECTED_FROM}; "
        "refusing to run a test gate with an unknown required-suite list.")
endif()
string(REPLACE "\n" ";" _mco_expected_lines "${CMAKE_MATCH_1}")
set(_mco_required "")
foreach(_mco_line IN LISTS _mco_expected_lines)
    string(REGEX REPLACE "^ +| +$" "" _mco_line "${_mco_line}")
    if(_mco_line AND NOT _mco_line MATCHES "^#")
        list(APPEND _mco_required "${_mco_line}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _mco_required)
list(LENGTH _mco_required _mco_required_count)
if(_mco_required_count LESS 1)
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: MCO_EXPECTED_UNIT_TESTS in ${EXPECTED_FROM} parsed out empty; "
        "a gate with no names would pass on any build.")
endif()

if(NOT DEFINED MIN_TESTS)
    set(MIN_TESTS "${_mco_required_count}")
endif()
if(MIN_TESTS LESS 1)
    message(FATAL_ERROR "MCO-UNIT-TESTS: MIN_TESTS must be >= 1 (got ${MIN_TESTS})")
endif()

find_program(MCO_CTEST NAMES ctest REQUIRED)

message(STATUS "MCO-UNIT-TESTS: ${_mco_required_count} required suite(s) from ${EXPECTED_FROM}")
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

# Collect the registered case names.
string(REPLACE "\n" ";" _mco_list_lines "${_list}")
set(_mco_registered "")
foreach(_mco_line IN LISTS _mco_list_lines)
    if(_mco_line MATCHES "Test +#[0-9]+: +([^ \r\n\t]+)")
        list(APPEND _mco_registered "${CMAKE_MATCH_1}")
    endif()
endforeach()

set(_mco_missing "")
foreach(_mco_name IN LISTS _mco_required)
    # list(FIND) rather than `IN_LIST`: script mode (-P) has no
    # cmake_minimum_required() to bring policy CMP0057 to NEW, where IN_LIST would
    # be a hard "Unknown arguments specified" error instead of a check.
    list(FIND _mco_registered "${_mco_name}" _mco_found)
    if(_mco_found LESS 0)
        string(APPEND _mco_missing " ${_mco_name}")
    endif()
endforeach()
if(_mco_missing)
    list(JOIN _mco_registered ", " _mco_seen)
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: required suite(s) not present in the ctest registration of "
        "${BUILD_DIR}:${_mco_missing}\n"
        "  registered cases (${_count}): ${_mco_seen}\n"
        "A count that happens to match is not evidence: an unrelated case registered in place "
        "of a required one would pass a count-only check.")
endif()

if(_count LESS MIN_TESTS)
    message(FATAL_ERROR
        "MCO-UNIT-TESTS: only ${_count} test case(s) registered in ${BUILD_DIR}, "
        "expected at least ${MIN_TESTS}. A CI run that executes no test is not a pass.")
endif()
message(STATUS "MCO-UNIT-TESTS: ${_count} case(s) registered, minimum is ${MIN_TESTS}")

execute_process(
    COMMAND ${MCO_CTEST} --test-dir "${BUILD_DIR}" --output-on-failure --timeout 120
    RESULT_VARIABLE _ctest_rc)
if(NOT _ctest_rc EQUAL 0)
    message(FATAL_ERROR "MCO-UNIT-TESTS: ctest failed with exit code ${_ctest_rc}")
endif()

message(STATUS "MCO-UNIT-TESTS: all ${_count} test case(s) passed")
