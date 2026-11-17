# Loaded by verify.ps1. The isolated wWinMain is not the first code executed:
# the loader calls TLS callbacks and the static CRT walks initialization tables.
# Resolve those tables in the final PE, using its linker map for object provenance.
# Prebuilt Microsoft CRT objects and Windows imports are the trust boundary;
# their own CPU dispatch remains intact. Every product function before the gate,
# and every product static destructor, must stay within the SSE4.2 baseline.

function Read-StartupImage {
    param([string]$Executable)
    $bytes = [IO.File]::ReadAllBytes($Executable)
    $map = [IO.Path]::ChangeExtension($Executable, '.map')
    $symbols = @{}
    $names = @{}
    foreach ($line in [IO.File]::ReadAllLines($map)) {
        if ($line -cmatch '^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$') {
            $name = $Matches[1]
            $address = [Convert]::ToUInt64($Matches[2], 16)
            $symbol = [pscustomobject]@{ Name = $name; Address = $address; Object = $Matches[3] }
            if (-not $symbols.ContainsKey($address)) { $symbols[$address] = @() }
            $symbols[$address] += $symbol
            $names[$name] = $symbol
        }
    }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'Startup audit requires an x64 PE image.' }
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) { throw 'Startup audit requires PE32+.' }
    $base = [BitConverter]::ToUInt64($bytes, $optional + 24)
    $headers = [BitConverter]::ToUInt32($bytes, $optional + 60)
    $sectionStart = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
    $sections = @(for ($i = 0; $i -lt [BitConverter]::ToUInt16($bytes, $pe + 6); ++$i) {
        $offset = $sectionStart + 40 * $i
        [pscustomobject]@{
            Rva = [BitConverter]::ToUInt32($bytes, $offset + 12)
            VirtualSize = [BitConverter]::ToUInt32($bytes, $offset + 8)
            Size = [BitConverter]::ToUInt32($bytes, $offset + 16)
            Raw = [BitConverter]::ToUInt32($bytes, $offset + 20)
        }
    })
    $readPointer = {
        param([UInt64]$Address)
        if ($Address -lt $base) { throw "Startup pointer address below image: $Address" }
        $rva = $Address - $base
        $raw = if ($rva + 8 -le $headers) { $rva } else {
            $section = @($sections | Where-Object { $rva -ge $_.Rva -and $rva + 8 -le [UInt64]$_.Rva + [Math]::Max($_.Size, $_.VirtualSize) })
            if ($section.Count -ne 1) { throw ('Startup pointer is not backed by one PE section: {0:X}' -f $Address) }
            $within = $rva - $section[0].Rva
            # PE virtual tails (.bss, including unused dynamic TLS pointers) are
            # initialized to zero by the loader and have no bytes in the file.
            if ($within -ge $section[0].Size) { return [UInt64]0 }
            if ($within + 8 -gt $section[0].Size) { throw 'Startup pointer crosses the file-backed/zero-fill boundary.' }
            [UInt64]$section[0].Raw + $within
        }
        if ($raw + 8 -gt $bytes.Length) { throw 'Startup pointer extends past the PE file.' }
        [BitConverter]::ToUInt64($bytes, [int]$raw)
    }
    $roots = [Collections.Generic.List[UInt64]]::new()
    $roots.Add($base + [BitConverter]::ToUInt32($bytes, $optional + 16))
    # Include C/C++ initialization and pre-termination/termination tables.
    foreach ($kind in 'xi', 'xc', 'xp', 'xt') {
        $first = "__${kind}_a"; $last = "__${kind}_z"
        if (-not $names.ContainsKey($first) -or -not $names.ContainsKey($last)) { throw "Startup table $kind is missing from $map" }
        $start = $names[$first].Address; $end = $names[$last].Address
        if ($end -lt $start -or ($end - $start) % 8 -ne 0 -or $end - $start -gt 65536) { throw "Invalid CRT table $kind" }
        if ((& $readPointer $start) -ne 0 -or (& $readPointer $end) -ne 0) { throw "Non-null CRT table sentinel $kind" }
        for ($address = $start + 8; $address -lt $end; $address += 8) {
            $target = & $readPointer $address
            if ($target) { $roots.Add($target) }
        }
    }
    # The loader's IMAGE_TLS_DIRECTORY64 can run code before the PE entry point.
    $tlsRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 9 * 8)
    if ($tlsRva) {
        $callbacks = & $readPointer ($base + $tlsRva + 24)
        if ($callbacks) {
            $ended = $false
            for ($i = 0; $i -lt 1024; ++$i) {
                $target = & $readPointer ($callbacks + 8 * $i)
                if (-not $target) { $ended = $true; break }
                $roots.Add($target)
            }
            if (-not $ended) { throw 'Unterminated PE TLS callback table.' }
        }
    }
    # The static CRT invokes dynamic TLS initializers separately for the first
    # thread, and registers their destructors before invoking wWinMain.
    foreach ($name in '__dyn_tls_init_callback', '__dyn_tls_dtor_callback') {
        if ($names.ContainsKey($name)) {
            $target = & $readPointer $names[$name].Address
            if ($target) { $roots.Add($target) }
        }
    }
    if (-not $names.ContainsKey('wWinMain')) { throw 'Baseline wWinMain is missing from the map.' }
    $roots.Add($names['wWinMain'].Address)
    [pscustomobject]@{ Symbols = $symbols; Names = $names; Roots = $roots.ToArray(); Base = $base }
}

function Test-StartupListing {
    param([string[]]$Lines, $Functions, $Image)
    $failures = [Collections.Generic.List[string]]::new()
    $walked = [Collections.Generic.List[object]]::new()
    $byAddress = @{}; $byName = @{}
    foreach ($function in $Functions) { $byAddress[$function.Start] = $function; $byName[$function.Name] = $function }
    $trustedCrt = '^(?:libcmt|libcpmt|libucrt|libvcruntime):'
    $queue = [Collections.Generic.Queue[UInt64]]::new()
    foreach ($root in $Image.Roots) { $queue.Enqueue($root) }
    # Compiler-generated static destructors need not appear in a PE table: their
    # initializers register their addresses with atexit. Audit them independently.
    foreach ($function in $Functions) {
        if ($function.Name -cmatch "dynamic atexit destructor|^\?\?__F") { $queue.Enqueue($function.Start) }
    }
    $seen = @{}
    $bodyCalls = 0
    while ($queue.Count) {
        $address = $queue.Dequeue()
        if ($seen.ContainsKey($address)) { continue }
        $seen[$address] = $true
        $origin = @($Image.Symbols[$address] | Where-Object { $null -ne $_ })
        if (-not $origin.Count) { $failures.Add(('startup target absent from map: {0:X}' -f $address)); continue }
        # Do not trust a folded product alias just because another alias is CRT.
        if (-not @($origin | Where-Object { $_.Object -notmatch $trustedCrt }).Count) { continue }
        $function = $byAddress[$address]
        if (-not $function) { $failures.Add(('startup target absent from disassembly: {0:X}' -f $address)); continue }
        $walked.Add($function)
        for ($i = $function.First; $i -le $function.Last; ++$i) {
            if ($Lines[$i] -cnotmatch '^\s+([0-9A-F]{8,16}): (.*)$') { continue }
            $text = $Matches[2]
            $at = "$($function.Name): $($text -replace '\s+', ' ')"
            if ($text -cmatch '^CC(?: CC)*(?:\s{2}|$)') { continue }
            if ($text -cnotmatch '^(?:(lock|rep\w*|notrack|bnd) )?([a-z]\w*)\s*(.*?)\s*$') {
                $failures.Add("unreadable startup instruction in $at"); continue
            }
            $mnemonic = $Matches[2]; $operand = $Matches[3]
            if ($mnemonic -cmatch '^(?:v|k|(?:andn|bextr|blsi|blsmsk|blsr|bzhi|mulx|pdep|pext|rorx|sarx|shlx|shrx|lzcnt|tzcnt|movbe|xgetbv)$)' -or
                $operand -cmatch '\b[yz]mm\d') { $failures.Add("non-baseline startup instruction in $at") }
            if ($mnemonic -cne 'call' -and $mnemonic -cnotlike 'j*') { continue }
            if ($operand -cmatch '^qword ptr \[__imp_(\w+)\]$') {
                if ($Matches[1] -cnotin @('GetEnabledXStateFeatures', 'IsProcessorFeaturePresent', 'MessageBoxW')) {
                    $failures.Add("unexpected startup import in $at")
                }
                continue
            }
            $target = $null
            if ($operand -cmatch '^[0-9A-F]{8,16}$') {
                $target = [Convert]::ToUInt64($operand, 16)
                if ($target -ge $function.Start -and $target -le $function.End) { continue }
            } elseif ($byName.ContainsKey($operand)) { $target = $byName[$operand].Start }
            elseif ($Image.Names.ContainsKey($operand)) { $target = $Image.Names[$operand].Address }
            if ($null -eq $target) { $failures.Add("unresolved startup branch in $at"); continue }
            $callee = @($Image.Symbols[$target] | Where-Object { $null -ne $_ })
            if (@($callee | Where-Object { $_.Name -cmatch '^\?run_application@@' }).Count) {
                if ($function.Name -cne 'wWinMain') { $failures.Add("application body reachable before the CPU gate in $at") }
                ++$bodyCalls
                continue # The sole boundary to AVX2 application code.
            }
            if ($callee.Count -and -not @($callee | Where-Object { $_.Object -notmatch $trustedCrt }).Count) {
                # A generic CRT routine might invoke a product function pointer.
                # Only these callback-free helpers, plus atexit's checked target,
                # may be reached directly from baseline product code.
                $allowed = @($callee | Where-Object { $_.Name -cin @('atexit', '__security_check_cookie', '__chkstk', 'memcpy', 'memmove', 'memset') })
                if (-not $allowed.Count) { $failures.Add("unexpected CRT helper before the CPU gate in $at"); continue }
                if (@($allowed | Where-Object Name -CEQ 'atexit').Count) {
                    # The constinit NoDestroy object's only dynamic work is the
                    # compiler's lea rcx,[destructor]; jmp atexit. Any different
                    # registration shape needs an explicit audit update.
                    if ($i -le $function.First -or $Lines[$i - 1] -cnotmatch '^\s+[0-9A-F]{8,16}: lea\s+rcx,\[([0-9A-F]+)h\]\s*$') {
                        $failures.Add("unresolved static destructor registration in $at")
                    } else { $queue.Enqueue([Convert]::ToUInt64($Matches[1], 16)) }
                }
            }
            $queue.Enqueue([UInt64]$target)
        }
    }
    if ($bodyCalls -ne 1) { $failures.Add("expected one guarded application entry, found $bodyCalls") }
    foreach ($name in 'query_cpu_features', 'supports_required_cpu') {
        if (-not @($walked | Where-Object { $_.Name -cmatch "^\?$name@|::$name$" }).Count) {
            $failures.Add("CPU gate function not audited: $name")
        }
    }
    [pscustomobject]@{ Failures = $failures; Walked = $walked }
}

function Assert-StartupCode {
    param([string]$Executable, [string[]]$Lines, $Functions)
    $image = Read-StartupImage $Executable
    $audit = Test-StartupListing $Lines $Functions $image
    if ($audit.Failures.Count) { throw "Startup ISA audit failed:`n$($audit.Failures -join "`n")" }
    # Negative checks use the same addresses/table targets and mutate only the
    # instruction text, so they exercise the final image's actual startup roots.
    $guard = @($audit.Walked | Where-Object Name -CEQ 'wWinMain')[0]
    $initializer = @($audit.Walked | Where-Object { $_.Name -cmatch 'dynamic initializer|^\?\?__E' })
    if (-not $initializer.Count) { throw 'Startup audit expected the product callback-state initializer.' }
    foreach ($probe in @(
        @{ Function = $guard; Instruction = 'vmovdqu ymm0,ymmword ptr [rcx]'; Expected = 'non-baseline startup instruction' }
        @{ Function = $guard; Instruction = 'call rax'; Expected = 'unresolved startup branch' }
        @{ Function = $initializer[0]; Instruction = 'vzeroupper'; Expected = 'non-baseline startup instruction' }
        @{ Function = $initializer[0]; Instruction = 'call ?run_application@@YAHH@Z'; Expected = 'application body reachable before the CPU gate' }
    )) {
        $copy = [string[]]$Lines.Clone()
        $copy[$probe.Function.First] = '  {0:X16}: {1}' -f $probe.Function.Start, $probe.Instruction
        $result = Test-StartupListing $copy $Functions $image
        if (-not @($result.Failures | Where-Object { $_.StartsWith($probe.Expected, [StringComparison]::Ordinal) }).Count) {
            throw "Startup audit self-check accepted $($probe.Instruction) in $($probe.Function.Name)"
        }
    }
    Write-Output "Startup ISA audit: $($image.Roots.Count) CRT/TLS/entry roots; $($audit.Walked.Count) product functions stay on the SSE4.2 baseline; negative checks passed"
}
