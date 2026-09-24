#Requires -Version 5.1
<#
.SYNOPSIS
    Verifies that the unit-test gate (cmake/McoTesting.cmake) fails closed.
.DESCRIPTION
    Configures the dependency-free fixture in test/gate-check once per scenario
    and asserts the behaviour the gate promises:

      missing-dep   Google Test unavailable, BUILD_TESTING=ON   -> configure MUST fail
      zero-cases    Google Test available, nothing registered   -> configure MUST fail
      registered    Google Test available, 1 case registered    -> configure MUST pass, ctest sees 1 case
      build-only    BUILD_TESTING=OFF                           -> configure MUST pass, ctest sees 0 cases

    No product source is compiled and no vcpkg dependency is needed, so all four
    scenarios cost a few seconds. CI runs it on Linux and Windows
    (.github/workflows/ci.yml, job test-gate).
.PARAMETER RepoRoot
    Repository root. Defaults to the parent directory of this script.
.PARAMETER WorkDir
    Where the throwaway configure directories are created.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'mco-test-gate')
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$fixture = Join-Path $RepoRoot 'test/gate-check'
if (-not (Test-Path -LiteralPath $fixture)) { throw "fixture not found: $fixture" }
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = (Get-Command ctest -ErrorAction Stop).Source

$scenarios = @(
    [pscustomobject]@{ Name = 'missing-dep'; BuildTesting = 'ON'; ExpectSuccess = $false;
        Marker = 'Google Test was not found'; ExpectTestCount = $null }
    [pscustomobject]@{ Name = 'zero-cases'; BuildTesting = 'ON'; ExpectSuccess = $false;
        Marker = 'no test case was registered'; ExpectTestCount = $null }
    [pscustomobject]@{ Name = 'registered'; BuildTesting = 'ON'; ExpectSuccess = $true;
        Marker = 'ctest case(s) registered'; ExpectTestCount = 1 }
    [pscustomobject]@{ Name = 'build-only'; BuildTesting = 'OFF'; ExpectSuccess = $true;
        Marker = 'unit tests are NOT built, NOT registered and NOT run'; ExpectTestCount = 0 }
)

function Get-RegisteredTestCount {
    param([string]$BuildDir)

    $listing = (& $ctest --test-dir $BuildDir -N 2>&1 | Out-String)
    $match = [regex]::Match($listing, 'Total Tests:\s*(\d+)')
    if (-not $match.Success) { return $null }
    return [int]$match.Groups[1].Value
}

$failures = [Collections.Generic.List[string]]::new()

foreach ($scenario in $scenarios) {
    $buildDir = Join-Path $WorkDir $scenario.Name
    if (Test-Path -LiteralPath $buildDir) { Remove-Item -Recurse -Force -LiteralPath $buildDir }
    $null = New-Item -ItemType Directory -Force -Path $buildDir

    $output = (& $cmake -S $fixture -B $buildDir `
        "-DMCO_GATE_SCENARIO=$($scenario.Name)" `
        "-DBUILD_TESTING=$($scenario.BuildTesting)" 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    $succeeded = ($exitCode -eq 0)

    $problems = [Collections.Generic.List[string]]::new()
    if ($succeeded -ne $scenario.ExpectSuccess) {
        $problems.Add("configure exit=$exitCode, expected success=$($scenario.ExpectSuccess)")
    }
    if (-not $output.Contains($scenario.Marker)) {
        $problems.Add("configure log is missing the gate message: $($scenario.Marker)")
    }
    if ($null -ne $scenario.ExpectTestCount) {
        $registered = Get-RegisteredTestCount -BuildDir $buildDir
        if ($registered -ne $scenario.ExpectTestCount) {
            $problems.Add("ctest reported '$registered' case(s), expected $($scenario.ExpectTestCount)")
        }
    }

    if ($problems.Count -eq 0) {
        Write-Host "[gate] $($scenario.Name): OK"
    }
    else {
        Write-Host "[gate] $($scenario.Name): FAILED"
        Write-Host ($output.Trim())
        foreach ($problem in $problems) { Write-Host "       - $problem" }
        $failures.Add($scenario.Name)
    }
}

if ($failures.Count -gt 0) {
    Write-Host "unit-test gate check FAILED for: $($failures -join ', ')"
    exit 1
}

Write-Host "unit-test gate check passed: all $($scenarios.Count) scenarios behave as specified"
