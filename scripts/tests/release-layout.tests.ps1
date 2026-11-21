# SPDX-License-Identifier: GPL-3.0-only
# The fixture-level checks also run without a previous CI run or an ASIO driver.
# Exercise the CLI with the Release product just verified by this worker.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
foreach ($test in 'layout-image.tests.ps1', 'layout-instructions.tests.ps1', 'ci-release-layout.tests.ps1') {
    & (Join-Path $PSScriptRoot $test)
}

$scripts = Split-Path -Parent $PSScriptRoot
$out = Join-Path (Split-Path -Parent $scripts) 'out'
$release = Join-Path $out 'x64/Release'
$compare = Join-Path $scripts 'compare-release-layout.ps1'
$prefix = 'layout-report-test-'
$work = New-StagingDirectory $out $prefix
try {
    $report = Join-Path $work 'comparison.json'
    Write-TestFile $report '{}' # Exercise replacement of an earlier report.
    & $compare -BaselineDir $release -CandidateDir $release -ReportPath $report | Out-Null
    $result = [IO.File]::ReadAllText($report) | ConvertFrom-Json
    $expectedHash = (Get-FileHash -LiteralPath (Join-Path $release 'wavplay.exe') -Algorithm SHA256).Hash
    Assert-True ($result.BaselineSha256 -eq $expectedHash -and $result.CandidateSha256 -eq $expectedHash) 'The report does not identify its input.'
    foreach ($label in 'switch_buffer', 'callback_state', 'fill_slots') {
        $row = @($result.Summary.Anchors | Where-Object Label -ceq $label)
        Assert-True ($row.Count -eq 1 -and $null -ne $row[0].Delta -and $row[0].Delta -eq 0) "No matching $label in self-comparison."
    }
    foreach ($group in $result.Summary.Groups) {
        Assert-True ($group.Matched -gt 0 -and $group.Moved -eq 0) 'Group coverage or movement is incorrect for identical builds.'
    }
    foreach ($section in $result.Summary.Sections) {
        Assert-True ($section.Status -ceq 'Matched' -and $section.DifferentBytes -eq 0 -and
            $section.BaselineOnlyBytes -eq 0 -and $section.CandidateOnlyBytes -eq 0) 'Section differs from itself.'
    }
    $instructions = $result.Summary.Instructions
    Assert-True ($null -ne $instructions.BaselineCrossings -and
        $instructions.BaselineCrossings -eq $instructions.CandidateCrossings) 'Self-comparison lost its instruction coverage.'
    Complete-TestGroup 'identical product images have matching symbols, instructions and section bytes'

    # A mixed pair must fail before it can overwrite a previously useful report.
    $copy = Join-Path $work 'build'
    [void][IO.Directory]::CreateDirectory($copy)
    Copy-Item -LiteralPath (Join-Path $release 'wavplay.exe') -Destination $copy
    $map = [IO.File]::ReadAllText((Join-Path $release 'wavplay.map'))
    $identity = [regex]::Match($map, '(?im)^\s*Timestamp is ([0-9a-f]{1,8})\b')
    Assert-True $identity.Success 'The Release map is missing its timestamp.'
    $stamp = $identity.Groups[1]
    $other = ([Convert]::ToUInt32($stamp.Value, 16) -bxor 1).ToString('x8')
    Write-TestFile (Join-Path $copy 'wavplay.map') ($map.Remove($stamp.Index, $stamp.Length).Insert($stamp.Index, $other))
    $oldHash = (Get-FileHash -LiteralPath $report).Hash
    Assert-Fails { & $compare -BaselineDir $release -CandidateDir $copy -ReportPath $report } 'MAP build identity does not match'
    Assert-True ((Get-FileHash -LiteralPath $report).Hash -ceq $oldHash) 'Rejected inputs damaged the previous report.'
    Complete-TestGroup 'mismatched map fails without replacing the report'
} finally {
    Remove-StagingDirectory $work $out $prefix
}
