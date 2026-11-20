# Loaded by verify.ps1 after disassembly.ps1. The isolated wWinMain is not the
# first code executed: the loader calls TLS callbacks and the static CRT walks
# initialization tables. Resolve those tables in the final PE, using its linker
# map for object provenance. Prebuilt Microsoft CRT objects and Windows imports
# are the trust boundary; their own CPU dispatch remains intact. Every product
# function before the gate, and every product static destructor, must stay
# within the SSE4.2 baseline.

# Reads the public symbols of a linker map.
#   Symbols  address -> the symbols at it (Name, Address, Object); folded
#            functions share one address.
#   Names    name -> symbol.
function Read-LinkerMap {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Startup audit needs the linker map beside the executable: $Path"
    }
    $symbols = @{}
    $names = [hashtable]::new([StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -cmatch '^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$') {
            $address = [Convert]::ToUInt64($Matches[2], 16)
            $symbol = [pscustomobject]@{ Name = $Matches[1]; Address = $address; Object = $Matches[3] }
            if (-not $symbols.ContainsKey($address)) { $symbols[$address] = @() }
            $symbols[$address] += $symbol
            $names[$symbol.Name] = $symbol
        }
    }
    [pscustomobject]@{ Symbols = $symbols; Names = $names }
}

# Reads what the audit needs from the headers of an x64 PE32+ file: its Bytes,
# the image Base, the size of the Headers, the Sections, and the addresses of
# the entry point (EntryPoint) and of the TLS directory (TlsDirectory, 0 if none).
function Read-PeImage {
    param([string]$Executable)
    $bytes = [IO.File]::ReadAllBytes($Executable)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'Startup audit requires an x64 PE image.' }
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) { throw 'Startup audit requires PE32+.' }
    $base = [BitConverter]::ToUInt64($bytes, $optional + 24)
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
    # Data directory 9 of the optional header is the IMAGE_TLS_DIRECTORY64.
    $tlsRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 9 * 8)
    [pscustomobject]@{
        Bytes = $bytes
        Base = $base
        Headers = [BitConverter]::ToUInt32($bytes, $optional + 60)
        Sections = $sections
        EntryPoint = $base + [BitConverter]::ToUInt32($bytes, $optional + 16)
        TlsDirectory = if ($tlsRva) { $base + $tlsRva } else { [UInt64]0 }
    }
}

# Returns the 64-bit value the loaded image holds at the virtual address Address.
function Read-ImagePointer {
    param($Pe, [UInt64]$Address)
    if ($Address -lt $Pe.Base) { throw "Startup pointer address below image: $Address" }
    $rva = $Address - $Pe.Base
    if ($rva + 8 -le $Pe.Headers) {
        $raw = $rva
    } else {
        $section = @($Pe.Sections | Where-Object { $rva -ge $_.Rva -and $rva + 8 -le [UInt64]$_.Rva + [Math]::Max($_.Size, $_.VirtualSize) })
        if ($section.Count -ne 1) { throw ('Startup pointer is not backed by one PE section: {0:X}' -f $Address) }
        $within = $rva - $section[0].Rva
        # PE virtual tails (.bss, including unused dynamic TLS pointers) are
        # initialized to zero by the loader and have no bytes in the file.
        if ($within -ge $section[0].Size) { return [UInt64]0 }
        if ($within + 8 -gt $section[0].Size) { throw 'Startup pointer crosses the file-backed/zero-fill boundary.' }
        $raw = [UInt64]$section[0].Raw + $within
    }
    if ($raw + 8 -gt $Pe.Bytes.Length) { throw 'Startup pointer extends past the PE file.' }
    [BitConverter]::ToUInt64($Pe.Bytes, [int]$raw)
}

# Returns the addresses of everything the loader and the static CRT call before
# and around wWinMain: the PE entry point, the CRT tables, the TLS callbacks,
# the dynamic TLS callbacks, and wWinMain itself.
function Get-StartupRoots {
    param($Pe, $Map)
    $names = $Map.Names
    $roots = [Collections.Generic.List[UInt64]]::new()
    $roots.Add($Pe.EntryPoint)
    # Include C/C++ initialization and pre-termination/termination tables.
    foreach ($kind in 'xi', 'xc', 'xp', 'xt') {
        $first = "__${kind}_a"; $last = "__${kind}_z"
        if (-not $names.ContainsKey($first) -or -not $names.ContainsKey($last)) { throw "Startup table $kind is missing from the linker map." }
        $start = $names[$first].Address; $end = $names[$last].Address
        if ($end -lt $start -or ($end - $start) % 8 -ne 0 -or $end - $start -gt 65536) { throw "Invalid CRT table $kind" }
        if ((Read-ImagePointer $Pe $start) -ne 0 -or (Read-ImagePointer $Pe $end) -ne 0) { throw "Non-null CRT table sentinel $kind" }
        for ($address = $start + 8; $address -lt $end; $address += 8) {
            $target = Read-ImagePointer $Pe $address
            if ($target) { $roots.Add($target) }
        }
    }
    # The loader's IMAGE_TLS_DIRECTORY64 can run code before the PE entry point.
    # Its fourth field is the address of the null-terminated callback table.
    if ($Pe.TlsDirectory) {
        $callbacks = Read-ImagePointer $Pe ($Pe.TlsDirectory + 24)
        if ($callbacks) {
            $ended = $false
            for ($i = 0; $i -lt 1024; ++$i) {
                $target = Read-ImagePointer $Pe ($callbacks + 8 * $i)
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
            $target = Read-ImagePointer $Pe $names[$name].Address
            if ($target) { $roots.Add($target) }
        }
    }
    if (-not $names.ContainsKey('wWinMain')) { throw 'Baseline wWinMain is missing from the map.' }
    $roots.Add($names['wWinMain'].Address)
    , $roots.ToArray()
}

# Returns the map symbols (Symbols, Names), the startup Roots and the image Base
# of an executable whose linker map lies beside it.
function Read-StartupImage {
    param([string]$Executable)
    $map = Read-LinkerMap ([IO.Path]::ChangeExtension($Executable, '.map'))
    $pe = Read-PeImage $Executable
    [pscustomobject]@{ Symbols = $map.Symbols; Names = $map.Names; Roots = (Get-StartupRoots $pe $map); Base = $pe.Base }
}

# Walks a listing from the startup roots of Image through product code and
# returns the violations, product functions it walked and the first static
# destructor registration for the self-check. Index is the listing's function index.
function Test-StartupListing {
    param([string[]]$Lines, $Index, $Image)
    $failures = [Collections.Generic.List[string]]::new()
    $walked = [Collections.Generic.List[object]]::new()
    $registration = $null
    $grammar = Get-DisassemblyGrammar
    $instructionLine = $grammar.InstructionLine
    $padding = $grammar.Padding
    $instruction = $grammar.Instruction
    $importOperand = $grammar.ImportOperand
    $addressOperand = $grammar.AddressOperand
    $trustedCrt = '^(?:libcmt|libcpmt|libucrt|libvcruntime):'
    # VEX and mask instructions, and the scalar extensions beyond SSE4.2.
    $beyondBaseline = '^(?:v|k|(?:andn|bextr|blsi|blsmsk|blsr|bzhi|mulx|pdep|pext|rorx|sarx|shlx|shrx|lzcnt|tzcnt|movbe|xgetbv)$)'
    $allowedImports = @('GetEnabledXStateFeatures', 'IsProcessorFeaturePresent', 'MessageBoxW')
    # A generic CRT routine might invoke a product function pointer. Only these
    # callback-free helpers, plus atexit's checked target, may be reached
    # directly from baseline product code.
    $allowedCrtHelpers = @('atexit', '__security_check_cookie', '__chkstk', 'memcpy', 'memmove', 'memset')
    $queue = [Collections.Generic.Queue[UInt64]]::new()
    foreach ($root in $Image.Roots) { $queue.Enqueue($root) }
    # Compiler-generated static destructors need not appear in a PE table: their
    # initializers register their addresses with atexit. Audit them independently.
    foreach ($function in $Index.Functions) {
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
        if (-not $Index.ByAddress.ContainsKey($address)) { $failures.Add(('startup target absent from disassembly: {0:X}' -f $address)); continue }
        $function = $Index.Functions[$Index.ByAddress[$address]]
        $walked.Add($function)
        for ($i = $function.First; $i -le $function.Last; ++$i) {
            if ($Lines[$i] -cnotmatch $instructionLine) { continue }
            $text = $Matches[2]
            $at = "$($function.Name): $($text -replace '\s+', ' ')"
            if ($text -cmatch $padding) { continue }
            if ($text -cnotmatch $instruction) {
                $failures.Add("unreadable startup instruction in $at"); continue
            }
            $mnemonic = $Matches[2]; $operand = $Matches[3]
            if ($mnemonic -cmatch $beyondBaseline -or $operand -cmatch '\b[yz]mm\d') {
                $failures.Add("non-baseline startup instruction in $at")
            }
            if ($mnemonic -cne 'call' -and $mnemonic -cnotlike 'j*') { continue }
            if ($operand -cmatch $importOperand) {
                if ($Matches[1] -cnotin $allowedImports) {
                    $failures.Add("unexpected startup import in $at")
                }
                continue
            }
            $target = $null
            if ($operand -cmatch $addressOperand) {
                $target = [Convert]::ToUInt64($operand, 16)
                if ($target -ge $function.Start -and $target -le $function.End) { continue }
            } elseif ($operand -cmatch $grammar.SymbolizedOperand) {
                $target = Get-SymbolizedFunctionAddress $Index $Image.Base $Matches[1] $Matches[2]
            } elseif ($Index.ByName.ContainsKey($operand)) {
                $target = $Index.Functions[$Index.ByName[$operand][-1]].Start
            } elseif ($Image.Names.ContainsKey($operand)) {
                $target = $Image.Names[$operand].Address
            }
            if ($null -eq $target) { $failures.Add("unresolved startup branch in $at"); continue }
            $callee = @($Image.Symbols[$target] | Where-Object { $null -ne $_ })
            if (@($callee | Where-Object { $_.Name -cmatch '^\?run_application@@' }).Count) {
                if ($function.Name -cne 'wWinMain') { $failures.Add("application body reachable before the CPU gate in $at") }
                ++$bodyCalls
                continue # The sole boundary to AVX2 application code.
            }
            if ($callee.Count -and -not @($callee | Where-Object { $_.Object -notmatch $trustedCrt }).Count) {
                $allowed = @($callee | Where-Object { $_.Name -cin $allowedCrtHelpers })
                if (-not $allowed.Count) { $failures.Add("unexpected CRT helper before the CPU gate in $at"); continue }
                if (@($allowed | Where-Object Name -CEQ 'atexit').Count) {
                    # The constinit NoDestroy object's only dynamic work is the
                    # compiler's lea rcx,[destructor]; jmp atexit. Audit the
                    # registered function through the linker map as another root.
                    $destructor = $null
                    if ($i -gt $function.First -and $Lines[$i - 1] -cmatch '^\s+([0-9A-F]{8,16}): lea\s+rcx,\[(.+)\]\s*$') {
                        $leaAddress = $Matches[1]
                        $referent = $Matches[2]
                        if ($referent -cmatch '^([0-9A-F]{8,16})h$') {
                            $destructor = [Convert]::ToUInt64($Matches[1], 16)
                        } elseif ($referent -cmatch $grammar.SymbolizedOperand) {
                            $destructor = Get-SymbolizedFunctionAddress $Index $Image.Base $Matches[1] $Matches[2]
                        }
                    }
                    if ($null -eq $destructor -or -not $Index.ByAddress.ContainsKey($destructor) -or -not $Image.Symbols.ContainsKey($destructor)) {
                        $failures.Add("unresolved static destructor registration in $at")
                    } else {
                        $queue.Enqueue($destructor)
                        if ($null -eq $registration) {
                            $registration = @{ Line = $i - 1; LeaAddress = $leaAddress; Target = $destructor }
                        }
                    }
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
    [pscustomobject]@{ Failures = $failures; Walked = $walked; Registration = $registration }
}

# Audits the startup code of Executable in its disassembly Lines with the
# function Index of that listing, then checks that the audit rejects each
# violation written into a copy of the listing.
function Assert-StartupCode {
    param([string]$Executable, [string[]]$Lines, $Index)
    $image = Read-StartupImage $Executable
    $audit = Test-StartupListing $Lines $Index $image
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
        # Replacing a line in place keeps every line index of Index valid.
        $copy = [string[]]$Lines.Clone()
        $copy[$probe.Function.First] = '  {0:X16}: {1}' -f $probe.Function.Start, $probe.Instruction
        $result = Test-StartupListing $copy $Index $image
        if (-not @($result.Failures | Where-Object { $_.StartsWith($probe.Expected, [StringComparison]::Ordinal) }).Count) {
            throw "Startup audit self-check accepted $($probe.Instruction) in $($probe.Function.Name)"
        }
    }
    $registration = $audit.Registration
    if ($null -eq $registration) { throw 'Startup audit self-check needs a static destructor registration.' }
    $destructor = $Index.Functions[$Index.ByAddress[$registration.Target]]
    $rva = $destructor.Start - $image.Base
    $copy = [string[]]$Lines.Clone()
    foreach ($operand in ('{0:X}h' -f $destructor.Start), ('{0} ({1:X8})' -f $destructor.Name, $rva)) {
        $copy[$registration.Line] = '  {0}: lea rcx,[{1}]' -f $registration.LeaAddress, $operand
        $result = Test-StartupListing $copy $Index $image
        if ($result.Failures.Count) { throw "Startup audit self-check rejected a valid destructor address: $($result.Failures -join ' | ')" }
    }
    foreach ($operand in ('missing_destructor ({0:X8})' -f $rva), ('{0} (FFFFFFFF)' -f $destructor.Name)) {
        $copy[$registration.Line] = '  {0}: lea rcx,[{1}]' -f $registration.LeaAddress, $operand
        $result = Test-StartupListing $copy $Index $image
        Assert-Rejected 'Startup ISA audit' $operand $result.Failures 'unresolved static destructor registration'
    }
    Write-Output "Startup ISA audit: $($image.Roots.Count) CRT/TLS/entry roots; $($audit.Walked.Count) product functions stay on the SSE4.2 baseline; self-checks passed"
}
