# SPDX-License-Identifier: GPL-3.0-only
# Exercise event selection and the real checkout/restore in a disposable repository.
# Only MSBuild and the standalone comparison tool use fixtures.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'ci-release-layout.ps1')

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workParent = Join-Path $root 'out'
$workPrefix = 'ci-layout-test-'
$work = New-StagingDirectory $workParent $workPrefix
$repo = Join-Path $work 'fixture repo'
$runnerTemp = Join-Path $work 'runner temp'
$summaryPath = Join-Path $work 'summary.md'
$eventPath = Join-Path $work 'event.json'
$stepOutput = Join-Path $work 'step-output'
$releaseDir = Join-Path $repo 'out/x64/Release'
$realCommand = (Get-Command Get-LayoutCommandOutput).ScriptBlock

function Invoke-FixtureGit([string[]]$Arguments) {
    & $realCommand git (@('-C', $repo, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
        '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=NUL') + $Arguments)
}

function New-LayoutRepo {
    [void][IO.Directory]::CreateDirectory((Join-Path $repo 'scripts'))
    [void][IO.Directory]::CreateDirectory($releaseDir)
    $null = Invoke-FixtureGit @('init', '--initial-branch=fixture')
    Write-TestFile (Join-Path $repo '.gitignore') "out/`n"
    Write-TestFile (Join-Path $repo 'version') 'baseline'
    $sdkPin = Join-Path $repo 'scripts/fetch-asio-sdk.ps1'
    Write-TestFile $sdkPin ('$expectedHash = ''{0}''' -f ('a' * 64))
    $null = Invoke-FixtureGit @('add', '.')
    $null = Invoke-FixtureGit @('commit', '-m', 'Baseline')
    $script:baseSha = Invoke-FixtureGit @('rev-parse', 'HEAD')
    Write-TestFile (Join-Path $repo 'version') 'candidate'
    Write-TestFile (Join-Path $repo 'scripts/compare-release-layout.ps1') @'
param($BaselineDir, $CandidateDir, $ReportPath)
$ErrorActionPreference = 'Stop'
$version = [IO.File]::ReadAllText((Join-Path $BaselineDir 'wavplay.exe'))
if ($version -ceq 'bad') { throw 'PRIVATE_FIXTURE_ANALYSIS' }
if ($version -cne 'baseline') { throw 'Comparison did not get the baseline build.' }
if ([IO.File]::ReadAllText((Join-Path $CandidateDir 'wavplay.exe')) -cne 'candidate') { throw 'Wrong candidate.' }
$layout = @{
    Anchors = @(@{Label='switch_buffer';Delta=-16},@{Label='callback_state';Delta=$null},@{Label='fill_slots';Delta=24})
    Groups = @(@{Label='Kernels';Matched=7;Moved=4;Unmatched=2;MinDelta=-8;MaxDelta=32},
        @{Label='Constants';Matched=0;Moved=0;Unmatched=1;MinDelta=$null;MaxDelta=$null})
    Instructions = @{Status='Partial';BaselineCrossings=3;CandidateCrossings=4}
    Footprints = @(foreach ($path in 'Callback','Refill','Control','Display') {
        $pages = if ($path -ceq 'Control') { 7 } else { 5 }
        @{Path=$path
            Baseline=@{Pages=5;Lines=40;Instructions=600;OpenFunctions=0;PageRvas=@(4096);Name='PRIVATE_FIXTURE_FOOTPRINT'}
            Candidate=@{Pages=$pages;Lines=41;Instructions=612;OpenFunctions=1;PageRvas=@(8192)}}
    })
    Sections = @(@{Name='.text';DifferentBytes=12;BaselineOnlyBytes=0;CandidateOnlyBytes=8},
        @{Name='.PRIVATE_FIXTURE_SECTION';DifferentBytes=123;BaselineOnlyBytes=0;CandidateOnlyBytes=0})
}
Write-Output 'PRIVATE_FIXTURE_stdout'
[Console]::Error.WriteLine('PRIVATE_FIXTURE_stderr')
[IO.File]::WriteAllText($ReportPath, (@{Summary=$layout} | ConvertTo-Json -Depth 6))
'@
    $null = Invoke-FixtureGit @('add', '.')
    $null = Invoke-FixtureGit @('commit', '-m', 'Candidate')
    $script:candidateSha = Invoke-FixtureGit @('rev-parse', 'HEAD')
    $null = Invoke-FixtureGit @('switch', '--detach', $baseSha)
    Write-TestFile $sdkPin ('$expectedHash = ''{0}''' -f ('b' * 64))
    $null = Invoke-FixtureGit @('commit', '-am', 'Different SDK')
    $script:otherSdkSha = Invoke-FixtureGit @('rev-parse', 'HEAD')
    $null = Invoke-FixtureGit @('switch', 'fixture')
    foreach ($file in 'wavplay.exe', 'wavplay.map', 'wavplay.pdb') { Write-TestFile (Join-Path $releaseDir $file) 'candidate' }
}

function Test-LayoutEvents {
    $pr = [pscustomobject]@{pull_request=@{base=@{sha=$baseSha};head=@{sha=('a' * 40)}}}
    Assert-True ((Get-LayoutBaseline 'pull_request' $pr $repo $candidateSha) -ceq $baseSha) 'PR must use its base SHA.'
    Assert-True ((Get-LayoutBaseline 'push' @{before=$otherSdkSha} $repo $candidateSha) -ceq $otherSdkSha) 'Push must use before even when it differs from the first parent.'
    Assert-True ($null -eq (Get-LayoutBaseline 'push' @{before=('0' * 40)} $repo $candidateSha)) 'New branch has no baseline.'
    $manual = '{"inputs":{}}' | ConvertFrom-Json
    Assert-True ((Get-LayoutBaseline 'workflow_dispatch' $manual $repo $candidateSha) -ceq $baseSha) 'Manual run must default to first parent.'
    Assert-True ($null -eq (Get-LayoutBaseline 'workflow_dispatch' $manual $repo $baseSha)) 'Root commit has no parent.'
    $manual.inputs | Add-Member -NotePropertyName baseline_sha -NotePropertyValue ('B' * 40)
    Assert-True ((Get-LayoutBaseline 'workflow_dispatch' $manual $repo $candidateSha) -ceq ('b' * 40)) 'Explicit SHA must win.'
    $manual.inputs.baseline_sha = 'main|PRIVATE_FIXTURE'
    Assert-Fails { Get-LayoutBaseline 'workflow_dispatch' $manual $repo $candidateSha } 'Expected a full commit SHA'
    Assert-Fails { Format-LayoutNumber '0|PRIVATE_FIXTURE' } 'Invalid summary integer'
    $unknownPin = @{ScriptHash='A'; ArchiveHash=$null}
    Assert-True (Test-LayoutAsioSource $unknownPin $unknownPin) 'An unrecognized pin syntax must work for identical downloaders.'
    Assert-True (-not (Test-LayoutAsioSource $unknownPin @{ScriptHash='B';ArchiveHash=$null})) 'Unknown pins cannot compare two different downloaders.'
    Assert-True (Test-LayoutAsioSource @{ScriptHash='A';ArchiveHash='H'} @{ScriptHash='B';ArchiveHash='H'}) 'An unchanged archive pin must survive a downloader edit.'
    Complete-TestGroup 'Release layout event selection and bounded field types'
}

function Test-LayoutRun([string]$Mode, [string]$Expected, [switch]$Fails) {
    function Find-VisualStudio { $work }
    function Get-LayoutCommandOutput([string]$Program, [string[]]$Arguments) {
        # Model a post-checkout hook that fails after the switch updated HEAD.
        if ($Program -ceq 'git' -and $Arguments -contains 'switch' -and
            (($Mode -ceq 'switch-failure' -and $Arguments[-1] -ceq $baseSha) -or
             ($Mode -ceq 'restore-failure' -and $Arguments[-1] -ceq $candidateSha))) {
            $null = & $realCommand $Program $Arguments
            throw 'PRIVATE_FIXTURE_SWITCH'
        }
        if ([IO.Path]::GetFileName($Program) -ine 'MSBuild.exe') { return & $realCommand $Program $Arguments }
        $version = [IO.File]::ReadAllText((Join-Path $repo 'version'))
        $baseOption = $Arguments -like '/p:BaseOutputPath=*'
        $baseOutput = if ($baseOption) { $baseOption.Substring('/p:BaseOutputPath='.Length) } else { Join-Path $repo 'out' }
        $built = Join-Path $baseOutput 'x64/Release'
        if ($Arguments -like '/getProperty:*') {
            $toolset = if ($version -ceq 'baseline' -and $Mode -ceq 'mismatch') { 'older' } else { 'current' }
            return @{Properties=@{PlatformToolset='v145';VCToolsVersion=$toolset;VCToolsInstallDir='compiler';
                WindowsTargetPlatformVersion='10.0';WindowsSdkDir='sdk';OutDir=($built + '\');IntDir=(Join-Path $baseOutput 'obj')}} | ConvertTo-Json
        }
        Assert-True ($version -ceq 'baseline' -and $Arguments -contains '/t:Rebuild' -and $baseOption) 'Baseline needs a fresh rebuild.'
        Assert-True ($Arguments[0] -eq (Join-Path $repo 'src/app/wavplay.vcxproj')) 'The product must build from the original absolute path.'
        if ($Mode -ceq 'build-failure') { throw 'PRIVATE_FIXTURE_BUILD' }
        [void][IO.Directory]::CreateDirectory($built)
        if ($Mode -ceq 'analysis-failure') { $version = 'bad' }
        Write-TestFile (Join-Path $built 'wavplay.exe') $version
    }
    if ($Mode -ceq 'detached') { $null = Invoke-FixtureGit @('switch', '--detach', $candidateSha) }
    $branch = Invoke-FixtureGit @('rev-parse', '--abbrev-ref', 'HEAD')
    $before = switch ($Mode) {
        'no-baseline' { '0' * 40 }
        'fetch-failure' { 'f' * 40 }
        'sdk-mismatch' { $otherSdkSha }
        default { $baseSha }
    }
    $verification = if ($Mode -ceq 'no-release') { 'failure' } else { 'success' }
    Write-TestFile $eventPath (@{before=$before} | ConvertTo-Json)
    Write-TestFile $summaryPath ''
    Write-TestFile $stepOutput ''
    $publicLog = [Collections.Generic.List[string]]::new()
    $run = {
        Invoke-ReleaseLayoutReport $repo $runnerTemp $summaryPath 'push' $eventPath $candidateSha $verification $stepOutput |
            ForEach-Object { $publicLog.Add([string]$_) }
    }
    if ($Fails) { Assert-Fails $run 'Release layout comparison failed' } else { & $run }
    $status = if ($Fails) { 'failed' } elseif ($Expected -ceq '比較済み') { 'compared' } else { 'skipped' }
    Assert-True ($publicLog.Count -eq 1 -and $publicLog[0] -ceq "Release layout summary: $status") 'Unexpected public log.'
    Assert-True ([IO.File]::ReadAllText($stepOutput) -ceq "summary_written=true`n") 'Missing or duplicate summary acknowledgement.'
    $text = [IO.File]::ReadAllText($summaryPath)
    Assert-True ($text.Contains($Expected)) "Expected summary state was missing for $Mode."
    Assert-True (-not $text.Contains('PRIVATE_FIXTURE') -and -not $text.Contains($work) -and $text.Contains($candidateSha)) 'Invalid public fields.'
    if ($Mode -ceq 'success') {
        foreach ($field in $baseSha, '| `switch_buffer` | -16 |', '| `callback_state` | — |', '| `fill_slots` | +24 |',
            '| 変換カーネル | 7 | 4 | 2 | -8 ～ +32 |', '| SIMD定数 | 0 | 0 | 1 | — ～ — |',
            '（一部のみ）　基準 3 件 ／ 候補 4 件', '| `.text` | 12 | 0 | 8 |',
            '| コールバック | 5 → 5 | 40 → 41 | 600 → 612 | 0 → 1 |', '| 制御の定期起床 | 5 → 7 | 40 → 41 | 600 → 612 | 0 → 1 |',
            '| 補充 | 5 → 5 |', '| 表示の更新 | 5 → 5 |') {
            Assert-True ($text.Contains($field)) 'Published table differs from the report.'
        }
    }
    Assert-True ((Invoke-FixtureGit @('rev-parse', 'HEAD')) -ceq $candidateSha) 'Candidate commit not restored.'
    Assert-True ((Invoke-FixtureGit @('rev-parse', '--abbrev-ref', 'HEAD')) -ceq $branch) 'Candidate branch state not restored.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $releaseDir 'wavplay.exe')) -ceq 'candidate') 'Verified binary was overwritten.'
    if (Test-Path -LiteralPath $runnerTemp) {
        Assert-True (@([IO.Directory]::GetFileSystemEntries($runnerTemp)).Count -eq 0) 'Report artifacts escaped cleanup.'
    }
}

try {
    Invoke-WithoutGitEnvironment {
        New-LayoutRepo
        Test-LayoutEvents
        Test-LayoutRun 'success' '比較済み'
        Test-LayoutRun 'detached' '比較済み'
        Test-LayoutRun 'mismatch' '比較未実施（MSVCまたはWindows SDKの条件が異なる）'
        Test-LayoutRun 'sdk-mismatch' '比較未実施（ASIO SDKの一致を確認できない）'
        Test-LayoutRun 'fetch-failure' '比較失敗（基準コミットの取得）' -Fails
        Test-LayoutRun 'switch-failure' '比較失敗（基準コミットの取得）' -Fails
        Test-LayoutRun 'restore-failure' '比較失敗（候補コミットの復元）' -Fails
        Test-LayoutRun 'build-failure' '比較失敗（基準版のビルド）' -Fails
        Test-LayoutRun 'analysis-failure' '比較失敗（配置差分の解析）' -Fails
        Test-LayoutRun 'no-release' '比較未実施（候補版の検証未完了）'
        Test-LayoutRun 'no-baseline' '比較未実施（基準コミットなし）'
        Complete-TestGroup 'Release report, failure stages, bounded publication and restored checkout'
    }
    Write-Output ('[PASS] CI Release layout tests: ' + (Get-TestGroupCount) + ' groups')
} finally { Remove-StagingDirectory $work $workParent $workPrefix }
