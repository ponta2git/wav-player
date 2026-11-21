# SPDX-License-Identifier: GPL-3.0-only
# Run on the Release worker after verify.ps1. Only the bounded projection below
# reaches the public summary. Native tool output stays inside this process.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/visual-studio.ps1')
. (Join-Path $PSScriptRoot 'support/staging.ps1')

function Get-LayoutCommandOutput([string]$Program, [string[]]$Arguments) {
    # Windows PowerShell 5.1 can turn native stderr into ErrorRecords. An
    # exit code is authoritative even when a tool writes progress to stderr.
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Program @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'Native tool failed.' }
    foreach ($line in $lines) { [string]$line }
}

function Get-LayoutSha([string]$Value) {
    if ($Value -cnotmatch '\A[0-9a-fA-F]{40}\z' -or $Value -ceq ('0' * 40)) {
        throw 'Expected a full commit SHA.'
    }
    $Value.ToLowerInvariant()
}

function Get-LayoutBaseline([string]$EventName, $EventData, [string]$Root, [string]$Candidate) {
    switch -CaseSensitive ($EventName) {
        'pull_request' { return Get-LayoutSha $EventData.pull_request.base.sha }
        'push' {
            if ($EventData.before -ceq ('0' * 40)) { return }
            return Get-LayoutSha $EventData.before
        }
        'workflow_dispatch' {
            $provided = $EventData.inputs.PSObject.Properties['baseline_sha']
            if ($provided -and -not [string]::IsNullOrWhiteSpace($provided.Value)) {
                return Get-LayoutSha $provided.Value
            }
            # The Release checkout has full history. A root commit has no parent.
            $parents = Get-LayoutCommandOutput git @('-C', $Root, 'show', '-s', '--format=%P', $Candidate)
            if ($parents) { return Get-LayoutSha ($parents.Split(' ')[0]) }
        }
        default { throw 'Unsupported event.' }
    }
}

# The same downloader has the same provenance even if it changes its pin syntax.
# When downloader revisions differ, compare their literal verified archive pins.
function Get-LayoutAsioSource([string]$Root) {
    $fetch = Join-Path $Root 'scripts/fetch-asio-sdk.ps1'
    if (-not [IO.File]::Exists($fetch)) { return }
    $pins = [regex]::Matches([IO.File]::ReadAllText($fetch), '(?m)^\s*\$expectedHash\s*=\s*''([0-9A-Fa-f]{64})''\s*$')
    [pscustomobject]@{
        ScriptHash = (Get-FileHash -LiteralPath $fetch -Algorithm SHA256).Hash
        ArchiveHash = if ($pins.Count -eq 1) { $pins[0].Groups[1].Value.ToUpperInvariant() }
    }
}

function Test-LayoutAsioSource($Baseline, $Candidate) {
    if (-not $Baseline -or -not $Candidate) { return $false }
    $Baseline.ScriptHash -ceq $Candidate.ScriptHash -or
        ($null -ne $Baseline.ArchiveHash -and $Baseline.ArchiveHash -ceq $Candidate.ArchiveHash)
}

# Return the effective compiler and SDK settings for both production projects.
# With a separate BaseOutputPath, refuse any old project that escapes that root.
function Get-LayoutBuildSettings([string]$Root, [string]$Msbuild, [string[]]$Options, [string]$OutputRoot) {
    $properties = 'PlatformToolset,VCToolsVersion,VCToolsInstallDir,WindowsTargetPlatformVersion,WindowsSdkDir,OutDir,IntDir'
    $prefix = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\', '/') + '\'
    $keys = @(foreach ($relative in 'src/app/wavplay.vcxproj', 'src/wavplay/wavplay_core.vcxproj') {
        $project = Join-Path $Root $relative
        $json = Get-LayoutCommandOutput $Msbuild (@($project, '/nologo', "/getProperty:$properties") + $Options)
        $values = ($json -join "`n" | ConvertFrom-Json).Properties
        foreach ($name in 'OutDir', 'IntDir') {
            $path = [IO.Path]::GetFullPath($values.$name)
            if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Unexpected build destination.'
            }
        }
        # A native PS 5.1 child can misread a quoted argument ending in a backslash.
        if ($relative -eq 'src/app/wavplay.vcxproj') { $productDir = [IO.Path]::GetFullPath($values.OutDir).TrimEnd('\', '/') }
        foreach ($name in 'PlatformToolset', 'VCToolsVersion', 'VCToolsInstallDir', 'WindowsTargetPlatformVersion', 'WindowsSdkDir') {
            if ([string]::IsNullOrWhiteSpace($values.$name)) { throw 'Unknown effective toolchain.' }
            $values.$name
        }
    })
    [pscustomobject]@{ Toolchain = $keys -join '|'; ProductDir = $productDir }
}

function Format-LayoutNumber($Value, [switch]$Signed) {
    if ($null -eq $Value) { return '—' }
    if ($Value -isnot [int] -and $Value -isnot [long]) { throw 'Invalid summary integer.' }
    if (-not $Signed -and $Value -lt 0) { throw 'Invalid negative count.' }
    $number = $Value.ToString([Globalization.CultureInfo]::InvariantCulture)
    if ($Signed -and $Value -gt 0) { return '+' + $number }
    $number
}

function Get-LayoutSummaryTable($Summary) {
    '| 対象 | 移動量（バイト） |'
    '|---|---:|'
    foreach ($name in 'switch_buffer', 'callback_state', 'fill_slots') {
        $rows = @($Summary.Anchors | Where-Object { $_.Label -ceq $name })
        if ($rows.Count -ne 1) { throw 'Missing or duplicate anchor.' }
        '| `{0}` | {1} |' -f $name, (Format-LayoutNumber $rows[0].Delta -Signed)
    }
    ''
    '| 対象 | 照合 | 移動 | 未照合 | 移動量の範囲（バイト） |'
    '|---|---:|---:|---:|---|'
    foreach ($name in 'Kernels', 'Constants') {
        $rows = @($Summary.Groups | Where-Object { $_.Label -ceq $name })
        if ($rows.Count -ne 1) { throw 'Missing or duplicate symbol group.' }
        $group = $rows[0]
        $label = if ($name -ceq 'Kernels') { '変換カーネル' } else { 'SIMD定数' }
        '| {0} | {1} | {2} | {3} | {4} ～ {5} |' -f $label, (Format-LayoutNumber $group.Matched),
            (Format-LayoutNumber $group.Moved), (Format-LayoutNumber $group.Unmatched),
            (Format-LayoutNumber $group.MinDelta -Signed), (Format-LayoutNumber $group.MaxDelta -Signed)
    }
    $instructionStatus = switch -CaseSensitive ($Summary.Instructions.Status) {
        'Compared' { '比較済み' }
        'Partial' { '一部のみ' }
        'Unavailable' { '判定不能' }
        default { throw 'Unknown instruction coverage.' }
    }
    ''
    '64バイト境界をまたぐ命令（{0}）　基準 {1} 件 ／ 候補 {2} 件' -f $instructionStatus,
        (Format-LayoutNumber $Summary.Instructions.BaselineCrossings), (Format-LayoutNumber $Summary.Instructions.CandidateCrossings)
    ''
    '| セクション | 共通範囲で異なるバイト | 基準だけのバイト | 候補だけのバイト |'
    '|---|---:|---:|---:|'
    foreach ($name in '.text', '.rdata', '.data', '.pdata', '.rsrc', '.reloc', '.cbstate') {
        $rows = @($Summary.Sections | Where-Object { $_.Name -ceq $name })
        if ($rows.Count -gt 1) { throw 'Duplicate section.' }
        if ($rows.Count) {
            $section = $rows[0]
            '| `{0}` | {1} | {2} | {3} |' -f $name, (Format-LayoutNumber $section.DifferentBytes),
                (Format-LayoutNumber $section.BaselineOnlyBytes), (Format-LayoutNumber $section.CandidateOnlyBytes)
        }
    }
}

# The only operation that changes the checkout. Once switched, every exit
# restores the candidate before its comparison tool is invoked.
function Invoke-LayoutBaselineBuild {
    param([string]$Root, [string]$Baseline, [string]$Candidate, [string]$BuildRoot,
        [string]$Msbuild, [string[]]$Options, [string]$Toolchain, $AsioSource)
    $build = [pscustomobject]@{ Directory = $null; SkipReason = $null; FailedStage = $null }
    $stage = '基準コミットの取得'
    try {
        $branch = Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', '--abbrev-ref', 'HEAD')
        try { $type = Get-LayoutCommandOutput git @('-C', $Root, 'cat-file', '-t', $Baseline) } catch {
            # For example, a force-push's previous tip may have no ref.
            $null = Get-LayoutCommandOutput git @('-C', $Root, 'fetch', '--no-tags', '--depth=1', 'origin', $Baseline)
            $type = Get-LayoutCommandOutput git @('-C', $Root, 'cat-file', '-t', $Baseline)
        }
        if ($type -cne 'commit') { throw 'Expected a commit.' }
        try {
            $null = Get-LayoutCommandOutput git @('-C', $Root, 'switch', '--quiet', '--detach', $Baseline)
            $stage = 'ビルド条件の確認'
            if (-not (Test-LayoutAsioSource (Get-LayoutAsioSource $Root) $AsioSource)) {
                $build.SkipReason = 'ASIO SDKの一致を確認できない'
                return $build
            }
            # A fresh intermediate tree retains the absolute source path. End
            # the native property argument with '/' so PS 5.1 can quote spaces.
            $buildOptions = $Options + "/p:BaseOutputPath=$BuildRoot/"
            $old = Get-LayoutBuildSettings $Root $Msbuild $buildOptions $BuildRoot
            if ($old.Toolchain -ne $Toolchain) {
                $build.SkipReason = 'MSVCまたはWindows SDKの条件が異なる'
                return $build
            }
            $stage = '基準版のビルド'
            $project = Join-Path $Root 'src/app/wavplay.vcxproj'
            $null = Get-LayoutCommandOutput $Msbuild (@($project, '/nologo', '/verbosity:quiet', '/t:Rebuild', '/m') + $buildOptions)
            $build.Directory = $old.ProductDir
        } finally {
            try {
                # A post-checkout hook can fail after HEAD has already changed.
                $at = Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', 'HEAD')
                $attached = Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', '--abbrev-ref', 'HEAD')
                if ($at -cne $Candidate -or $attached -cne $branch) {
                    if ($branch -ceq 'HEAD') {
                        $null = Get-LayoutCommandOutput git @('-C', $Root, 'switch', '--quiet', '--detach', $Candidate)
                    } else { $null = Get-LayoutCommandOutput git @('-C', $Root, 'switch', '--quiet', $branch) }
                }
                if ((Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', 'HEAD')) -cne $Candidate -or
                    (Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', '--abbrev-ref', 'HEAD')) -cne $branch -or
                    @(Get-LayoutCommandOutput git @('-C', $Root, 'status', '--porcelain')).Count) {
                    throw 'Candidate checkout was not restored.'
                }
            } catch { $stage = '候補コミットの復元'; throw }
        }
    } catch { $build.FailedStage = $stage }
    $build
}

function Invoke-ReleaseLayoutReport {
    param([string]$Root, [string]$TempRoot, [string]$SummaryPath, [string]$EventName,
        [string]$EventPath, [string]$CandidateSha, [string]$ReleaseOutcome, [string]$StepOutput)

    $baseline = $null
    $candidate = $null
    $staging = $null
    $status = 'skipped'
    $stage = '比較元の選択'
    $result = '比較未実施'
    $details = @()
    try {
        $candidate = Get-LayoutSha $CandidateSha
        $eventData = Get-Content -LiteralPath $EventPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $baseline = Get-LayoutBaseline $EventName $eventData $Root $candidate
        if ($ReleaseOutcome -cne 'success') { $result = '比較未実施（候補版の検証未完了）'; return }
        if (-not $baseline) { $result = '比較未実施（基準コミットなし）'; return }
        $stage = '候補版の確認'
        $head = Get-LayoutCommandOutput git @('-C', $Root, 'rev-parse', 'HEAD')
        $changes = @(Get-LayoutCommandOutput git @('-C', $Root, 'status', '--porcelain'))
        if ($head -cne $candidate -or $changes.Count) { throw 'The checkout must match the verified commit.' }
        $msbuild = Join-Path (Find-VisualStudio) 'MSBuild/Current/Bin/amd64/MSBuild.exe'
        $options = @('/p:Platform=x64', '/p:Configuration=Release')
        $diagnostic = $EventName -ceq 'workflow_dispatch' -and $eventData.inputs.PSObject.Properties['release_only'] -and
            [string]$eventData.inputs.release_only -eq 'true'
        if (-not $diagnostic) { $options += '/p:RunCodeAnalysis=true', '/p:CodeAnalysisTreatWarningsAsErrors=true' }
        $current = Get-LayoutBuildSettings $Root $msbuild $options (Join-Path $Root 'out')
        $asioSource = Get-LayoutAsioSource $Root
        $staging = New-StagingDirectory $TempRoot 'release-layout-'
        $candidateDir = Join-Path $staging 'candidate'
        [void][IO.Directory]::CreateDirectory($candidateDir)
        foreach ($file in 'wavplay.exe', 'wavplay.map', 'wavplay.pdb') {
            [IO.File]::Copy((Join-Path $current.ProductDir $file), (Join-Path $candidateDir $file))
        }

        $baselineDir = $candidateDir
        if ($baseline -cne $candidate) {
            $build = Invoke-LayoutBaselineBuild -Root $Root -Baseline $baseline -Candidate $candidate -BuildRoot (Join-Path $staging 'build') `
                -Msbuild $msbuild -Options $options -Toolchain $current.Toolchain -AsioSource $asioSource
            if ($build.FailedStage) { $stage = $build.FailedStage; throw 'Baseline preparation failed.' }
            if ($build.SkipReason) { $result = '比較未実施（' + $build.SkipReason + '）'; return }
            $baselineDir = $build.Directory
        }
        $stage = '配置差分の解析'
        $report = Join-Path $staging 'report.json'
        $engine = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $null = Get-LayoutCommandOutput $engine @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', (Join-Path $Root 'scripts/compare-release-layout.ps1'), '-BaselineDir', $baselineDir,
            '-CandidateDir', $candidateDir, '-ReportPath', $report)
        $summary = (Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json).Summary
        $details = @(Get-LayoutSummaryTable $summary)
        $result = '比較済み'
        $status = 'compared'
    } catch {
        $status = 'failed'
        $result = "比較失敗（$stage）"
    } finally {
        if ($staging) { Remove-StagingDirectory $staging $TempRoot 'release-layout-' -WarningAction SilentlyContinue }
        $lines = @('## Release 配置差分', '', $result, '')
        if ($baseline) { $lines += '- 基準 `{0}`' -f $baseline }
        if ($candidate) { $lines += '- 候補 `{0}`' -f $candidate }
        $lines += ''
        $lines += $details
        [IO.File]::AppendAllText($SummaryPath, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
        if ($StepOutput) {
            [IO.File]::AppendAllText($StepOutput, "summary_written=true`n", [Text.UTF8Encoding]::new($false))
        }
        Write-Output "Release layout summary: $status"
    }
    if ($status -ceq 'failed') { throw 'Release layout comparison failed. See the job summary for the stage.' }
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($env:GITHUB_ACTIONS -cne 'true' -or
        [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE) -ne (Split-Path -Parent $PSScriptRoot)) {
        throw 'Run this wrapper in the checked-out Actions Release workspace.'
    }
    $env:GIT_TERMINAL_PROMPT = '0'
    Invoke-ReleaseLayoutReport $env:GITHUB_WORKSPACE $env:RUNNER_TEMP $env:GITHUB_STEP_SUMMARY $env:GITHUB_EVENT_NAME `
        $env:GITHUB_EVENT_PATH $env:GITHUB_SHA $env:WAVPLAY_RELEASE_OUTCOME $env:GITHUB_OUTPUT
}
