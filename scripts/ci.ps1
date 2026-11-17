# SPDX-License-Identifier: GPL-3.0-only
# Keep detailed diagnostics on the runner only. Reproduce a failed stage with
# scripts/verify.ps1 locally; never upload SDKs, executables or diagnostic dumps.
[CmdletBinding()]
param([switch]$ReleaseOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$log = Join-Path $temporaryRoot ('wav-player-ci-' + [Guid]::NewGuid().ToString('N') + '.log')
$engine = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$verificationArguments = @('-Analyze', '-IncludeASan')
$verificationName = 'Build, analysis and native tests'
if ($ReleaseOnly) {
    $verificationArguments = @('-Configuration', 'Release')
    $verificationName = 'Release-only diagnostic build and native tests'
    Write-Output 'Verification mode: Release-only diagnostics (no static analysis or ASan)'
} else {
    Write-Output 'Verification mode: full (static analysis, Release and ASan)'
}

# Reconstruct diagnostic lines from bounded fields, rather than publishing a
# child's exception text, paths, driver diagnostics or arbitrary output.
function Write-PublicDiagnostic([string]$Line) {
    if ($Line -match '^\d+ suites in [\d.]+ s: \d+ passed, \d+ failed, \d+ timed out, \d+ crashed\.$' -or
        $Line -match '^== (Build|Test) (Debug|Release|Debug-ASan)\|x64 ==$' -or
        $Line -match '^Toolset: MSVC [\d.]+, Windows SDK [\d.]+$') {
        Write-Output $Line
    } elseif ($Line -match '^== (?<suite>[a-z][a-z0-9_]{0,63}) ==$') {
        Write-Output ('== ' + $Matches.suite + ' ==')
    } elseif ($Line -match '^(?<verdict>PASS|FAIL|TIMEOUT|CRASH)\s+(?<suite>[a-z][a-z0-9_]{0,63})\s+(?<seconds>\d+(?:\.\d+)?) s(?:\s+.*)?$') {
        Write-Output ('{0} {1} {2} s' -f $Matches.verdict, $Matches.suite, $Matches.seconds)
    } elseif ($Line -match '^\[FAIL\] (?<case>[A-Za-z0-9][A-Za-z0-9 ,.''()/_+<>=-]{0,239}): ') {
        $message = '[FAIL] ' + $Matches.case
        if ($Line -match ': expected (?<expected>-?\d+|true|false), got (?<actual>-?\d+|true|false)(?: \([A-Za-z0-9_-]+\.(?:cpp|h):\d+\))?$') {
            $message += ': expected ' + $Matches.expected + ', got ' + $Matches.actual
        }
        if ($Line -match ' \((?<file>[A-Za-z0-9_-]+\.(?:cpp|h)):(?<line>\d+)\)$') {
            $message += ' (' + $Matches.file + ':' + $Matches.line + ')'
        }
        Write-Output $message
    }
}
$steps = @(
    @{ Name = 'Public source files'; File = 'check-public-source.ps1'; Arguments = @('-Tracked') },
    @{ Name = 'Source publication tests'; File = 'tests/source-publication.tests.ps1'; Arguments = @() },
    @{ Name = 'SDK download and integrity'; File = 'fetch-asio-sdk.ps1'; Arguments = @() },
    @{ Name = $verificationName; File = 'verify.ps1'; Arguments = $verificationArguments },
    @{ Name = 'Player setup tests'; File = 'tests/player-setup.tests.ps1'; Arguments = @() }
)
try {
    foreach ($step in $steps) {
        Write-Output ("== {0} ==" -f $step.Name)
        $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
            (Join-Path $PSScriptRoot $step.File)) + $step.Arguments
        # PowerShell 5.1 may turn native stderr into ErrorRecords. Temporarily
        # collect these as data; the child exit code remains authoritative.
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $engine @arguments 2>&1 | ForEach-Object {
                $line = [string]$_
                Add-Content -LiteralPath $log -Value $line -Encoding UTF8 -ErrorAction Stop
                Write-PublicDiagnostic $line
            }
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($code -ne 0) {
            throw "$($step.Name) failed (exit $code). Run the corresponding script locally for detailed diagnostics."
        }
        Write-Output ("[PASS] {0}" -f $step.Name)
    }
} finally {
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log }
}
