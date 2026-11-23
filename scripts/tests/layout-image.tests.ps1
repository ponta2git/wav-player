# SPDX-License-Identifier: GPL-3.0-only
# Small, synthetic maps disambiguate cases hard to isolate in a real build.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'verification/layout-image.ps1')

function New-Symbol([string]$Name, [string]$Object, [uint32]$Rva) {
    [pscustomobject]@{ Name = $Name; Object = $Object; Rva = $Rva; Section = '.text'; IsFunction = $true; Group = 'Other' }
}

# The streaming paths are named by decorated function names. A function that
# runs only on a state change, a failure or a dispatched message is not a member.
foreach ($case in @(
    @('?switch_buffer@?A0xfa60603d@playback@wavplay@@YAXJJ@Z', 'Callback'),
    @('?fill_slots@Impl@PrefetchRing@stream@wavplay@@QEAAXAEAVGaplessRun@34@_K1_N@Z', 'Refill'),
    @('?wait_for_stop@SegmentRunner@?A0xab3cf866@playback@wavplay@@AEAAXXZ', 'Control'),
    @('?wait_for_events_until@core@wavplay@@YA?AUEventWait@12@XZ', 'Control'),
    @('?poll@Impl@PlaybackController@control@wavplay@@SAXPEAX@Z', 'Control'),
    @('??R<lambda_1>@?BB@??play@TrackPlayer@playback@wavplay@@QEAA?AW4PlaybackResult@34@XZ@QEBA@XZ', 'Control'),
    @('?<lambda_invoker_cdecl>@<lambda_3>@?BB@??play@TrackPlayer@playback@wavplay@@QEAA?AW4PlaybackResult@45@XZ@SA@PEAX@Z', 'Control'),
    @('?progressed@StateNotification@?A0x379dfc1b@ui@wavplay@@SAXPEAX_K@Z', 'Control'),
    @('?changed@StateNotification@?A0x379dfc1b@ui@wavplay@@SAXPEAX_K@Z', 'Other'),
    @('?pump_messages@core@wavplay@@YA_NXZ', 'Other'),
    @('?deliver_active@Impl@PlaybackController@control@wavplay@@QEAAXXZ', 'Other'),
    @('?refresh@PlayerWindow@?A0x379dfc1b@ui@wavplay@@AEAAXXZ', 'Display'),
    @('?window_proc@PlayerWindow@?A0x379dfc1b@ui@wavplay@@CA_JPEAUHWND__@@I_K_J@Z', 'Display'),
    @('?playback_time_text@ui@wavplay@@YAXAEAV?$basic_string@_W@std@@_K1I@Z', 'Display'),
    @('?refresh_failed@PlayerWindow@?A0x379dfc1b@ui@wavplay@@AEAAXXZ', 'Other'),
    @('?render_controls@PlayerWindow@?A0x379dfc1b@ui@wavplay@@AEAAXAEBUPlaybackSnapshot@control@3@_N@Z', 'Other'))) {
    $symbol = [pscustomobject]@{ Name = $case[0]; IsFunction = $true }
    Assert-True ((Get-LayoutSymbolGroup $symbol) -ceq $case[1]) "Unexpected path group for $($case[0])."
}
$data = [pscustomobject]@{ Name = '?poll@Impl@PlaybackController@control@wavplay@@SAXPEAX@Z'; IsFunction = $false }
Assert-True ((Get-LayoutSymbolGroup $data) -ceq 'Other') 'A data symbol joined a function path.'
Complete-TestGroup 'Streaming path membership by decorated name'

$baseline = [pscustomobject]@{
    Sha256 = 'left'
    Symbols = @(
        New-Symbol '?work@?A0x1a@sample@@' 'part.obj' 256
        New-Symbol 'alias' 'part.obj' 256
        New-Symbol 'Other' 'other.obj' 320
        New-Symbol 'other' 'other.obj' 344
        New-Symbol 'same' 'one.obj' 384
        New-Symbol 'local' 'part.obj' 400
        New-Symbol 'local' 'part.obj' 416
    )
}
$candidate = [pscustomobject]@{
    Sha256 = 'right'
    Symbols = @(
        New-Symbol '?work@?A0xff@sample@@' 'part.obj' 240
        New-Symbol 'alias' 'part.obj' 240
        New-Symbol 'Other' 'other.obj' 304
        New-Symbol 'other' 'other.obj' 336
        New-Symbol 'same' 'two.obj' 384
        New-Symbol 'local' 'part.obj' 400
        New-Symbol 'local' 'part.obj' 424
    )
}
$comparison = Compare-LayoutSymbols $baseline $candidate
$work = @($comparison.Items | Where-Object Name -CEQ '?work@?A0x@sample@@')
Assert-True ($work.Count -eq 1 -and $work[0].Status -eq 'Matched' -and $work[0].Delta -eq -16 -and
    $work[0].BaselineLineOffset -eq 0 -and $work[0].CandidateLineOffset -eq 48) 'Lost normalized symbol identity or signed movement.'
Assert-True ($comparison.Aliases.Baseline.Count -eq 1 -and $comparison.Aliases.Baseline[0].Symbols.Count -eq 2 -and
    $comparison.Aliases.Candidate[0].Rva -eq 240) 'The two names for a shared entry point must be reported.'
Assert-True ($comparison.MovementRuns.Count -eq 2 -and $comparison.MovementRuns[0].FunctionCount -eq 2 -and
    $comparison.MovementRuns[0].BaselineLastRva -eq 320 -and $comparison.MovementRuns[1].Delta -eq -8) 'Consecutive entries or folded aliases were counted incorrectly.'
$separate = @($comparison.Items | Where-Object Name -CEQ 'same')
Assert-True ($separate.Count -eq 2 -and 'Added' -in $separate.Status -and 'Removed' -in $separate.Status) 'A name in different objects is not sufficient to match.'
$uncertain = @($comparison.Items | Where-Object Name -CEQ 'local')
Assert-True ($uncertain.Count -eq 4 -and -not @($uncertain | Where-Object { $_.Status -ne 'Ambiguous' -or $null -ne $_.Delta }).Count) 'Repeated local names must retain every possible location.'
Complete-TestGroup 'Layout symbol identity and movement runs'

# Different raw offsets contain matching section bytes. Count the unmatched
# tail and the completely new section separately from changed common bytes.
$baseline | Add-Member Pe ([pscustomobject]@{
    Headers = 2; Bytes = [byte[]](0, 1, 11, 12, 90)
    Sections = @([pscustomobject]@{ Name = '.text'; Raw = 2; Size = 2 }, [pscustomobject]@{ Name = '.gone'; Raw = 4; Size = 1 })
})
$candidate | Add-Member Pe ([pscustomobject]@{
    Headers = 2; Bytes = [byte[]](0, 2, 72, 11, 10, 13)
    Sections = @([pscustomobject]@{ Name = '.new'; Raw = 2; Size = 1 }, [pscustomobject]@{ Name = '.text'; Raw = 3; Size = 3 })
})
$rows = @{}
foreach ($section in (Compare-LayoutSections $baseline $candidate)) { $rows[$section.Name] = $section }
Assert-True ($rows.Count -eq 4 -and $rows['Headers'].DifferentBytes -eq 1 -and $rows['.text'].ComparedBytes -eq 2 -and
    $rows['.text'].DifferentBytes -eq 1 -and
    $rows['.text'].CandidateOnlyBytes -eq 1 -and $rows['.text'].BaselineOnlyBytes -eq 0) 'Section-relative differences or tails are incorrect.'
Assert-True ($rows['.gone'].Status -eq 'Removed' -and $rows['.gone'].BaselineOnlyBytes -eq 1 -and
    $rows['.new'].Status -eq 'Added' -and $rows['.new'].CandidateOnlyBytes -eq 1 -and $rows['.new'].DifferentBytes -eq 0) 'One-sided sections must retain their direction and size.'
Complete-TestGroup 'Layout section tails and one-sided sections'
