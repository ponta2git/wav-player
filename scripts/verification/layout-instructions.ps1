# SPDX-License-Identifier: GPL-3.0-only
# Instruction locations in an AMD64 PE. MAP identities come from layout-image;
# dumpbin supplies opcodes and lengths. Its optional PDB labels are not used to
# pair functions, identify a target, or decide where embedded data ends.

function Get-LayoutInstructionReference {
    param([byte[]]$Bytes, [int]$FileOffset, [int]$Length, [long]$Rva, [string]$Text)
    $opcode = $Bytes[$FileOffset]
    $kind = 'Branch'
    $width = 4
    if (($opcode -eq 0xE8 -or $opcode -eq 0xE9) -and $Length -eq 5) {
        $field = 1
    } elseif ($opcode -eq 0x0F -and $Length -eq 6 -and ($Bytes[$FileOffset + 1] -band 0xF0) -eq 0x80) {
        $field = 2
    } elseif ($Length -eq 2 -and (($opcode -band 0xF0) -eq 0x70 -or $opcode -eq 0xEB -or
            ($opcode -ge 0xE0 -and $opcode -le 0xE3))) {
        $field = 1
        $width = 1
    } else {
        # Only a bracketed single address/symbol can qualify. The bytes must
        # independently encode [RIP+disp32]. Leave address-size overrides,
        # moffs, and other unsupported forms unclassified.
        if ($Text -cnotmatch '\[[^\[\]+*\-]+\]' -or $Text -cmatch '\[(?:[er][abcd]x|[er][sb]p|[er][sd]i|r\d+[dwb]?)\]') { return }
        $i = 0
        while ($i -lt $Length -and ($Bytes[$FileOffset + $i] -in 0x66, 0xF2, 0xF3, 0xF0 -or
                ($Bytes[$FileOffset + $i] -band 0xF0) -eq 0x40)) { ++$i }
        if ($i -ge $Length) { return }
        $lead = $Bytes[$FileOffset + $i]
        if ($lead -eq 0x67 -or ($lead -ge 0xA0 -and $lead -le 0xA3)) { return }
        if ($lead -eq 0xC4) { $i += 4 }       # three-byte VEX prefix and opcode
        elseif ($lead -eq 0xC5) { $i += 3 }    # two-byte VEX prefix and opcode
        elseif ($lead -eq 0x0F) {
            $i += 2
            if ($Bytes[$FileOffset + $i - 1] -in 0x38, 0x3A) { ++$i }
        } elseif ($lead -in 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x62) { return }
        else { ++$i }
        if ($i + 5 -gt $Length -or ($Bytes[$FileOffset + $i] -band 0xC7) -ne 0x05) { return }
        $field = $i + 1
        $kind = 'Memory'
    }
    if ($width -eq 4) { $disp = [BitConverter]::ToInt32($Bytes, $FileOffset + $field) }
    else {
        $disp = [int]$Bytes[$FileOffset + $field]
        if ($disp -ge 128) { $disp -= 256 }
    }
    # Preserve every other byte, including any immediate following disp32.
    $form = [BitConverter]::ToString($Bytes, $FileOffset, $field) + '/'
    $rest = $Length - $field - $width
    if ($rest) { $form += [BitConverter]::ToString($Bytes, $FileOffset + $field + $width, $rest) }
    [pscustomobject]@{ Kind = $kind; Target = $Rva + $Length + $disp; Form = $form }
}

function Get-LayoutInstructionBodies {
    param($Image, [string[]]$Lines)
    $starts = [Collections.Generic.SortedSet[long]]::new()
    $selected = @{}
    $imports = [Collections.Generic.HashSet[long]]::new()
    foreach ($s in $Image.Symbols) {
        if (-not $s.IsFunction) {
            if ($s.Name.StartsWith('__imp_', [StringComparison]::Ordinal)) { [void]$imports.Add($s.Rva) }
            continue
        }
        [void]$starts.Add($s.Rva)
        if ($s.Group -cin 'Callback','Refill','Kernel') { $selected[[long]$s.Rva] = $true }
    }
    $entries = @($starts)
    $bodies = @{}
    for ($i = 0; $i -lt $entries.Count; ++$i) {
        $start = $entries[$i]
        if (-not $selected.ContainsKey($start)) { continue }
        foreach ($section in $Image.Pe.Sections) {
            $size = $section.Size
            if ($section.VirtualSize) { $size = [Math]::Min($size, $section.VirtualSize) }
            $end = [long]$section.Rva + $size
            if ($start -lt $section.Rva -or $start -ge $end) { continue }
            if ($i + 1 -lt $entries.Count) { $end = [Math]::Min($end, $entries[$i + 1]) }
            $bodies[$start] = [pscustomobject]@{
                Start = $start; End = $end; FileDelta = [long]$section.Raw - $section.Rva; Code = @{}
                Instructions = @(); Crossings = @(); Missing = $false; IndirectJumps = 0
            }
            break
        }
    }
    $active = $null
    $byteImage = $Image.Pe.Bytes
    $base = [long]$Image.Pe.Base
    $instructionPattern = (Get-DisassemblyGrammar).Instruction
    for ($i = 0; $i -lt $Lines.Count; ++$i) {
        if ($Lines[$i] -cnotmatch '^\s+([0-9A-F]{8,16}): (.*)$') { continue }
        $rva = [Convert]::ToInt64($Matches[1], 16) - $base
        $payload = $Matches[2]
        if ($bodies.ContainsKey($rva)) { $active = $bodies[$rva] }
        if ($null -eq $active) { continue }
        if ($rva -lt $active.Start -or $rva -ge $active.End) { $active = $null; continue }
        if ($payload -cnotmatch '^((?:[0-9A-F]{2} )*[0-9A-F]{2})\s{2,}([a-z][a-z0-9]*(?:\s+.*)?$)') { continue }
        $encoded = $Matches[1]
        $instruction = ($Matches[2].Trim() -creplace '\s+', ' ')
        if ($instruction -cnotmatch $instructionPattern) { continue }
        $mnemonic = $Matches[2]
        if ($mnemonic -cin 'db','dw','dd','dq') { continue }
        # dumpbin wraps instruction bytes after the sixth; this also works for
        # the last instruction before padding, the next symbol or end of file.
        while ($i + 1 -lt $Lines.Count -and $Lines[$i + 1] -cmatch '^\s+((?:[0-9A-F]{2} )*[0-9A-F]{2})\s*$') {
            $encoded += ' ' + $Matches[1]
            ++$i
        }
        $length = [int](($encoded.Length + 1) / 3)
        if ($length -gt 15 -or $rva + $length -gt $active.End) { continue }
        $file = [int]($rva + $active.FileDelta)
        $raw = [BitConverter]::ToString($byteImage, $file, $length)
        if ($raw -cne $encoded.Replace(' ', '-')) { throw 'Disassembly bytes do not match the supplied PE image.' }
        $active.Code[$rva] = [pscustomobject]@{
            Rva = $rva; Offset = $rva - $active.Start; Length = $length; Bytes = $raw; Text = $instruction; Mnemonic = $mnemonic
            Reference = Get-LayoutInstructionReference $byteImage $file $length $rva $instruction
        }
    }
    foreach ($body in $bodies.Values) {
        # Walk from the actual entry. A switch table after jmp rax can look
        # like instructions even in a symbolized listing. Do not guess targets
        # or walk its fall-through. Tail transfers stay outside this body.
        $todo = [Collections.Generic.Stack[long]]::new()
        $seen = [Collections.Generic.HashSet[long]]::new()
        $code = [Collections.Generic.List[object]]::new()
        $todo.Push($body.Start)
        while ($todo.Count) {
            $ip = $todo.Pop()
            if (-not $seen.Add($ip)) { continue }
            $ins = $body.Code[$ip]
            if ($null -eq $ins) { $body.Missing = $true; continue }
            $code.Add($ins)
            if ($ins.Mnemonic -cmatch '^(?:j\w+|loop\w*)$') {
                if ($null -ne $ins.Reference -and $ins.Reference.Kind -ceq 'Branch') {
                    $target = $ins.Reference.Target
                    if ($target -ge $body.Start -and $target -lt $body.End) { $todo.Push($target) }
                } elseif ($null -eq $ins.Reference -or -not $imports.Contains($ins.Reference.Target)) {
                    ++$body.IndirectJumps
                } # A RIP-relative tail jump via a MAP import slot leaves the image.
            }
            if ($ins.Mnemonic -cmatch '^(?:jmp|ret\w*|iret\w*|ud2|hlt|int3?)$') { continue }
            $next = $ip + $ins.Length
            if ($next -lt $body.End) { $todo.Push($next) }
        }
        $body.Instructions = @($code | Sort-Object Rva)
        $body.Crossings = @($body.Instructions | Where-Object { ($_.Rva % 64) + $_.Length -gt 64 } |
            Select-Object Offset, Length, Text)
    }
    $bodies
}

function Compare-LayoutFunctionBody {
    param($Left, $Right, $PairedTargets)
    if ($null -eq $Left -or $null -eq $Right -or $Left.Missing -or $Right.Missing) { return 'Unknown' }
    if ($Left.Instructions.Count -ne $Right.Instructions.Count) {
        if ($Left.IndirectJumps -or $Right.IndirectJumps) { return 'Unknown' }
        return 'Changed'
    }
    $rawEqual = $true
    $unknown = $false
    for ($i = 0; $i -lt $Left.Instructions.Count; ++$i) {
        $a = $Left.Instructions[$i]
        $b = $Right.Instructions[$i]
        # Unexecuted padding or a different ordering of basic blocks can move
        # instructions inside one function. Offsets alone cannot pair them.
        if ($a.Offset -ne $b.Offset) { return 'Unknown' }
        if ($a.Length -ne $b.Length) { return 'Changed' }
        $same = $a.Bytes -ceq $b.Bytes
        if (-not $same) { $rawEqual = $false }
        $x = $a.Reference
        $y = $b.Reference
        if ($null -ne $x -and $null -ne $y) {
            if ($x.Kind -cne $y.Kind -or $x.Form -cne $y.Form) { return 'Changed' }
            if ($x.Kind -ceq 'Branch' -and $Left.Code.ContainsKey($x.Target) -and $Right.Code.ContainsKey($y.Target)) {
                if ($x.Target - $Left.Start -ne $y.Target - $Right.Start) { return 'Changed' }
            } elseif ($PairedTargets.ContainsKey($x.Target)) {
                if ($PairedTargets[$x.Target] -ne $y.Target) { return 'Changed' }
            } else { $unknown = $true }
        } elseif (-not $same) {
            # Masking a number in dumpbin's text could hide an immediate, an
            # offset in a structure, or an unrelated call. Different opcodes or
            # plain scalar literals are observable; other encodings stay open.
            # Restrict scalar recognition to register operands and byte-sized
            # literal values; larger numeric values can be image-relative addresses.
            $scalar = '^(?:mov|cmp|test|add|sub|and|or|xor|shl|shr|sar|rol|ror) \w+,(?:[0-9]|0?[0-9A-F]{1,2}h)$'
            if ($a.Mnemonic -cne $b.Mnemonic -or
                ($a.Text -cmatch $scalar -and $b.Text -cmatch $scalar -and $a.Text -cne $b.Text)) { return 'Changed' }
            $unknown = $true
        }
    }
    # RawEqual describes encoded bytes only. Relocated is stronger: every
    # differing field in the inspected instructions had a proved counterpart.
    if ($rawEqual) { return 'RawEqual' }
    if ($unknown -or $Left.IndirectJumps -or $Right.IndirectJumps) { return 'Unknown' }
    'Relocated'
}

function Compare-LayoutInstructions {
    param($Baseline, $Candidate, $SymbolComparison, [string]$Dumpbin)
    $result = [pscustomobject]@{
        Status = 'Unavailable'; Reason = ''; Scope = 'MAP callback, refill and conversion function entries, checked against PE bytes. Counts cover fall-through and direct branches within each entry. Calls and unresolved indirect jump targets are not followed. RawEqual describes observed bytes. PDB labels are optional.'
        BaselineCrossings = $null; CandidateCrossings = $null; Details = @()
    }
    if (-not $Dumpbin) { $result.Reason = 'dumpbin unavailable'; return $result }
    $leftLines = [string[]]@(& $Dumpbin /nologo /disasm $Baseline.ExePath)
    if ($LASTEXITCODE -ne 0) { throw 'dumpbin could not disassemble the baseline executable.' }
    if ($Baseline.Sha256 -ceq $Candidate.Sha256) { $rightLines = $leftLines }
    else {
        $rightLines = [string[]]@(& $Dumpbin /nologo /disasm $Candidate.ExePath)
        if ($LASTEXITCODE -ne 0) { throw 'dumpbin could not disassemble the candidate executable.' }
    }
    $left = Get-LayoutInstructionBodies $Baseline $leftLines
    $right = Get-LayoutInstructionBodies $Candidate $rightLines
    # MAP aliases can fold, split, or disappear. Require an unambiguous
    # counterpart in both directions before naming the referent.
    $targets = @{}
    $sources = @{}
    $ambiguousA = [Collections.Generic.HashSet[long]]::new()
    $ambiguousB = [Collections.Generic.HashSet[long]]::new()
    foreach ($row in $SymbolComparison.Items) {
        if ($row.Status -cne 'Matched') {
            if ($null -ne $row.BaselineRva) { [void]$ambiguousA.Add($row.BaselineRva) }
            if ($null -ne $row.CandidateRva) { [void]$ambiguousB.Add($row.CandidateRva) }
            continue
        }
        $a = [long]$row.BaselineRva
        $b = [long]$row.CandidateRva
        if ($targets.ContainsKey($a) -and $targets[$a] -ne $b) { [void]$ambiguousA.Add($a) }
        if ($sources.ContainsKey($b) -and $sources[$b] -ne $a) { [void]$ambiguousB.Add($b) }
        $targets[$a] = $b
        $sources[$b] = $a
    }
    foreach ($a in @($targets.Keys)) {
        if ($ambiguousA.Contains($a) -or $ambiguousB.Contains($targets[$a])) { $targets.Remove($a) }
    }
    $details = [Collections.Generic.List[object]]::new()
    $partial = $false
    foreach ($row in $SymbolComparison.Items) {
        if ($row.Group -cnotin 'Callback','Refill','Kernel') { continue }
        $a = if ($null -ne $row.BaselineRva) { $left[[long]$row.BaselineRva] }
        $b = if ($null -ne $row.CandidateRva) { $right[[long]$row.CandidateRva] }
        $relation = 'Unknown'
        if ($row.Status -ceq 'Matched') { $relation = Compare-LayoutFunctionBody $a $b $targets }
        if ($relation -ceq 'Unknown' -or ($null -ne $a -and $a.IndirectJumps) -or
            ($null -ne $b -and $b.IndirectJumps)) { $partial = $true }
        $record = [ordered]@{ Group = $row.Group; Name = $row.Name; Object = $row.Object; Match = $row.Status; Comparison = $relation }
        foreach ($side in 'Baseline','Candidate') {
            $body = if ($side -ceq 'Baseline') { $a } else { $b }
            $record[$side] = if ($null -ne $body) {
                [pscustomobject]@{ Rva = $body.Start; Instructions = $body.Instructions.Count
                    MissingInstruction = $body.Missing; IndirectJumps = $body.IndirectJumps; Crossings = $body.Crossings }
            }
        }
        $details.Add([pscustomobject]$record)
    }
    $result.Details = $details.ToArray()
    foreach ($side in 'Baseline','Candidate') {
        $bodies = if ($side -ceq 'Baseline') { $left } else { $right }
        $covered = $false
        $sum = 0
        foreach ($body in $bodies.Values) {
            if ($body.Instructions.Count) { $covered = $true; $sum += $body.Crossings.Count }
        }
        if ($covered) { $result.("${side}Crossings") = $sum }
    }
    if ($null -eq $result.BaselineCrossings -and $null -eq $result.CandidateCrossings) {
        $result.Reason = 'No selected MAP entry could be decoded.'
    } elseif ($partial) {
        $result.Status = 'Partial'
        $result.Reason = 'Some entries, indirect successors or reference identities could not be paired.'
    } else { $result.Status = 'Compared' }
    $result
}
