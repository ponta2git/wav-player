# SPDX-License-Identifier: GPL-3.0-only
# Keep detailed diagnostics on the runner only. Reproduce a failed stage with
# scripts/verify.ps1 locally; never upload SDKs, executables or diagnostic dumps.
# Each step runs in its own Windows PowerShell process; its exit code decides the
# step, and only lines rebuilt by ConvertTo-PublicDiagnostic reach the public log.
[CmdletBinding()]
param([switch]$ReleaseOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Reconstruct diagnostic lines from bounded fields, rather than publishing a
# child's exception text, paths, driver diagnostics or arbitrary output. Returns
# the public form of a line, or nothing for a line that has none.
function ConvertTo-PublicDiagnostic([string]$Line) {
    if ($Line -match '^\d+ suites in [\d.]+ s: \d+ passed, \d+ failed, \d+ timed out, \d+ crashed\.$' -or
        $Line -match '^== (Build|Test) (Debug|Release|Debug-ASan)\|x64 ==$' -or
        $Line -match '^Toolset: MSVC [\d.]+, Windows SDK [\d.]+$') {
        return $Line
    }
    if ($Line -match '^== (?<suite>[a-z][a-z0-9_]{0,63}) ==$') {
        return '== ' + $Matches.suite + ' =='
    }
    if ($Line -match '^(?<verdict>PASS|FAIL|TIMEOUT|CRASH)\s+(?<suite>[a-z][a-z0-9_]{0,63})\s+(?<seconds>\d+(?:\.\d+)?) s(?:\s+.*)?$') {
        return '{0} {1} {2} s' -f $Matches.verdict, $Matches.suite, $Matches.seconds
    }
    if ($Line -match '^\[FAIL\] (?<case>[A-Za-z0-9][A-Za-z0-9 ,.''()/_+<>=-]{0,239}): ') {
        return ConvertTo-PublicFailure $Line $Matches.case $Line.Substring($Matches[0].Length)
    }
}

# The public form of a failed case: its name, then only what matches a fixed
# schema of Detail, the text after the case name.
function ConvertTo-PublicFailure([string]$Line, [string]$Case, [string]$Detail) {
    $message = '[FAIL] ' + $Case
    $exception = $null
    # Only known standard types are public; arbitrary RTTI names and what()
    # text can contain private fixture details.
    if ($Detail -cmatch '^(?:class |struct )?(?<exception>std::(?:runtime_error|logic_error|out_of_range|invalid_argument|length_error|domain_error|range_error|overflow_error|underflow_error|system_error|filesystem::filesystem_error|bad_alloc|bad_array_new_length|bad_cast|bad_typeid|exception)): ') {
        $exception = $Matches.exception
        $message += ': exception=' + $exception
        $Detail = $Detail.Substring($Matches[0].Length)
    }
    # A GUI send failure has no payload or HWND in its public diagnostic.
    # Match the entire fixed schema, then format only valid UINT/DWORD values.
    if ($exception -ceq 'std::runtime_error' -and
        $Detail -cmatch '^gui_send_failed message=(?<messageId>[0-9]{1,10}) win32=(?<win32>[0-9]{1,10})(?: \([A-Za-z0-9_-]+\.(?:cpp|h):[0-9]+\))?$') {
        [uint32]$messageId = 0
        [uint32]$win32 = 0
        if ([uint32]::TryParse($Matches.messageId, [ref]$messageId) -and
            [uint32]::TryParse($Matches.win32, [ref]$win32)) {
            $message += '; gui_send_failed message=' + $messageId + ' win32=' + $win32
        }
    }
    if ($Line -match ': expected (?<expected>-?\d+|true|false), got (?<actual>-?\d+|true|false)(?: \([A-Za-z0-9_-]+\.(?:cpp|h):\d+\))?$') {
        $message += ': expected ' + $Matches.expected + ', got ' + $Matches.actual
    }
    if ($Line -match ' \((?<file>[A-Za-z0-9_-]+\.(?:cpp|h)):(?<line>\d+)\)$') {
        $message += ' (' + $Matches.file + ':' + $Matches.line + ')'
    }
    $message
}

# Runs one script of this directory in a child process, writes every output line
# to Log and the reconstructed lines to the pipeline, and returns nothing else.
# Fails when the child exits with a nonzero code.
function Invoke-Step([hashtable]$Step, [IO.TextWriter]$Log) {
    Write-Output ("== {0} ==" -f $Step.Name)
    $engine = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $PSScriptRoot $Step.File)) + $Step.Arguments
    # PowerShell 5.1 may turn native stderr into ErrorRecords. Collect these as
    # data for this call only; the child exit code remains authoritative. Under
    # that preference a failed log write would merely end the pipeline, so it is
    # rethrown: throw stops the script under every preference.
    $code = $null
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $engine @arguments 2>&1 | ForEach-Object {
            $line = [string]$_
            try { $Log.WriteLine($line) } catch { throw 'Cannot write the detailed log on the runner.' }
            ConvertTo-PublicDiagnostic $line
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($code -ne 0) {
        throw "$($Step.Name) failed (exit $code). Run the corresponding script locally for detailed diagnostics."
    }
    Write-Output ("[PASS] {0}" -f $Step.Name)
}

$verification = @{ Name = 'Build, analysis and native tests'; File = 'verify.ps1'; Arguments = @('-Analyze', '-IncludeASan') }
if ($ReleaseOnly) {
    $verification = @{ Name = 'Release-only diagnostic build and native tests'; File = 'verify.ps1'; Arguments = @('-Configuration', 'Release') }
    Write-Output 'Verification mode: Release-only diagnostics (no static analysis or ASan)'
} else {
    Write-Output 'Verification mode: full (static analysis, Release and ASan)'
}
$steps = @(
    @{ Name = 'Public source files'; File = 'check-public-source.ps1'; Arguments = @('-Tracked') },
    @{ Name = 'Source publication tests'; File = 'tests/source-publication.tests.ps1'; Arguments = @() },
    @{ Name = 'SDK download and integrity'; File = 'fetch-asio-sdk.ps1'; Arguments = @() },
    $verification,
    @{ Name = 'Player setup tests'; File = 'tests/player-setup.tests.ps1'; Arguments = @() }
)

# The detailed log exists only while this script runs: it is removed on success
# and on failure alike, and survives only a run that was killed.
$temporaryRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$logPath = Join-Path $temporaryRoot ('wav-player-ci-' + [Guid]::NewGuid().ToString('N') + '.log')
$log = [IO.StreamWriter]::new($logPath, $false, [Text.UTF8Encoding]::new($false))
try {
    $log.AutoFlush = $true
    foreach ($step in $steps) { Invoke-Step $step $log }
} finally {
    $log.Dispose()
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath }
}
