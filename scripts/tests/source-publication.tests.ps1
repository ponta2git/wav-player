# SPDX-License-Identifier: GPL-3.0-only
# Exercises the public source checks, the export and the CI orchestrator on small
# fixture trees. No SDK, compiler, player or hardware is used. Each group below
# builds the trees it needs in the test directory.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workParent = Join-Path $root 'out'
$workPrefix = 'source-tests-'
$work = New-StagingDirectory $workParent $workPrefix
$utf8 = Get-TestEncoding

# A source tree holds the real publication scripts, its manifest and a settings example.
$publicationScripts = @('scripts/check-public-source.ps1', 'scripts/export-source.ps1',
    'scripts/support/public-manifest.ps1', 'scripts/support/staging.ps1')
$treeFiles = @('build/public-files.txt') + $publicationScripts + 'settings.example.ini'

function Write-Manifest([string]$Tree, [string[]]$Entries) {
    [IO.File]::WriteAllLines((Join-Path $Tree 'build/public-files.txt'), $Entries, $utf8)
}

function Write-Settings([string]$Tree, [string]$SelectedDevice) {
    Write-TestFile (Join-Path $Tree 'settings.example.ini') "[General]`nSelectedDevice=$SelectedDevice`n; choose locally`n"
}

function New-SourceTree([string]$Name) {
    $tree = Join-Path $work $Name
    foreach ($directory in 'build', 'scripts/support') { [void][IO.Directory]::CreateDirectory((Join-Path $tree $directory)) }
    foreach ($relative in $publicationScripts) {
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination (Join-Path $tree $relative)
    }
    Write-Manifest $tree $treeFiles
    Write-Settings $tree ''
    $tree
}

function Invoke-Check([string]$Tree, [hashtable]$Options = @{}) {
    & (Join-Path $Tree 'scripts/check-public-source.ps1') @Options | Out-Null
}

function Invoke-Export([string]$Tree, [string]$OutputDirectory) {
    & (Join-Path $Tree 'scripts/export-source.ps1') -OutputDirectory $OutputDirectory | Out-Null
}

function Test-CleanSource {
    $tree = New-SourceTree 'clean source'
    Invoke-Check $tree @{ StrictTree = $true }
    Complete-TestGroup 'clean source with an unselected device'
}

function Test-RejectedManifestEntries {
    $tree = New-SourceTree 'rejected entries'
    foreach ($path in 'private/results/probe.txt', 'external/asiosdk/header.h', 'out/player.exe',
        '.claude/session.json', '.codex/session.json', '.agents/session.json', 'docs/private/notes.md',
        '../escape.md', 'docs/../escape.md', '/rooted.md', 'docs\backslash.md', ' padded.md', 'binary.dll') {
        Write-Manifest $tree ($treeFiles + $path)
        Assert-Fails { Invoke-Check $tree } 'Not a public source path'
    }
    Write-Manifest $tree ($treeFiles + $treeFiles[0])
    Assert-Fails { Invoke-Check $tree } 'Duplicate public source path'
    Write-Manifest $tree ($treeFiles + 'docs/absent.md')
    Assert-Fails { Invoke-Check $tree } 'Missing public source file'
    Write-Manifest $tree @('# no entries', '')
    Assert-Fails { Invoke-Check $tree } 'manifest is empty'
    Complete-TestGroup 'local state, SDK, binaries, traversal, duplicate, missing and absent entries rejected'
}

function Test-ConfiguredDevice {
    $tree = New-SourceTree 'configured device'
    Write-Settings $tree '{A116D20A-4907-4F39-8231-A6701B01DBE1}'
    Assert-Fails { Invoke-Check $tree } 'Public settings must start without a selected device'
    Complete-TestGroup 'configured device rejected'
}

# The manifest must equal the files Git tracks, and name them in the same letter case.
function Test-TrackedFiles {
    $tree = New-SourceTree 'tracked source'
    & git -C $tree init --quiet --template= --initial-branch=main
    Assert-True ($LASTEXITCODE -eq 0) 'Cannot initialize the tracked fixture.'
    & git -C $tree -c core.autocrlf=false add -- $treeFiles
    Assert-True ($LASTEXITCODE -eq 0) 'Cannot track the fixture files.'
    Invoke-Check $tree @{ Tracked = $true }
    Write-Manifest $tree @($treeFiles | ForEach-Object { $_.Replace('settings.example.ini', 'Settings.Example.ini') })
    Assert-Fails { Invoke-Check $tree @{ Tracked = $true } } 'Tracked files differ'
    Write-Manifest $tree $treeFiles
    Write-TestFile (Join-Path $tree 'unlisted.md') 'tracked but not listed'
    & git -C $tree -c core.autocrlf=false add -- unlisted.md
    Assert-True ($LASTEXITCODE -eq 0) 'Cannot track the unlisted fixture file.'
    Assert-Fails { Invoke-Check $tree @{ Tracked = $true } } 'Tracked files differ'
    Complete-TestGroup 'tracked files must equal the manifest, letter case included'
}

function Test-Export {
    $tree = New-SourceTree 'export source'
    $localDirectory = Join-Path $tree 'private/notes'
    [void][IO.Directory]::CreateDirectory($localDirectory)
    Write-TestFile (Join-Path $localDirectory 'local.txt') 'local-only fixture'
    Assert-Fails { Invoke-Check $tree @{ StrictTree = $true } } 'Exported files differ'
    $exportParent = Join-Path $work 'exports'
    $output = Join-Path $exportParent 'source export'
    Invoke-Export $tree $output
    Invoke-Check $tree @{ SourceDirectory = $output; StrictTree = $true }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $output 'private'))) 'Export copied local records.'
    Assert-True (@(Get-ChildItem -LiteralPath $exportParent -Force).Count -eq 1) 'Export left temporary data beside its destination.'
    Assert-Fails { Invoke-Export $tree $output } 'Choose a new'
    Complete-TestGroup 'export excludes unlisted local records and refuses existing destinations'
}

function Test-ExportedTreeState {
    $tree = New-SourceTree 'state source'
    $output = Join-Path $work 'state export'
    Invoke-Export $tree $output
    foreach ($name in '.git', '.claude', '.codex', '.agents') {
        $stateDirectory = Join-Path $output $name
        [void][IO.Directory]::CreateDirectory($stateDirectory)
        Assert-Fails { Invoke-Check $tree @{ SourceDirectory = $output; StrictTree = $true } } 'local state or generated directories'
        Remove-Item -LiteralPath $stateDirectory
    }
    Invoke-Check $tree @{ SourceDirectory = $output; StrictTree = $true }
    Complete-TestGroup 'exported tree rejects even empty Git and agent state'
}

# A fresh repository must ignore agent state using the checked-in rules,
# independently of the developer's global excludes or old .git/info/exclude.
function Test-IgnoreRules {
    $ignoreRepository = Join-Path $work 'ignore fixture'
    [void][IO.Directory]::CreateDirectory($ignoreRepository)
    Copy-Item -LiteralPath (Join-Path $root '.gitignore') -Destination $ignoreRepository
    $emptyExcludes = Join-Path $work 'empty excludes'
    Write-TestFile $emptyExcludes ''
    & git -C $ignoreRepository init --quiet --template= --initial-branch=main
    Assert-True ($LASTEXITCODE -eq 0) 'Cannot initialize the isolated ignore fixture.'
    foreach ($name in '.claude', '.codex', '.agents') {
        $stateDirectory = Join-Path $ignoreRepository $name
        [void][IO.Directory]::CreateDirectory($stateDirectory)
        Write-TestFile (Join-Path $stateDirectory 'session.json') '{}'
        $relative = $name + '/session.json'
        $ignored = @(& git -C $ignoreRepository -c ('core.excludesFile=' + $emptyExcludes) check-ignore --no-index -- $relative)
        Assert-True ($LASTEXITCODE -eq 0 -and $ignored.Count -eq 1 -and $ignored[0] -eq $relative) ('Fresh repository does not ignore ' + $name)
    }
    Complete-TestGroup 'checked-in ignore rules exclude agent state in a fresh repository'
}

# The stand-ins record each step the real CI orchestrator actually starts.
# verify.ps1 also prints public and private diagnostics and can fail with 17.
$ciStepsBeforeSetup = @('check-public-source.ps1', 'source-publication.tests.ps1', 'project-layout.tests.ps1',
    'fetch-asio-sdk.ps1', 'verify.ps1')

function New-CiFixture([string]$Name) {
    $ci = Join-Path $work $Name
    foreach ($relative in 'scripts/tests', 'logs') { [void][IO.Directory]::CreateDirectory((Join-Path $ci $relative)) }
    Copy-Item -LiteralPath (Join-Path $root 'scripts/ci.ps1') -Destination (Join-Path $ci 'scripts/ci.ps1')
    foreach ($name in 'check-public-source.ps1', 'fetch-asio-sdk.ps1', 'tests/source-publication.tests.ps1',
        'tests/project-layout.tests.ps1', 'tests/player-setup.tests.ps1') {
        Write-TestFile (Join-Path $ci ('scripts/' + $name)) @'
param([switch]$Tracked)
$name = $MyInvocation.MyCommand.Name
if ($name -eq 'check-public-source.ps1' -and -not $Tracked) { throw 'Missing -Tracked.' }
[IO.File]::AppendAllText($env:WAVPLAY_CI_TEST_STEPS, $name + [Environment]::NewLine)
'@
    }
    Write-TestFile (Join-Path $ci 'scripts/verify.ps1') @'
param([string]$Configuration = 'All', [switch]$Analyze, [switch]$IncludeASan, [switch]$CiWorker, [switch]$TestReleaseStartup)
[IO.File]::AppendAllText($env:WAVPLAY_CI_TEST_STEPS, $MyInvocation.MyCommand.Name + [Environment]::NewLine)
@{
    Configuration=$Configuration; Analyze=[bool]$Analyze; IncludeASan=[bool]$IncludeASan
    CiWorker=[bool]$CiWorker; TestReleaseStartup=[bool]$TestReleaseStartup
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'arguments.json')
Write-Output '== Build Release|x64 =='
Write-Output '== player_window =='
Write-Output 'TIMING Settings 0 ms'
Write-Output 'TIMING Build-Debug 123 ms'
Write-Output 'TIMING Audit-Release 456 ms'
Write-Output 'TIMING Test-Debug-ASan 789 ms'
Write-Output 'TIMING Release-Startup 10 ms'
Write-Output 'TIMING PRIVATE_CI_STAGE 12 ms'
Write-Output 'TIMING Audit-Unknown 12 ms'
Write-Output 'TIMING Audit-Release 12 ms PRIVATE_CI_TOKEN'
Write-Output 'TIMING Build-Debug -1 ms'
Write-Output 'TIMING Test-Debug-ASan 1.5 ms'
Write-Output 'TIMING Settings 9223372036854775808 ms'
Write-Output '[FAIL] fixture numeric assertion: example field: expected 1, got 0 (fixture_tests.cpp:42)'
Write-Output '[FAIL] fixture exception: C:\Users\PRIVATE_CI_FIXTURE\secret.log token=PRIVATE_CI_TOKEN'
Write-Output '[FAIL] fixture GUI timeout: class std::runtime_error: GUI state transition: deadline expired; C:\Users\PRIVATE_CI_GUI\input.wav token=PRIVATE_CI_TOKEN (player_window_tests.cpp:1150)'
Write-Output '[FAIL] fixture GUI fullpath suffix: class std::runtime_error: GUI stopped responding (C:\Users\PRIVATE_CI_GUI\player_window_tests.cpp:1151)'
Write-Output '[FAIL] fixture typed exception: class std::out_of_range: C:\Users\PRIVATE_CI_RANGE\input.wav token=PRIVATE_CI_TOKEN'
Write-Output '[FAIL] fixture GUI send: class std::runtime_error: gui_send_failed message=13 win32=1460 (player_window_tests.cpp:431)'
Write-Output '[FAIL] fixture GUI send upper bound: class std::runtime_error: gui_send_failed message=4294967295 win32=4294967295 (player_window_tests.cpp:431)'
Write-Output '[FAIL] fixture unknown exception: class PRIVATE_CI_TYPE: gui_send_failed message=13 win32=1460'
Write-Output '[FAIL] fixture extended exception name: class std::runtime_error_PRIVATE_CI_TYPE: gui_send_failed message=13 win32=1460'
Write-Output '[FAIL] fixture GUI false marker: class std::runtime_error: gui_send_failed message=13 win32=1460 PRIVATE_CI_TOKEN (player_window_tests.cpp:431)'
Write-Output '[FAIL] fixture GUI private message: class std::runtime_error: gui_send_failed message=PRIVATE_CI_MESSAGE win32=1460'
Write-Output '[FAIL] fixture GUI overflowing message: class std::runtime_error: gui_send_failed message=4294967296 win32=1460 (player_window_tests.cpp:431)'
Write-Output '[FAIL] fixture GUI overflowing error: class std::runtime_error: gui_send_failed message=13 win32=4294967296 (player_window_tests.cpp:431)'
Write-Output '[FAIL] fixture GUI negative message: class std::runtime_error: gui_send_failed message=-1 win32=1460'
Write-Output '[FAIL] fixture\PRIVATE_CI_CASE: do not expose a path as a case name'
Write-Output 'FAIL    player_window         1.2 s PRIVATE_CI_DETAIL'
Write-Output '1 suites in 1.2 s: 0 passed, 1 failed, 0 timed out, 0 crashed.'
Write-Output 'unstructured diagnostic PRIVATE_CI_RAW'
[Console]::Error.WriteLine('stderr diagnostic PRIVATE_CI_STDERR')
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fail.flag')) { exit 17 }
'@
    $ci
}

function Invoke-CiFixture([string]$Directory, [switch]$ReleaseOnly, [string]$Worker) {
    $arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + (Join-Path $Directory 'scripts/ci.ps1') + '"'
    if ($ReleaseOnly) { $arguments += ' -ReleaseOnly' }
    if ($Worker) { $arguments += ' -Worker ' + $Worker }
    $environment = @{ RUNNER_TEMP = Join-Path $Directory 'logs'; WAVPLAY_CI_TEST_STEPS = Join-Path $Directory 'steps.txt' }
    Invoke-TestProcess -FileName (Get-WindowsPowerShell) -Arguments $arguments -Environment $environment -TimeoutSeconds 90
}

function Get-ForwardedArguments([string]$Directory) {
    Get-Content -LiteralPath (Join-Path $Directory 'scripts/arguments.json') -Raw | ConvertFrom-Json
}

function Assert-CiSteps([string]$Directory, [string[]]$Expected) {
    $actual = [IO.File]::ReadAllLines((Join-Path $Directory 'steps.txt'))
    Assert-True (($actual -join ', ') -ceq ($Expected -join ', ')) ('CI ran unexpected steps: ' + ($actual -join ', '))
}

function Assert-NoPrivateDiagnostic($Result, [string]$Directory, [string]$Description) {
    Assert-True (-not ($Result.Output + $Result.Error).Contains('PRIVATE_CI_')) ($Description + ' exposed a raw exception, path, case or status detail.')
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $Directory 'logs') -Force).Count -eq 0) ($Description + ' left detailed logs.')
}

function Test-CiFullVerification {
    $ci = New-CiFixture 'CI full'
    $result = Invoke-CiFixture $ci
    Assert-True ($result.Code -eq 0) 'Default CI fixture failed.'
    $arguments = Get-ForwardedArguments $ci
    Assert-True ($arguments.Configuration -eq 'All' -and $arguments.Analyze -and $arguments.IncludeASan) 'Default CI omitted full verification arguments.'
    Assert-True (-not $arguments.CiWorker -and $arguments.TestReleaseStartup) 'Default CI omitted the complete source audit or startup test.'
    Assert-True ($result.Output.Contains('Verification mode: full')) 'Full mode was not labelled.'
    $publicLines = @($result.Output -split '\r?\n')
    foreach ($expected in @(
        '== Build Release|x64 ==',
        '== player_window ==',
        'TIMING Settings 0 ms',
        'TIMING Build-Debug 123 ms',
        'TIMING Audit-Release 456 ms',
        'TIMING Test-Debug-ASan 789 ms',
        'TIMING Release-Startup 10 ms',
        '[FAIL] fixture numeric assertion: expected 1, got 0 (fixture_tests.cpp:42)',
        '[FAIL] fixture exception',
        '[FAIL] fixture GUI timeout: exception=std::runtime_error (player_window_tests.cpp:1150)',
        '[FAIL] fixture GUI fullpath suffix: exception=std::runtime_error',
        '[FAIL] fixture typed exception: exception=std::out_of_range',
        '[FAIL] fixture GUI send: exception=std::runtime_error; gui_send_failed message=13 win32=1460 (player_window_tests.cpp:431)',
        '[FAIL] fixture GUI send upper bound: exception=std::runtime_error; gui_send_failed message=4294967295 win32=4294967295 (player_window_tests.cpp:431)',
        '[FAIL] fixture unknown exception',
        '[FAIL] fixture extended exception name',
        '[FAIL] fixture GUI false marker: exception=std::runtime_error (player_window_tests.cpp:431)',
        '[FAIL] fixture GUI private message: exception=std::runtime_error',
        '[FAIL] fixture GUI overflowing message: exception=std::runtime_error (player_window_tests.cpp:431)',
        '[FAIL] fixture GUI overflowing error: exception=std::runtime_error (player_window_tests.cpp:431)',
        '[FAIL] fixture GUI negative message: exception=std::runtime_error',
        'FAIL player_window 1.2 s',
        '1 suites in 1.2 s: 0 passed, 1 failed, 0 timed out, 0 crashed.'
    )) {
        Assert-True ($publicLines -ccontains $expected) ('CI diagnostic differs from its bounded schema: ' + $expected)
    }
    Assert-True (@($publicLines -clike 'TIMING *').Count -eq 5) 'A timing outside the stage and integer duration schema reached the public log.'
    Assert-NoPrivateDiagnostic $result $ci 'Successful CI'
    Assert-CiSteps $ci ($ciStepsBeforeSetup + 'player-setup.tests.ps1')
    Complete-TestGroup 'default CI forwards full verification and publishes only reconstructed diagnostics'
}

function Test-CiReleaseOnly {
    $ci = New-CiFixture 'CI release only'
    Write-TestFile (Join-Path $ci 'scripts/fail.flag') 'fail'
    $result = Invoke-CiFixture $ci -ReleaseOnly
    $arguments = Get-ForwardedArguments $ci
    Assert-True ($arguments.Configuration -eq 'Release' -and -not $arguments.Analyze -and -not $arguments.IncludeASan) 'Release-only mode forwarded the wrong verification arguments.'
    Assert-True (-not $arguments.CiWorker -and $arguments.TestReleaseStartup) 'Release-only diagnostics omitted the complete source audit or startup test.'
    Assert-True ($result.Output.Contains('Verification mode: Release-only diagnostics')) 'Diagnostic mode was not labelled.'
    Assert-True ($result.Code -ne 0 -and $result.Error.Contains('failed (exit 17)')) 'CI swallowed the child failure exit code.'
    Assert-NoPrivateDiagnostic $result $ci 'Failed CI'
    Assert-CiSteps $ci $ciStepsBeforeSetup
    Complete-TestGroup 'Release-only CI forwards its mode, fails on a nonzero child and removes detailed logs'
}

function Test-CiWorkers {
    foreach ($worker in 'Release', 'Debug') {
        $ci = New-CiFixture ('CI worker ' + $worker)
        $result = Invoke-CiFixture $ci -Worker $worker
        Assert-True ($result.Code -eq 0) "$worker CI worker failed."
        $arguments = Get-ForwardedArguments $ci
        Assert-True ($arguments.Configuration -ceq $worker -and $arguments.CiWorker -and $arguments.Analyze) "$worker CI worker selected the wrong configuration or analysis."
        if ($worker -eq 'Release') {
            Assert-True ($arguments.TestReleaseStartup -and -not $arguments.IncludeASan) 'Release worker forwarded the wrong startup or ASan options.'
            Assert-CiSteps $ci ($ciStepsBeforeSetup + 'player-setup.tests.ps1')
        } else {
            Assert-True ($arguments.IncludeASan -and -not $arguments.TestReleaseStartup) 'Debug worker forwarded the wrong startup or ASan options.'
            Assert-CiSteps $ci @('fetch-asio-sdk.ps1', 'verify.ps1')
        }
        Assert-NoPrivateDiagnostic $result $ci "$worker CI worker"
    }
    Complete-TestGroup 'CI workers each check the SDK and their own build profiles; public source and setup run once'
}

try {
    Test-CleanSource
    Test-RejectedManifestEntries
    Test-ConfiguredDevice
    Invoke-WithoutGitEnvironment { Test-TrackedFiles }
    Test-Export
    Test-ExportedTreeState
    Invoke-WithoutGitEnvironment { Test-IgnoreRules }
    Test-CiFullVerification
    Test-CiReleaseOnly
    Test-CiWorkers
    Write-Output ('[PASS] Source publication tests: ' + (Get-TestGroupCount) + ' groups')
} finally {
    Remove-StagingDirectory $work $workParent $workPrefix
}
