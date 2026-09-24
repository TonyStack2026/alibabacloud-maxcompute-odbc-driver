#Requires -Version 5.1
<#
.SYNOPSIS
    Runs the registered ctest cases and refuses to report success for a build
    that contains (almost) no tests.
.DESCRIPTION
    Prints the case list, asserts that at least -MinTests cases are registered,
    and only then executes ctest. A zero-case or shrunken suite fails the step
    instead of printing a quiet "no tests found" pass.
.PARAMETER BuildDir
    CMake build directory to test.
.PARAMETER MinTests
    Minimum number of registered cases. Keep it in sync with
    MCO_EXPECTED_UNIT_TESTS in test/CMakeLists.txt.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [ValidateRange(1, 10000)][int]$MinTests = 5
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$ctest = (Get-Command ctest -ErrorAction Stop).Source

Write-Host "--- registered test cases in $BuildDir ---"
$listing = (& $ctest --test-dir $BuildDir -N 2>&1 | Out-String)
Write-Host $listing.Trim()

$match = [regex]::Match($listing, 'Total Tests:\s*(\d+)')
if (-not $match.Success) {
    Write-Host "ERROR: could not read a test count from 'ctest -N' output"
    exit 1
}

$registered = [int]$match.Groups[1].Value
if ($registered -lt $MinTests) {
    Write-Host "ERROR: only $registered test case(s) registered, expected at least $MinTests."
    Write-Host "A CI run that executes no test is not a pass; see cmake/McoTesting.cmake."
    exit 1
}
Write-Host "--- $registered test case(s) registered (minimum $MinTests) ---"

& $ctest --test-dir $BuildDir --output-on-failure --timeout 120
$ctestExit = $LASTEXITCODE
if ($ctestExit -ne 0) {
    Write-Host "ERROR: ctest failed with exit code $ctestExit"
    exit $ctestExit
}

Write-Host "--- all $registered test case(s) passed ---"
