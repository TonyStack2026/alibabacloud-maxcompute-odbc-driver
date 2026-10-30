# test/gate-check/CMakeLists.txt
#
# Standalone CMake project that proves the unit-test gate (cmake/McoTesting.cmake)
# fails closed. It compiles no product source and needs no vcpkg dependency, so a
# scenario costs one configure and runs in seconds on any machine that has CMake.
#
# Each scenario is one way a "testing build" can be hollow; the first two are
# dependency/registration failures, the last two are the shapes a count cannot see:
#
#   missing-dep          Google Test unavailable            -> configure MUST fail
#   zero-cases           dependency fine, nothing registered-> configure MUST fail
#   registered           one expected name, one case        -> configure MUST pass
#   build-only           BUILD_TESTING=OFF                  -> MUST pass, 0 cases registered
#   unregistered-suite   target + name present, add_test gone -> configure MUST fail by name
#   substituted          3 required names, 3 registered cases but one is unrelated
#                                                             -> configure MUST fail by name
#
# scripts/check_test_gate.cmake drives these scenarios with those expectations and is
# what CI runs on Linux and Windows (see the test-gate job in .github/workflows/ci.yml);
# it also runs scripts/run_unit_tests.cmake against the `registered` directory once with
# the right names and once with a name that is not there, to prove the runner checks names
# and not just counts.
cmake_minimum_required(VERSION 3.20)
# NONE: the fixture compiles nothing, it only exercises the gate logic.
project(mco_test_gate_check NONE)

set(MCO_GATE_SCENARIO
    ""
    CACHE STRING
    "Scenario to exercise: missing-dep | zero-cases | registered | build-only | unregistered-suite | substituted")

if(NOT MCO_GATE_SCENARIO MATCHES "^(missing-dep|zero-cases|registered|build-only|unregistered-suite|substituted)$")
    message(FATAL_ERROR
        "MCO-GATE-CHECK: MCO_GATE_SCENARIO must be one of missing-dep, zero-cases, "
        "registered, build-only, unregistered-suite, substituted "
        "(got '${MCO_GATE_SCENARIO}')")
endif()

# Keep the scenario and BUILD_TESTING in sync, otherwise a harness typo would
# "pass" for the wrong reason.
if(MCO_GATE_SCENARIO STREQUAL "build-only")
    if(BUILD_TESTING)
        message(FATAL_ERROR "MCO-GATE-CHECK: scenario 'build-only' needs -DBUILD_TESTING=OFF")
    endif()
elseif(NOT BUILD_TESTING)
    message(FATAL_ERROR
        "MCO-GATE-CHECK: scenario '${MCO_GATE_SCENARIO}' needs -DBUILD_TESTING=ON")
endif()

if(MCO_GATE_SCENARIO STREQUAL "missing-dep")
    # Hide Google Test even from a host that has it installed: this is the
    # situation the old QUIET find_package used to shrug off.
    set(CMAKE_DISABLE_FIND_PACKAGE_GTest TRUE)
elseif(MCO_GATE_SCENARIO MATCHES "^(zero-cases|registered|unregistered-suite|substituted)$")
    # Represent "the test dependency is available" without depending on the host.
    # Nothing here links against it; the gate only requires the imported target.
    add_library(GTest::gtest INTERFACE IMPORTED GLOBAL)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/McoTesting.cmake)

mco_testing_begin()

# The required-suite contract needs names, so every scenario declares its own list
# before the gate's end-check runs.
if(MCO_GATE_SCENARIO STREQUAL "registered")
    set(MCO_EXPECTED_UNIT_TESTS gate_probe)
elseif(MCO_GATE_SCENARIO STREQUAL "unregistered-suite" OR MCO_GATE_SCENARIO STREQUAL "substituted")
    set(MCO_EXPECTED_UNIT_TESTS alpha_probe beta_probe gamma_probe)
    add_custom_target(alpha_probe)
    add_custom_target(beta_probe)
    add_custom_target(gamma_probe)
endif()

if(MCO_BUILD_TESTS AND MCO_GATE_SCENARIO STREQUAL "registered")
    add_test(NAME gate_probe COMMAND ${CMAKE_COMMAND} -E echo "MCO-GATE-CHECK probe")
elseif(MCO_BUILD_TESTS AND MCO_GATE_SCENARIO STREQUAL "unregistered-suite")
    # Reproduction of the report in this repository's issue #8, step 2-3: the gamma
    # target exists and the name is still in the list, only its add_test() is gone.
    add_test(NAME alpha_probe COMMAND ${CMAKE_COMMAND} -E echo alpha)
    add_test(NAME beta_probe COMMAND ${CMAKE_COMMAND} -E echo beta)
elseif(MCO_BUILD_TESTS AND MCO_GATE_SCENARIO STREQUAL "substituted")
    # Same report, step 4: the count is identical to the required list, but one of the
    # registered cases is an unrelated name -- a count-only check passes this.
    add_test(NAME alpha_probe COMMAND ${CMAKE_COMMAND} -E echo alpha)
    add_test(NAME beta_probe COMMAND ${CMAKE_COMMAND} -E echo beta)
    add_test(NAME unrelated_probe COMMAND ${CMAKE_COMMAND} -E echo unrelated)
endif()

mco_testing_end()
