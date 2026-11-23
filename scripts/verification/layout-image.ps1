# SPDX-License-Identifier: GPL-3.0-only
# Uses the same PE and MAP readers as the startup audit. These groups describe
# symbols present in the final image; some functions can also be inlined.
# Callback, Refill, Control and Display are the paths that run while a segment
# streams; layout-instructions.ps1 reports the pages and lines each one occupies.
function Get-LayoutSymbolGroup($Symbol) {
    $name = $Symbol.Name
    if (-not $Symbol.IsFunction) {
        if ($name -cmatch '^\?callback_state@\?A0x[0-9a-fA-F]+@playback@wavplay@@3') { return 'State' }
        if ($name -cmatch '^__(?:xmm|real)@[0-9a-fA-F]+$') { return 'Constant' }
        return 'Other'
    }
    if ($name -cmatch '^\?(?:switch_buffer|sample_rate_changed|asio_message)@\?A0x[0-9a-fA-F]+@playback@wavplay@@' -or
        $name -cmatch '^\?(?:silence@SegmentRenderer|(?:stop|invalidate)@PlaybackSignal|is_streaming)@playback@wavplay@@' -or
        $name -cmatch '^\?pop_block@PrefetchRing@stream@wavplay@@') { return 'Callback' }
    if ($name -cmatch '^\?(?:run_producer|fill_slots|produce)@Impl@PrefetchRing@stream@wavplay@@' -or
        $name -cmatch '^\?(?:read|advance_track|next_batch_blocks)@GaplessRun@stream@wavplay@@' -or
        $name -cmatch '^\?(?:read_checked_planar@WavReader|read_exact_at@ReadOnlyFile)@media@wavplay@@') { return 'Refill' }
    # Match the owning namespace, not an audio type in another function's signature.
    if ($name -cmatch '^\?(?:[^@]+@)+@?\?A0x[0-9a-fA-F]+@audio@wavplay@@Y') { return 'Kernel' }
    # What the control thread enters at every periodic wake while a segment
    # streams: its timed wait, the request's observers, the controller's
    # progress poll and the listener of a progress publication. Message
    # dispatch and the listener of state changes run only when something else
    # happens, so they are not members.
    if ($name -cmatch '^\?wait_for_stop@SegmentRunner@\?A0x[0-9a-fA-F]+@playback@wavplay@@' -or
        $name -cmatch '^\?wait_until@PlaybackSignal@playback@wavplay@@' -or
        $name -cmatch '^\?(?:wait_for_events?_until|quit_pending)@core@wavplay@@' -or
        $name -cmatch '^\?(?:\?R|<lambda_invoker_cdecl>@)<lambda_\d+>@\?[0-9A-Z]+@\?\?play@TrackPlayer@playback@wavplay@@' -or
        $name -cmatch '^\?(?:poll|publish_progress_and_check_commands|update_position_locked)@Impl@PlaybackController@control@wavplay@@' -or
        $name -cmatch '^\?progressed@StateNotification@(?:\?A0x[0-9a-fA-F]+@)?ui@wavplay@@') { return 'Control' }
    # What the GUI thread enters for one progress publication: the message loop,
    # the window procedure, the refresh and the time text it renders.
    if ($name -cmatch '^\?(?:run|translate|window_proc|refresh)@PlayerWindow@\?A0x[0-9a-fA-F]+@ui@wavplay@@' -or
        $name -cmatch '^\?refresh_snapshot@(?:Impl@)?PlaybackController@control@wavplay@@' -or
        $name -cmatch '^\?playback_time_text@ui@wavplay@@' -or
        $name -cmatch '^\?text_reuse@PlayerControls@ui@wavplay@@') { return 'Display' }
    'Other'
}

function Read-LayoutImage([string]$Directory) {
    $exe = (Get-Item -LiteralPath (Join-Path $Directory 'wavplay.exe') -ErrorAction Stop).FullName
    $pe = Read-PeImage $exe
    $map = Read-LinkerMap ([IO.Path]::ChangeExtension($exe, '.map'))
    if ($map.Timestamp -ne $pe.Timestamp -or $map.PreferredBase -ne $pe.Base) {
        throw 'MAP build identity does not match the PE image.'
    }
    if (-not $map.HasSymbolTables -or $map.InvalidSymbolRows) { throw 'Incomplete or malformed MAP symbol tables.' }
    if ($map.EntrySection -lt 1 -or $map.EntrySection -gt $pe.Sections.Count -or
        $pe.Base + $pe.Sections[$map.EntrySection - 1].Rva + $map.EntryOffset -ne $pe.EntryPoint) {
        throw 'MAP entry point does not match the PE image.'
    }
    $symbols = [Collections.Generic.List[object]]::new()
    foreach ($entries in $map.Symbols.Values) {
        foreach ($symbol in $entries) {
            # Segment zero holds absolute linker constants, outside any PE section.
            if ($symbol.SectionIndex -eq 0) { continue }
            if ($symbol.SectionIndex -gt $pe.Sections.Count) { throw 'MAP refers to a missing PE section.' }
            $section = $pe.Sections[$symbol.SectionIndex - 1]
            $rva = [long]$section.Rva + $symbol.Offset
            if ($symbol.Address -ne $pe.Base + $rva -or $symbol.Offset -gt [Math]::Max($section.Size, $section.VirtualSize)) {
                throw 'MAP symbol address does not agree with its PE section.'
            }
            $symbols.Add([pscustomobject]@{
                Name = $symbol.Name; Object = $symbol.Object; Rva = [uint32]$rva; Section = $section.Name
                IsFunction = $symbol.IsFunction; Group = Get-LayoutSymbolGroup $symbol
            })
        }
    }
    if (-not $symbols.Count) { throw 'MAP contains no image symbols.' }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash($pe.Bytes)).Replace('-', '') } finally { $sha.Dispose() }
    [pscustomobject]@{ Pe = $pe; ExePath = $exe; Sha256 = $hash; Symbols = $symbols.ToArray() }
}

# Keep every distinct location of a name/object pair, and every alias of a
# location. Repeated MAP rows for the same identity and RVA count only once.
function Get-LayoutSymbolIndex($Symbols) {
    $identities = [hashtable]::new([StringComparer]::Ordinal)
    $byRva = @{}
    foreach ($symbol in $Symbols) {
        $name = $symbol.Name -creplace '\?A0x[0-9a-fA-F]+@', '?A0x@'
        $key = $name + [char]0 + $symbol.Object
        if (-not $identities.ContainsKey($key)) { $identities[$key] = @{} }
        $locations = $identities[$key]
        $locations[$symbol.Rva] = $symbol
        if (-not $byRva.ContainsKey($symbol.Rva)) { $byRva[$symbol.Rva] = [hashtable]::new([StringComparer]::Ordinal) }
        $byRva[$symbol.Rva][$symbol.Name + [char]0 + $symbol.Object] = $symbol
    }
    $aliases = @(foreach ($rva in ($byRva.Keys | Sort-Object)) {
        if ($byRva[$rva].Count -gt 1) {
            $names = @($byRva[$rva].Values | Sort-Object -Property Name, Object -CaseSensitive | Select-Object Name, Object)
            [pscustomobject]@{ Rva = $rva; Symbols = $names }
        }
    })
    [pscustomobject]@{ Identities = $identities; Aliases = $aliases }
}

function New-LayoutSymbolRow([string]$Status, $Baseline, $Candidate) {
    $identity = $Baseline
    if ($null -eq $identity) { $identity = $Candidate }
    $row = [pscustomobject]@{
        Name = $identity.Name -creplace '\?A0x[0-9a-fA-F]+@', '?A0x@'
        Object = $identity.Object; Group = $identity.Group; IsFunction = $identity.IsFunction; Status = $Status
        BaselineRva = $null; BaselineSection = $null; BaselinePageOffset = $null; BaselineLineOffset = $null
        CandidateRva = $null; CandidateSection = $null; CandidatePageOffset = $null; CandidateLineOffset = $null
        Delta = $null
    }
    if ($Baseline) {
        $row.BaselineRva = $Baseline.Rva
        $row.BaselineSection = $Baseline.Section
        $row.BaselinePageOffset = $Baseline.Rva % 4096
        $row.BaselineLineOffset = $Baseline.Rva % 64
    }
    if ($Candidate) {
        $row.CandidateRva = $Candidate.Rva
        $row.CandidateSection = $Candidate.Section
        $row.CandidatePageOffset = $Candidate.Rva % 4096
        $row.CandidateLineOffset = $Candidate.Rva % 64
    }
    if ($Baseline -and $Candidate) { $row.Delta = [long]$Candidate.Rva - [long]$Baseline.Rva }
    $row
}

# Each run covers consecutive function entry points in baseline address order.
# Count folded aliases just once. A missing, ambiguous or split entry breaks the run.
function Get-LayoutMovementRuns($Items) {
    $entries = @{}
    foreach ($item in $Items) {
        if ($item.IsFunction -and $null -ne $item.BaselineRva) {
            $rva = $item.BaselineRva
            if (-not $entries.ContainsKey($rva)) { $entries[$rva] = $item }
            elseif ($entries[$rva] -and $entries[$rva].CandidateRva -ne $item.CandidateRva) { $entries[$rva] = $null }
        }
    }
    $run = $null
    foreach ($rva in ($entries.Keys | Sort-Object)) {
        $item = $entries[$rva]
        if ($null -eq $item -or $null -eq $item.Delta) {
            if ($run) { $run; $run = $null }
            continue
        }
        if ($run -and ($run.Delta -ne $item.Delta -or $run.BaselineSection -cne $item.BaselineSection -or
                       $run.CandidateSection -cne $item.CandidateSection)) { $run; $run = $null }
        if (-not $run) {
            $run = [pscustomobject]@{
                BaselineSection = $item.BaselineSection; CandidateSection = $item.CandidateSection; Delta = $item.Delta
                BaselineFirstRva = $rva; BaselineLastRva = $rva
                CandidateFirstRva = $item.CandidateRva; CandidateLastRva = $item.CandidateRva; FunctionCount = 0
            }
        }
        $run.BaselineLastRva = $rva
        $run.CandidateLastRva = $item.CandidateRva
        ++$run.FunctionCount
    }
    if ($run) { $run }
}

function Compare-LayoutSymbols($Baseline, $Candidate) {
    $left = Get-LayoutSymbolIndex $Baseline.Symbols
    $right = Get-LayoutSymbolIndex $Candidate.Symbols
    $keys = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    $keys.UnionWith([string[]]$left.Identities.Keys)
    $keys.UnionWith([string[]]$right.Identities.Keys)
    $sameImage = $Baseline.Sha256 -ceq $Candidate.Sha256
    $items = @(foreach ($key in $keys) {
        $before = $left.Identities[$key]
        $after = $right.Identities[$key]
        if ($null -eq $before) {
            foreach ($rva in ($after.Keys | Sort-Object)) { New-LayoutSymbolRow Added $null $after[$rva] }
        } elseif ($null -eq $after) {
            foreach ($rva in ($before.Keys | Sort-Object)) { New-LayoutSymbolRow Removed $before[$rva] $null }
        } elseif ($before.Count -eq 1 -and $after.Count -eq 1) {
            New-LayoutSymbolRow Matched @($before.Values)[0] @($after.Values)[0]
        } elseif ($sameImage -and $before.Count -eq $after.Count -and
                  -not @($before.Keys | Where-Object { -not $after.ContainsKey($_) }).Count) {
            foreach ($rva in ($before.Keys | Sort-Object)) { New-LayoutSymbolRow Matched $before[$rva] $after[$rva] }
        } else {
            # Do not pair repeated identities by order or by an accidentally equal address.
            foreach ($rva in ($before.Keys | Sort-Object)) { New-LayoutSymbolRow Ambiguous $before[$rva] $null }
            foreach ($rva in ($after.Keys | Sort-Object)) { New-LayoutSymbolRow Ambiguous $null $after[$rva] }
        }
    })
    [pscustomobject]@{
        Items = $items; MovementRuns = @(Get-LayoutMovementRuns $items)
        Aliases = [pscustomobject]@{ Baseline = $left.Aliases; Candidate = $right.Aliases }
    }
}

# Compare at the same offset within a named section; raw file offsets and the
# length of the two files can change independently. The PE headers are a
# separate row. An extra tail or a one-sided section is never a byte mismatch.
function Compare-LayoutSections($Baseline, $Candidate) {
    $names = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    $images = @($Baseline.Pe, $Candidate.Pe)
    $sides = @(foreach ($pe in $images) {
        $ranges = [hashtable]::new([StringComparer]::Ordinal)
        $ranges.Add('', [pscustomobject]@{ Raw = 0; Size = $pe.Headers })
        foreach ($section in $pe.Sections) {
            if ($ranges.ContainsKey($section.Name)) { throw 'PE section name is empty or duplicated.' }
            $ranges.Add($section.Name, $section)
        }
        $names.UnionWith([string[]]$ranges.Keys)
        $ranges
    })
    $leftBytes = $images[0].Bytes
    $rightBytes = $images[1].Bytes
    foreach ($name in $names) {
        $before = $sides[0][$name]
        $after = $sides[1][$name]
        $leftSize = if ($before) { [int]$before.Size } else { 0 }
        $rightSize = if ($after) { [int]$after.Size } else { 0 }
        $common = [Math]::Min($leftSize, $rightSize)
        $different = 0
        if ($common) {
            $leftStart = [int]$before.Raw
            $rightStart = [int]$after.Raw
            for ($i = 0; $i -lt $common; ++$i) {
                if ($leftBytes[$leftStart + $i] -ne $rightBytes[$rightStart + $i]) { ++$different }
            }
        }
        [pscustomobject]@{
            Name = if ($name) { $name } else { 'Headers' }
            Status = if ($null -eq $before) { 'Added' } elseif ($null -eq $after) { 'Removed' } else { 'Matched' }
            ComparedBytes = $common; DifferentBytes = $different
            BaselineOnlyBytes = $leftSize - $common; CandidateOnlyBytes = $rightSize - $common
        }
    }
}
