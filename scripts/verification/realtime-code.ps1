# Audits the machine code the ASIO callbacks can reach in a Release disassembly
# (dumpbin /disasm:nobytes) against the callback rules (docs/architecture.md,
# real-time boundary) and returns the violations, the summary and the walk. With
# the PDB beside the executable, dumpbin labels every function and prints a direct
# target as the same decorated name or as an address. The walk starts at the
# callback entry points, follows direct calls and jumps through product code,
# whatever was inlined, and fails on forbidden constructs, on functions of the
# GUI-side layers and on lines it cannot read rather than on a particular code
# shape. Spin waits and semantic changes stay invisible to it.
function Test-RealTimeListing {
    param([string[]]$Lines, [UInt64]$ImageBase)
    # dumpbin prints mnemonics and registers in lower case and addresses and data
    # bytes in upper case, so every match on its output is case-sensitive.
    $failures = [Collections.Generic.List[string]]::new()
    $audit = [pscustomobject]@{
        Failures = $failures; Summary = [Collections.Generic.List[string]]::new()
        Functions = $null; Roots = @(); Walked = [Collections.Generic.List[int]]::new()
    }
    # Functions in address order. Instructions are indented; the other lines up to
    # the next instruction form the label. Captured output does not wrap labels,
    # so joining lines is only defensive.
    $functions = [Collections.Generic.List[object]]::new()
    $byName = [hashtable]::new([StringComparer]::Ordinal)
    $byAddress = @{}
    $label = ''
    $current = $null
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -cmatch '^\s+([0-9A-F]{8,16}): ') {
            $address = [Convert]::ToUInt64($Matches[1], 16)
            if ($label.EndsWith(':')) {
                $current = [pscustomobject]@{ Name = $label.TrimEnd(':'); First = $i; Last = $i; Start = $address; End = $address }
                # Only undecorated internal-linkage labels repeat today; dumpbin
                # prints calls to those as addresses. Keeping every index is defensive.
                if ($byName.ContainsKey($current.Name)) {
                    $byName[$current.Name] += $functions.Count
                } else {
                    $byName[$current.Name] = @($functions.Count)
                }
                $byAddress[$address] = $functions.Count
                $functions.Add($current)
            }
            $label = ''
            if ($current) {
                $current.Last = $i
                $current.End = $address
            }
        } elseif ($lines[$i] -cmatch '^\S') {
            $label += $lines[$i]
        } else {
            $label = ''
        }
    }
    $audit.Functions = $functions
    if (-not $functions.Count) {
        $failures.Add('no function labels in the disassembly; is the PDB beside the executable?')
        return $audit
    }
    $product = '@wavplay@@|^`?wavplay::'
    # The GUI, settings and playlist layers, optimized for size (build/WavplaySources.props), never
    # run in a callback. The rule reads function labels only: code inlined into a callback function
    # and shared inline functions of other namespaces generated from those layers' IL carry no such
    # label (docs/architecture.md, optimization policy).
    $guiSide = '@(?:ui|settings|playlist)@wavplay@@|^`?wavplay::(?:ui|settings|playlist)::'
    $register = '^r(?:[abcd]x|[sd]i|[sb]p|\d{1,2})$'
    $switchShape = '^lea (r\w+),\[([0-9A-F]+)h\]\nmov (\w+),dword ptr \[\1\+r\w+\*4\+([0-9A-F]+)h\]\nadd (r\w+),\1\njmp \5$'
    $queue = [Collections.Generic.Queue[int]]::new()
    $seen = @{}
    # The first three are the non-null entries of CallbackState::table in
    # src/wavplay/playback/segment_renderer.cpp; a callback added there is added here.
    # is_streaming is no table entry, and the driver never calls it: the renderer
    # calls it under switch_buffer. It is an extra root, found only when emitted out
    # of line, so its absence is no failure.
    foreach ($entry in 'switch_buffer', 'sample_rate_changed', 'asio_message', 'is_streaming') {
        $found = @(0..($functions.Count - 1) | Where-Object {
            $functions[$_].Name -cmatch $product -and $functions[$_].Name -cmatch "^\?$entry@|::$entry$" })
        if (-not $found -and $entry -ne 'is_streaming') {
            $failures.Add("callback root $entry not found in the disassembly")
        }
        $found | ForEach-Object { $seen[$_] = $true; $queue.Enqueue($_); $audit.Walked.Add($_) }
    }
    if ($failures.Count) {
        return $audit
    }
    $audit.Roots = $audit.Walked.ToArray()

    $importCalls = @{}
    $switches = [Collections.Generic.List[string]]::new()
    $instructions = 0
    $locked = 0
    $pages = @{}
    while ($queue.Count) {
        $function = $functions[$queue.Dequeue()]
        if ($function.Name -cmatch $guiSide) {
            $failures.Add("GUI-side function $($function.Name) in the real-time call graph")
        }
        $tableStart = [UInt64]::MaxValue
        $recent = @('', '', '', '') # The last four instructions, the current one last.
        for ($i = $function.First; $i -le $function.Last; $i++) {
            if ($lines[$i] -cnotmatch '^\s+([0-9A-F]{8,16}): (.*)$') {
                continue
            }
            $address = [Convert]::ToUInt64($Matches[1], 16)
            if ($address -ge $tableStart) {
                break # The switch tables: data after the code.
            }
            $text = $Matches[2]
            $at = "$($function.Name) at $($Matches[1]): $($text -replace '\s+', ' ')"
            # int3 padding, printed as data bytes: only CC up to the gap before the
            # character column.
            if ($text -cmatch '^CC(?: CC)*(?:\s{2}|$)') {
                continue
            }
            if ($text -cnotmatch '^(?:(lock|rep\w*|notrack|bnd) )?([a-z]\w*)\s*(.*?)\s*$') {
                $failures.Add("unreadable line in $at")
                continue
            }
            $prefix, $mnemonic, $operands = $Matches[1], $Matches[2], $Matches[3]
            $recent = @($recent[1], $recent[2], $recent[3], "$mnemonic $operands")
            ++$instructions
            $pages[$address -shr 12] = $true
            if ($prefix -ceq 'lock' -or ($mnemonic -ceq 'xchg' -and $operands -cmatch '\[')) {
                ++$locked
            }
            # Integer-only samples, in both legacy and VEX encodings. FMA and
            # FP conversion stay forbidden even though the startup gate requires them.
            if ($mnemonic -cmatch '^v?(?:add|sub|mul|div|sqrt|min|max|rcp|rsqrt|hadd|hsub|dp|round)(?:ps|pd|ss|sd)$|^v?cvt|^v?u?comis[sd]$|^v?f') {
                $failures.Add("floating-point arithmetic in $at")
            }
            # The product has one AVX2 path; the baseline startup gate has already
            # checked its scalar extensions and enabled XMM/YMM state. Enumerate
            # permitted VEX integer/move instructions, so AVX-512 forms using only
            # ymm registers (such as vmovdqu64) cannot slip through a width check.
            $vexInteger = '^v(?:zero(?:upper|all)|mov(?:d|q|dqa|dqu|aps|ups|apd|upd|ss|sd|hlps|lhps)|(?:and|andn|or|xor)p[sd]|p(?:and|andn|or|xor|add[bwdq]|sub[bwdq]|sll[wdq]|srl[wdq]|sra[wd]|sllv[dq]|srlv[dq]|sravd|s[lr]ldq|shuf[bwd]|shuf[lh]w|unpck[lh](?:bw|wd|dq|qdq)|cmp(?:eq|gt)[bwdq]|blend(?:w|d|vb)|pack(?:ss|us)(?:wb|dw)|mov(?:sx|zx)(?:bw|bd|bq|wd|wq|dq)|broadcast[bwdq]|min(?:s|u)[bwd]|max(?:s|u)[bwd])|perm(?:2i128|d|q)|(?:extract|insert)i128|(?:extract|insert)f128)$'
            if ($mnemonic -cmatch '^k' -or ($mnemonic -cmatch '^v' -and $mnemonic -cnotmatch $vexInteger) -or
                $operands -cmatch '\bzmm\d|\bk[0-7]\b|__isa_available|__isa_enabled|__isa_inverted|__favor') {
                $failures.Add("instruction outside the AVX2 integer contract or CPU dispatch in $at")
            }
            if ($operands -cmatch '__chkstk|__security_|__GSHandlerCheck|_CxxThrowException|__guard_|\?\?[23]@|\?\?_[UV]@') {
                $failures.Add("stack probe, GS cookie, CFG, exception or heap call in $at")
                continue
            }
            if ($mnemonic -cne 'call' -and $mnemonic -cnotlike 'j*') {
                continue
            }
            if ($operands -cmatch '^qword ptr \[__imp_(\w+)\]$') {
                if ($Matches[1] -cne 'SetEvent') {
                    $failures.Add("imported call in $at")
                }
                $importCalls[$Matches[1]] = 1 + $importCalls[$Matches[1]]
                continue
            }
            # The MSVC switch, four instructions in a row so that no other write
            # intervenes: lea B,[image base]; mov E,dword ptr [B+I*4+D] reads an
            # entry of the table at image base + D after the jump; add R,B with R
            # the 64-bit E, not B; jmp R. Any other register jump is indirect.
            if ($mnemonic -ceq 'jmp' -and ($recent -join "`n") -cmatch $switchShape) {
                $table = $ImageBase + [Convert]::ToUInt64($Matches[4], 16)
                if ([Convert]::ToUInt64($Matches[2], 16) -eq $ImageBase -and $Matches[5] -cne $Matches[1] -and
                    $Matches[5] -ceq ($Matches[3] -creplace '^e(\w\w)$', 'r$1' -creplace '^(r\d+)d$', '$1') -and
                    $table -gt $address -and $table -le $function.End) {
                    if ($table -lt $tableStart) {
                        $tableStart = $table
                    }
                    $switches.Add($at)
                    continue
                }
            }
            if ($operands -cmatch $register -or $operands -cmatch '\[') {
                $failures.Add("indirect call or jump in $at")
                continue
            }
            # Branches inside the function, and callees that dumpbin prints as
            # addresses because they have internal linkage (anonymous namespace).
            if ($operands -cmatch '^[0-9A-F]{8,16}$') {
                $target = [Convert]::ToUInt64($operands, 16)
                if ($target -ge $function.Start -and $target -le $function.End) {
                    continue
                }
                $targets = @($byAddress[$target] | Where-Object { $null -ne $_ })
            } elseif ($byName.ContainsKey($operands)) {
                $targets = $byName[$operands]
            } else {
                $targets = @()
            }
            if (-not $targets.Count) {
                $failures.Add("unknown branch target in $at")
            }
            foreach ($k in $targets) {
                $name = $functions[$k].Name
                # No CRT routine is allowed, memcpy and memset included: the block
                # copy and zero are core::copy_block/zero_block, inlined.
                if ($name -cnotmatch $product) {
                    $failures.Add("call or jump to $name in $at")
                } elseif (-not $seen.ContainsKey($k)) {
                    $seen[$k] = $true
                    $queue.Enqueue($k)
                    $audit.Walked.Add($k)
                }
            }
        }
    }

    # The before/after metrics of real-time code changes, all of the walked product code.
    $names = @($seen.Keys | Sort-Object | ForEach-Object {
        if ($functions[$_].Name -cmatch '^\?([^@]+)@([^@]+)@') { "$($Matches[2])::$($Matches[1])" } else { ($functions[$_].Name -split '::')[-1] }
    })
    $pageList = @($pages.Keys | Sort-Object | ForEach-Object { '{0:X}' -f ($_ -shl 12) })
    $audit.Summary.Add(('Real-time audit: {0} product functions with {1} instructions, {2} locked; {3} code pages ({4})' -f
        $names.Count, $instructions, $locked, $pageList.Count, ($pageList -join ' ')))
    $audit.Summary.Add("Real-time audit imported calls: $(@($importCalls.Keys | Sort-Object | ForEach-Object { "$_ x$($importCalls[$_])" }) -join ', ')")
    $audit.Summary.Add("Real-time audit functions: $($names -join ', ')")
    $switches | ForEach-Object { $audit.Summary.Add("Real-time audit switch table: $_") }
    $audit
}

# Release defines NDEBUG (build/Common.props), so product asserts check only in
# Debug. Without it an assert would link the CRT's _wassert, with its message box
# and console output, into the shipped executable. Returns the lines of a
# disassembly that name it, as a label or a call target.
function Test-CrtAssert {
    param([string[]]$Lines)
    @($Lines -cmatch '\b_wassert\b')
}

# The product writes no CRT stdio: DriverSession reports through WriteFile and
# OutputDebugStringA (docs/asio-contract.md). The conversion kernels copy with
# core::copy_block, not the CRT's memcpy and its runtime dispatch
# (docs/architecture.md). Returns the lines of a disassembly that name a CRT
# stdio output routine, as a label or an operand, and the direct calls and jumps
# to memcpy, memmove or memset inside wavplay::audio functions. Audio counts those
# functions and First is the index of the first one's first instruction, so a
# listing without them is never taken as clean.
function Test-CrtRoutines {
    param([string[]]$Lines)
    $stdio = '\b(?:fputs|fputws|fwrite|fputc|fputwc|putc|putwc|puts|_putws|__stdio_common_vfw?printf\w*)\b'
    # The function's own namespace, not a parameter type that names it.
    $audio = '^\?(?:[^@?]+@|\?A0x[0-9a-f]+@)+audio@wavplay@@|^`?wavplay::audio::'
    $result = [pscustomobject]@{
        Stdio = @($Lines -cmatch $stdio); Copies = [Collections.Generic.List[string]]::new(); Audio = 0; First = -1
    }
    $function = ''
    $inAudio = $false
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -cmatch '^(\S.*):$') {
            $function = $Matches[1]
            $inAudio = $function -cmatch $audio
            if ($inAudio) {
                ++$result.Audio
            }
        } elseif ($inAudio -and $Lines[$i] -cmatch '^\s+[0-9A-F]{8,16}: (.*)$') {
            if ($result.First -lt 0) {
                $result.First = $i
            }
            if ($Matches[1] -cmatch '^(?:call|j\w+)\s+(?:memcpy|memmove|memset)$') {
                $result.Copies.Add("${function}: $($Lines[$i].Trim())")
            }
        }
    }
    $result
}

# Audits the Release executable's callback machine code, then checks that the audit
# rejects each violation introduced into a copy of the real disassembly. Most copies
# insert instructions right after the first one of the switch_buffer root, at its
# address, wherever the walk found that root, so the check needs no particular code
# shape from the toolset. The same listing is then checked for the CRT assert.
function Assert-RealTimeCode {
    param([string]$Executable, [string]$Dumpbin)
    $imageBase = @(Get-CheckedOutput $Dumpbin @('/nologo', '/headers', $Executable) | ForEach-Object {
        if ($_ -cmatch '^\s+([0-9A-F]+) image base ') { [Convert]::ToUInt64($Matches[1], 16) }
    })
    if ($imageBase.Count -ne 1) {
        throw "Real-time audit: cannot read the image base of $Executable."
    }
    $base = $imageBase[0]
    $lines = [string[]]@(Get-CheckedOutput $Dumpbin @('/nologo', '/disasm:nobytes', $Executable))
    $audit = Test-RealTimeListing $lines $base
    $audit.Summary | ForEach-Object { Write-Output $_ }
    if ($audit.Failures.Count) {
        throw "Real-time audit of $Executable failed against the callback rules in docs/architecture.md:`n$($audit.Failures -join "`n")"
    }

    $switchBuffer = $audit.Functions[$audit.Roots[0]] # switch_buffer, the first root searched.
    $null = $lines[$switchBuffer.First] -cmatch '^\s+([0-9A-F]{8,16}): '
    $address = $Matches[1]
    $insert = {
        $copy = [Collections.Generic.List[string]]::new($lines)
        $copy.InsertRange($switchBuffer.First + 1, [string[]]@($args | ForEach-Object { "  ${address}: $_" }))
        , $copy.ToArray()
    }
    # Renames a function's label and every use of its name.
    $rename = {
        param([string]$Name, [string]$NewName)
        , @($lines -creplace [regex]::Escape($Name), $NewName.Replace('$', '$$'))
    }
    $toGuiSide = { param([string]$Name) $Name -creplace '@\w+@wavplay@@', '@ui@wavplay@@' -creplace '^(`?wavplay::)\w+::', '$1ui::' }
    $code = 'stack probe, GS cookie, CFG, exception or heap call in '
    $isa = 'instruction outside the AVX2 integer contract or CPU dispatch in '
    $variants = [Collections.Generic.List[object]]@(
        @{ Case = 'an imported call'; Expect = 'imported call in '; Lines = & $insert 'call        qword ptr [__imp_OutputDebugStringW]' }
        @{ Case = 'an indirect call'; Expect = 'indirect call or jump in '; Lines = & $insert 'call        rax' }
        @{ Case = 'a register jump through a table before it'; Expect = 'indirect call or jump in '
            Lines = & $insert ('lea         rcx,[{0:X}h]' -f $base) ('mov         eax,dword ptr [rcx+rdx*4+{0:X}h]' -f [UInt64]($switchBuffer.Start - $base)) 'add         rax,rcx' 'jmp         rax' }
        @{ Case = 'floating-point arithmetic'; Expect = 'floating-point arithmetic in '; Lines = & $insert 'addsd       xmm0,xmm1' }
        @{ Case = 'a stack probe'; Expect = $code; Lines = & $insert 'call        __chkstk' }
        @{ Case = 'operator new'; Expect = $code; Lines = & $insert 'call        ??2@YAPEAX_K@Z' }
        @{ Case = 'a zmm operand'; Expect = $isa; Lines = & $insert 'vmovdqu64   zmm0,zmmword ptr [rcx]' }
        @{ Case = 'AVX-512 with a ymm operand'; Expect = $isa; Lines = & $insert 'vmovdqu64   ymm0,ymmword ptr [rcx]' }
        @{ Case = 'VEX floating-point arithmetic'; Expect = 'floating-point arithmetic in '; Lines = & $insert 'vaddsd      xmm0,xmm1,xmm2' }
        @{ Case = 'a CPU-level read'; Expect = $isa; Lines = & $insert 'cmp         dword ptr [__isa_available],2' }
        @{ Case = 'a CRT call'; Expect = 'call or jump to memcpy in '; Lines = & $insert 'call        memcpy' }
        @{ Case = 'an unknown callee'; Expect = 'unknown branch target in '; Lines = & $insert 'call        ?missing@@YAXXZ' }
        @{ Case = 'a data line'; Expect = 'unreadable line in '; Lines = & $insert 'F0 0F B1 0D' }
        @{ Case = 'a renamed root'; Expect = "callback root switch_buffer not found"
            Lines = & $rename $switchBuffer.Name ($switchBuffer.Name -creplace 'switch_buffer', 'renamed_buffer') }
        @{ Case = 'a GUI-side root'; Expect = 'GUI-side function '; Lines = & $rename $switchBuffer.Name (& $toGuiSide $switchBuffer.Name) }
        @{ Case = 'no labels'; Expect = 'no function labels'; Lines = @($lines -cnotmatch '^\S.*:$') }
    )
    # A callee of the walk, when the toolset left one out of line.
    $callee = @($audit.Walked | Where-Object { $_ -notin $audit.Roots } | ForEach-Object { $audit.Functions[$_].Name })
    if ($callee) {
        $variants.Add(@{ Case = "a GUI-side callee ($($callee[0]))"; Expect = 'GUI-side function '
            Lines = & $rename $callee[0] (& $toGuiSide $callee[0]) })
    }
    foreach ($variant in $variants) {
        Assert-Rejected 'Real-time audit' $variant.Case (Test-RealTimeListing $variant.Lines $base).Failures $variant.Expect
    }
    Write-Output "Real-time audit self-check: $($variants.Count) altered listings rejected"

    $asserts = @(Test-CrtAssert $lines)
    if ($asserts.Count) {
        throw "Release links the CRT assert of ${Executable}; build/Common.props must define NDEBUG for Release, and no product source may undefine it:`n$(@($asserts | Select-Object -First 3) -join "`n")"
    }
    if (-not @(Test-CrtAssert (& $insert 'call        _wassert')).Count) {
        throw 'CRT assert audit self-check: an inserted call of _wassert was not rejected.'
    }
    Write-Output 'CRT assert audit: the Release listing names no _wassert; an inserted call was rejected'

    $routines = Test-CrtRoutines $lines
    if (-not $routines.Audio) {
        throw "CRT routine audit: no wavplay::audio function in the disassembly of ${Executable}; is the PDB beside it?"
    }
    if ($routines.Stdio.Count) {
        throw "Release links CRT stdio output into ${Executable}; report through WriteFile and OutputDebugStringA (docs/asio-contract.md):`n$(@($routines.Stdio | Select-Object -First 3) -join "`n")"
    }
    if ($routines.Copies.Count) {
        throw "A conversion kernel of ${Executable} calls a CRT block routine; copy with core::copy_block (docs/architecture.md):`n$(@($routines.Copies | Select-Object -First 3) -join "`n")"
    }
    if (-not @((Test-CrtRoutines (& $insert 'call        fputs')).Stdio).Count) {
        throw 'CRT routine audit self-check: an inserted call of fputs was not rejected.'
    }
    $null = $lines[$routines.First] -cmatch '^\s+([0-9A-F]{8,16}): '
    $kernel = [Collections.Generic.List[string]]::new($lines)
    $kernel.Insert($routines.First + 1, "  $($Matches[1]): call        memcpy")
    if (-not (Test-CrtRoutines $kernel.ToArray()).Copies.Count) {
        throw 'CRT routine audit self-check: a call of memcpy inserted into a wavplay::audio function was not rejected.'
    }
    Write-Output "CRT routine audit: no stdio output, and no memcpy, memmove or memset call in $($routines.Audio) wavplay::audio functions; inserted calls were rejected"
    Assert-StartupCode $Executable $lines $audit.Functions
}
