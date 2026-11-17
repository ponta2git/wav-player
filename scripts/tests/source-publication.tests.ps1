# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $root ('out/source-tests-' + [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $work 'input'
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($relative in 'input/build', 'input/scripts') { [void][IO.Directory]::CreateDirectory((Join-Path $work $relative)) }
foreach ($name in 'check-public-source.ps1', 'export-source.ps1') {
    Copy-Item -LiteralPath (Join-Path $root "scripts/$name") -Destination (Join-Path $fixture "scripts/$name")
}
$manifest = Join-Path $fixture 'build/public-files.txt'
$files = @('build/public-files.txt', 'scripts/check-public-source.ps1', 'scripts/export-source.ps1', 'settings.example.ini')
$check = Join-Path $fixture 'scripts/check-public-source.ps1'
$export = Join-Path $fixture 'scripts/export-source.ps1'
function Write-Manifest([string[]]$Entries) { [IO.File]::WriteAllLines($manifest, $Entries, $utf8) }
function Assert-Fails([scriptblock]$Action, [string]$Expected) {
    try { & $Action | Out-Null } catch {
        if (-not $_.Exception.Message.Contains($Expected)) { throw }
        return
    }
    throw "Expected rejection: $Expected"
}
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Invoke-CiFixture([string]$Directory, [switch]$ReleaseOnly) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $start.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
        (Join-Path $Directory 'scripts/ci.ps1') + '"'
    if ($ReleaseOnly) { $start.Arguments += ' -ReleaseOnly' }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['RUNNER_TEMP'] = Join-Path $Directory 'logs'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorText = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(90000)) {
            $process.Kill()
            throw 'CI fixture did not finish within its deadline.'
        }
        return [PSCustomObject]@{ Code=$process.ExitCode; Output=$output.Result; Error=$errorText.Result }
    } finally { $process.Dispose() }
}
try {
    Write-Manifest $files
    [IO.File]::WriteAllText((Join-Path $fixture 'settings.example.ini'), "[General]`nSelectedDevice=`n; choose locally`n", $utf8)
    & $check -StrictTree | Out-Null
    Write-Output 'PASS clean source with an unselected device'
    foreach ($path in 'private/results/probe.txt', 'external/asiosdk/header.h', 'out/player.exe',
        '.claude/session.json', '.codex/session.json', '.agents/session.json', '../escape.md', 'binary.dll') {
        Write-Manifest ($files + $path)
        Assert-Fails { & $check } 'Not a public source path'
    }
    Write-Manifest ($files + $files[0])
    Assert-Fails { & $check } 'Duplicate public source path'
    Write-Manifest $files
    Write-Output 'PASS local state, SDK, binaries, traversal and duplicate entries rejected'
    [IO.File]::WriteAllText((Join-Path $fixture 'settings.example.ini'), "[General]`nSelectedDevice={A116D20A-4907-4F39-8231-A6701B01DBE1}`n", $utf8)
    Assert-Fails { & $check } 'Public settings must start without a selected device'
    [IO.File]::WriteAllText((Join-Path $fixture 'settings.example.ini'), "[General]`nSelectedDevice=`n", $utf8)
    Write-Output 'PASS configured device rejected'
    $localDirectory = Join-Path $fixture 'private/notes'
    [void][IO.Directory]::CreateDirectory($localDirectory)
    [IO.File]::WriteAllText((Join-Path $localDirectory 'local.txt'), 'local-only fixture', $utf8)
    Assert-Fails { & $check -StrictTree } 'Exported files differ'
    $output = Join-Path $work 'source export'
    & $export -OutputDirectory $output | Out-Null
    & $check -SourceDirectory $output -StrictTree | Out-Null
    if (Test-Path -LiteralPath (Join-Path $output 'private')) { throw 'Export copied local records.' }
    Assert-Fails { & $export -OutputDirectory $output } 'Choose a new'
    Write-Output 'PASS export excludes unlisted local records and refuses existing destinations'
    foreach ($name in '.git', '.claude', '.codex', '.agents') {
        $stateDirectory = Join-Path $output $name
        [void][IO.Directory]::CreateDirectory($stateDirectory)
        Assert-Fails { & $check -SourceDirectory $output -StrictTree } 'local state or generated directories'
        Remove-Item -LiteralPath $stateDirectory
    }
    Write-Output 'PASS exported tree rejects even empty Git and agent state'

    # A fresh repository must ignore agent state using the checked-in rules,
    # independently of the developer's global excludes or old .git/info/exclude.
    $ignoreRepository = Join-Path $work 'ignore fixture'
    [void][IO.Directory]::CreateDirectory($ignoreRepository)
    Copy-Item -LiteralPath (Join-Path $root '.gitignore') -Destination $ignoreRepository
    $emptyExcludes = Join-Path $work 'empty excludes'
    [IO.File]::WriteAllText($emptyExcludes, '', $utf8)
    & git -C $ignoreRepository init --quiet --template= --initial-branch=main
    Assert-True ($LASTEXITCODE -eq 0) 'Cannot initialize the isolated ignore fixture.'
    foreach ($name in '.claude', '.codex', '.agents') {
        $stateDirectory = Join-Path $ignoreRepository $name
        [void][IO.Directory]::CreateDirectory($stateDirectory)
        [IO.File]::WriteAllText((Join-Path $stateDirectory 'session.json'), '{}', $utf8)
        $relative = $name + '/session.json'
        $ignored = @(& git -C $ignoreRepository -c ('core.excludesFile=' + $emptyExcludes) check-ignore --no-index -- $relative)
        Assert-True ($LASTEXITCODE -eq 0 -and $ignored.Count -eq 1 -and $ignored[0] -eq $relative) ('Fresh repository does not ignore ' + $name)
    }
    Write-Output 'PASS checked-in ignore rules exclude agent state in a fresh repository'

    # Run the real CI orchestrator against small child scripts. No SDK, compiler,
    # player, hardware or second copy of this test is executed in the fixture.
    $ci = Join-Path $work 'CI fixture'
    foreach ($relative in 'scripts/tests', 'logs') { [void][IO.Directory]::CreateDirectory((Join-Path $ci $relative)) }
    Copy-Item -LiteralPath (Join-Path $root 'scripts/ci.ps1') -Destination (Join-Path $ci 'scripts/ci.ps1')
    foreach ($name in 'check-public-source.ps1', 'fetch-asio-sdk.ps1', 'tests/source-publication.tests.ps1') {
        [IO.File]::WriteAllText((Join-Path $ci ('scripts/' + $name)), 'param([switch]$Tracked)', $utf8)
    }
    [IO.File]::WriteAllText((Join-Path $ci 'scripts/tests/player-setup.tests.ps1'),
        '[IO.File]::WriteAllText((Join-Path $PSScriptRoot ''setup-ran.txt''), ''ran'')', $utf8)
    [IO.File]::WriteAllText((Join-Path $ci 'scripts/verify.ps1'), @'
param([string]$Configuration = 'All', [switch]$Analyze, [switch]$IncludeASan)
@{ Configuration=$Configuration; Analyze=[bool]$Analyze; IncludeASan=[bool]$IncludeASan } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'arguments.json')
Write-Output '== Build Release|x64 =='
Write-Output '== player_window =='
Write-Output '[FAIL] fixture numeric assertion: example field: expected 1, got 0 (fixture_tests.cpp:42)'
Write-Output '[FAIL] fixture exception: C:\Users\PRIVATE_CI_FIXTURE\secret.log token=PRIVATE_CI_TOKEN'
Write-Output '[FAIL] fixture GUI timeout: class std::runtime_error: GUI state transition: deadline expired; C:\Users\PRIVATE_CI_GUI\input.wav token=PRIVATE_CI_TOKEN (player_window_tests.cpp:1150)'
Write-Output '[FAIL] fixture GUI fullpath suffix: class std::runtime_error: GUI stopped responding (C:\Users\PRIVATE_CI_GUI\player_window_tests.cpp:1151)'
Write-Output '[FAIL] fixture\PRIVATE_CI_CASE: do not expose a path as a case name'
Write-Output 'FAIL    player_window         1.2 s PRIVATE_CI_DETAIL'
Write-Output '1 suites in 1.2 s: 0 passed, 1 failed, 0 timed out, 0 crashed.'
Write-Output 'unstructured diagnostic PRIVATE_CI_RAW'
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fail.flag')) { exit 17 }
'@, $utf8)
    $result = Invoke-CiFixture $ci
    Assert-True ($result.Code -eq 0) 'Default CI fixture failed.'
    $arguments = Get-Content -LiteralPath (Join-Path $ci 'scripts/arguments.json') -Raw | ConvertFrom-Json
    Assert-True ($arguments.Configuration -eq 'All' -and $arguments.Analyze -and $arguments.IncludeASan) 'Default CI omitted full verification arguments.'
    Assert-True ($result.Output.Contains('Verification mode: full')) 'Full mode was not labelled.'
    foreach ($expected in @('== player_window ==', '[FAIL] fixture numeric assertion: expected 1, got 0 (fixture_tests.cpp:42)',
        '[FAIL] fixture exception', 'FAIL player_window 1.2 s')) {
        Assert-True ($result.Output.Contains($expected)) ('Missing reconstructed diagnostic: ' + $expected)
    }
    $publicLines = @($result.Output -split '\r?\n')
    Assert-True ($publicLines -contains '[FAIL] fixture GUI timeout (player_window_tests.cpp:1150)') 'CI omitted the GUI exception location or retained its raw detail.'
    Assert-True ($publicLines -contains '[FAIL] fixture GUI fullpath suffix') 'CI accepted a private path as a public source location.'
    Assert-True (-not ($result.Output + $result.Error).Contains('PRIVATE_CI_')) 'CI exposed a raw exception, path, case or status detail.'
    Assert-True (Test-Path -LiteralPath (Join-Path $ci 'scripts/tests/setup-ran.txt')) 'Successful CI did not continue to setup tests.'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $ci 'logs') -Force).Count -eq 0) 'Successful CI left detailed logs.'
    Write-Output 'PASS default CI forwards full verification and publishes only reconstructed diagnostics'

    Remove-Item -LiteralPath (Join-Path $ci 'scripts/tests/setup-ran.txt')
    [IO.File]::WriteAllText((Join-Path $ci 'scripts/fail.flag'), 'fail', $utf8)
    $result = Invoke-CiFixture $ci -ReleaseOnly
    $arguments = Get-Content -LiteralPath (Join-Path $ci 'scripts/arguments.json') -Raw | ConvertFrom-Json
    Assert-True ($arguments.Configuration -eq 'Release' -and -not $arguments.Analyze -and -not $arguments.IncludeASan) 'Release-only mode forwarded the wrong verification arguments.'
    Assert-True ($result.Output.Contains('Verification mode: Release-only diagnostics')) 'Diagnostic mode was not labelled.'
    Assert-True ($result.Code -ne 0 -and $result.Error.Contains('failed (exit 17)')) 'CI swallowed the child failure exit code.'
    Assert-True (-not ($result.Output + $result.Error).Contains('PRIVATE_CI_')) 'Failed CI exposed raw child diagnostics.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $ci 'scripts/tests/setup-ran.txt'))) 'CI continued after the verification failure.'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $ci 'logs') -Force).Count -eq 0) 'Failed CI left detailed logs.'
    Write-Output 'PASS Release-only CI forwards its mode, fails on a nonzero child and removes detailed logs'
} finally {
    $resolved = [IO.Path]::GetFullPath($work)
    $allowed = [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolved).StartsWith('source-tests-'))) { throw 'Unexpected test cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
Write-Output '[PASS] Source publication tests: 8 groups'
