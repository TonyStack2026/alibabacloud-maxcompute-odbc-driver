# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `BUILD_TESTING` build modes for the C++ unit-test suite: with the default `ON`,
  configuring fails when Google Test is unavailable or when a testing build
  registers no test case, instead of silently skipping the whole suite;
  `-DBUILD_TESTING=OFF` is the explicit build-only mode and says so in the log.
- `test/gate-check` fixture plus `scripts/check_test_gate.cmake`, which verify that
  the gate fails closed without needing the driver's dependencies. Both harnesses are
  CMake scripts, so they run unchanged on Linux, macOS and Windows and need nothing
  but CMake to reproduce.
- CI executes `ctest` on Linux, macOS and Windows through
  `scripts/run_unit_tests.cmake` and asserts the number of registered cases; a
  dedicated job checks the gate itself on Linux and Windows.

### Changed
- `logging_test` now uses a per-case file under the platform temp directory
  instead of the hard-coded `/tmp/mco_test_log.txt`, so it runs on Windows too and
  no longer needs to be excluded from `ctest`.

## [1.0.0] - 2025-03-11

### Added
- Initial open-source release of MaxCompute ODBC Driver.
- ODBC 3.x specification support with both ANSI and Wide (Unicode) API variants.
- Core ODBC functions: handle management, connection, query execution, result set navigation, catalog functions, diagnostics.
- `SQLDriverConnect` / `SQLConnect` for establishing connections to MaxCompute.
- `SQLExecDirect` / `SQLPrepare` / `SQLExecute` for query execution.
- `SQLFetch` / `SQLGetData` / `SQLBindCol` for result set retrieval.
- `SQLTables` / `SQLColumns` for metadata queries.
- MaxCompute Tunnel integration for high-throughput data download with concurrent prefetching.
- Protobuf V6 wire format deserialization with CRC32-C checksum validation.
- MaxQA (MaxCompute Query Acceleration) interactive mode support.
- HMAC-based request signing for MaxCompute REST API authentication.
- AccessKey ID/Secret and STS Token authentication.
- Environment variable support for credentials (`ALIBABA_CLOUD_ACCESS_KEY_ID`, etc.).
- HTTP/HTTPS proxy configuration support with system proxy auto-detection.
- `SET key=value;` SQL prefix parsing for per-query settings.
- Cross-platform support: Windows (DLL), macOS (dylib), Linux (SO).
- Configurable logging with `MCO_LOG_LEVEL` and `MCO_LOG_FILE` environment variables.
- Windows MSI installer via WiX Toolset.
- Unit test suite (GTest) and Python E2E test suite (pyodbc).
- GitHub Actions CI/CD for Windows and macOS builds.
- Apache 2.0 license.

### Known Limitations
- Parameter binding (`SQLBindParameter`) is not yet implemented.
- `SQLFetchScroll` / `SQLSetPos` / `SQLBulkOperations` are not supported.
- `SQLGetTypeInfo`, `SQLSpecialColumns`, `SQLStatistics`, `SQLPrimaryKeys`, `SQLForeignKeys` are not yet implemented.
- `SQLEndTran` is not supported (MaxCompute is an analytical engine without transaction semantics).
