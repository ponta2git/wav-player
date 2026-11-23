# SPDX-License-Identifier: GPL-3.0-only
# Compare two completed product builds. All positions are RVAs in the linked
# images; the two players are never started.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaselineDir,
    [Parameter(Mandatory)][string]$CandidateDir,
    [Parameter(Mandatory)][string]$ReportPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/visual-studio.ps1')
. (Join-Path $PSScriptRoot 'verification/disassembly.ps1')
. (Join-Path $PSScriptRoot 'verification/linked-image.ps1')
. (Join-Path $PSScriptRoot 'verification/layout-image.ps1')
. (Join-Path $PSScriptRoot 'verification/layout-instructions.ps1')

function Get-LayoutSummary($Symbols, $Instructions, $Sections) {
    $groupsByAnchor = [ordered]@{ switch_buffer = 'Callback'; callback_state = 'State'; fill_slots = 'Refill' }
    $anchors = foreach ($label in $groupsByAnchor.Keys) {
        $rows = @($Symbols.Items | Where-Object {
            $_.Group -ceq $groupsByAnchor[$label] -and $_.Name.StartsWith(('?' + $label + '@'), [StringComparison]::Ordinal)
        })
        $delta = if ($rows.Count -eq 1) { $rows[0].Delta }
        [pscustomobject]@{ Label = $label; Delta = $delta }
    }
    $groups = foreach ($entry in ([ordered]@{ Kernels = 'Kernel'; Constants = 'Constant' }).GetEnumerator()) {
        $items = @($Symbols.Items | Where-Object Group -ceq $entry.Value)
        $matched = @($items | Where-Object Status -ceq 'Matched')
        $moved = @($matched | Where-Object Delta -ne 0)
        $minimum = $maximum = $null
        if ($moved.Count) {
            $range = $moved | Measure-Object -Property Delta -Minimum -Maximum
            $minimum = [long]$range.Minimum
            $maximum = [long]$range.Maximum
        } elseif ($matched.Count) {
            $minimum = $maximum = 0
        }
        [pscustomobject]@{
            Label = $entry.Key
            Matched = $matched.Count
            Unmatched = $items.Count - $matched.Count
            Moved = $moved.Count
            MinDelta = $minimum
            MaxDelta = $maximum
        }
    }
    [pscustomobject]@{
        Anchors = @($anchors)
        Groups = @($groups)
        Instructions = [pscustomobject]@{
            Status = $Instructions.Status
            BaselineCrossings = $Instructions.BaselineCrossings
            CandidateCrossings = $Instructions.CandidateCrossings
        }
        Footprints = $Instructions.Footprints
        Sections = $Sections
    }
}

# Prepare the entire report beside its destination before publishing it. An
# interrupted comparison leaves the previous complete report in place.
function Write-LayoutReport([string]$Path, $Report) {
    $parent = [IO.Path]::GetDirectoryName($Path)
    [void][IO.Directory]::CreateDirectory($parent)
    $temporary = Join-Path $parent ([IO.Path]::GetRandomFileName())
    try {
        $json = ConvertTo-Json -InputObject $Report -Depth 12
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        if ([IO.File]::Exists($Path)) {
            [IO.File]::Replace($temporary, $Path, [System.Management.Automation.Language.NullString]::Value)
        } else {
            [IO.File]::Move($temporary, $Path)
        }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ReportPath)
$baseline = Read-LayoutImage -Directory $BaselineDir
$candidate = Read-LayoutImage -Directory $CandidateDir
foreach ($build in $baseline, $candidate) {
    foreach ($extension in '.exe', '.map', '.pdb') {
        if ($destination -eq [IO.Path]::ChangeExtension($build.ExePath, $extension)) {
            throw 'The report path must be separate from the input files.'
        }
    }
}
$symbols = Compare-LayoutSymbols $baseline $candidate
$sections = @(Compare-LayoutSections $baseline $candidate)
$installation = Find-VisualStudio -AllowMissing
$dumpbin = if ($installation) {
    $tools = Get-MsvcTools (Join-Path $installation ('VC\Tools\MSVC\' + (Get-RequiredMsvcVersion)))
    $tools.Dumpbin
}
$instructions = Compare-LayoutInstructions -Baseline $baseline -Candidate $candidate -SymbolComparison $symbols -Dumpbin $dumpbin
$summary = Get-LayoutSummary $symbols $instructions $sections
$report = [pscustomobject]@{
    BaselineSha256 = $baseline.Sha256
    CandidateSha256 = $candidate.Sha256
    Summary = $summary
    Symbols = $symbols
    Instructions = $instructions
}
Write-LayoutReport $destination $report
foreach ($anchor in $summary.Anchors) {
    $movement = if ($null -eq $anchor.Delta) { 'unresolved' } else { '{0:+0;-0;0} bytes' -f $anchor.Delta }
    Write-Output ('{0}: {1}' -f $anchor.Label, $movement)
}
foreach ($group in $summary.Groups) {
    Write-Output ('{0}: {1} moved of {2} matched; {3} unmatched' -f $group.Label, $group.Moved, $group.Matched, $group.Unmatched)
}
Write-Output ('Instruction comparison: ' + $instructions.Status)
foreach ($footprint in $summary.Footprints) {
    Write-Output ('{0} path: {1} -> {2} pages, {3} -> {4} lines' -f $footprint.Path,
        $footprint.Baseline.Pages, $footprint.Candidate.Pages, $footprint.Baseline.Lines, $footprint.Candidate.Lines)
}
Write-Output ('Layout report: ' + $destination)
