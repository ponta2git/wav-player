# SPDX-License-Identifier: GPL-3.0-only
# The startup audit and Release layout comparison use one interpretation of the
# PE image and its linker map, including aliases that share a virtual address.
# Reads the public and static symbols of a linker map.
#   Symbols  address -> the symbols at it (Name, Address, Object); folded
#            functions share one address.
#   Names    name -> symbol.
function Read-LinkerMap {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The linker map is missing: $Path"
    }
    $symbols = @{}
    $names = [hashtable]::new([StringComparer]::Ordinal)
    $timestamp = $preferredBase = $entrySection = $entryOffset = $null
    $publicTable = $staticTable = $false
    $invalidRows = 0
    $symbolLine = '^\s+([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{16})\s+(f\s+)?(?:i\s+)?(\S.*?)\s*$'
    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -cmatch $symbolLine) {
            $address = [Convert]::ToUInt64($Matches[4], 16)
            $symbol = [pscustomobject]@{
                Name = $Matches[3]; Address = $address; Object = $Matches[6]; IsFunction = [bool]$Matches[5]
                SectionIndex = [Convert]::ToInt32($Matches[1], 16); Offset = [Convert]::ToUInt32($Matches[2], 16)
            }
            if (-not $symbols.ContainsKey($address)) { $symbols[$address] = @() }
            $symbols[$address] += $symbol
            $names[$symbol.Name] = $symbol
        } elseif ($line -cmatch '^\s+Timestamp is ([0-9A-Fa-f]{1,8})(?:\s|$)') {
            $timestamp = [Convert]::ToUInt32($Matches[1], 16)
        } elseif ($line -cmatch '^\s+Preferred load address is ([0-9A-Fa-f]{16})\s*$') {
            $preferredBase = [Convert]::ToUInt64($Matches[1], 16)
        } elseif ($line -cmatch '^\s+entry point at\s+([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s*$') {
            $entrySection = [Convert]::ToInt32($Matches[1], 16)
            $entryOffset = [Convert]::ToUInt32($Matches[2], 16)
        } elseif ($line -cmatch '^\s+Address\s+Publics by Value\s+') {
            $publicTable = $true
        } elseif ($line -cmatch '^\s*Static symbols\s*$') {
            $staticTable = $true
        } elseif ($publicTable -and $line -cmatch '^\s+[0-9A-Fa-f]{4}:') {
            ++$invalidRows
        }
    }
    [pscustomobject]@{
        Symbols = $symbols; Names = $names; Timestamp = $timestamp; PreferredBase = $preferredBase
        EntrySection = $entrySection; EntryOffset = $entryOffset
        HasSymbolTables = $publicTable -and $staticTable; InvalidSymbolRows = $invalidRows
    }
}

# Reads the headers of an x64 PE32+ file: its Bytes,
# the image Base, the size of the Headers, the Sections, and the addresses of
# the entry point (EntryPoint) and of the TLS directory (TlsDirectory, 0 if none).
function Read-PeImage {
    param([string]$Executable)
    $bytes = [IO.File]::ReadAllBytes($Executable)
    if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5a4d) { throw 'Missing DOS image header.' }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($pe -lt 64 -or $pe -gt $bytes.Length - 24 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'An x64 PE image is required.' }
    $optional = $pe + 24
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $sectionStart = $optional + $optionalSize
    if ($optionalSize -lt 112 -or $sectionStart + 40 * $sectionCount -gt $bytes.Length -or
        [BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) { throw 'Invalid PE32+ headers or section table.' }
    $base = [BitConverter]::ToUInt64($bytes, $optional + 24)
    $headers = [BitConverter]::ToUInt32($bytes, $optional + 60)
    if ($headers -lt $sectionStart + 40 * $sectionCount -or $headers -gt $bytes.Length) { throw 'Invalid PE header size.' }
    $sections = @(for ($i = 0; $i -lt $sectionCount; ++$i) {
        $offset = $sectionStart + 40 * $i
        $size = [BitConverter]::ToUInt32($bytes, $offset + 16)
        $raw = [BitConverter]::ToUInt32($bytes, $offset + 20)
        if ($size -and ($raw -lt $headers -or [long]$raw + $size -gt $bytes.Length)) { throw 'PE section exceeds the file.' }
        [pscustomobject]@{
            Name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 8).TrimEnd([char]0)
            Rva = [BitConverter]::ToUInt32($bytes, $offset + 12)
            VirtualSize = [BitConverter]::ToUInt32($bytes, $offset + 8)
            Size = $size; Raw = $raw
        }
    })
    # Data directory 9 of the optional header is the IMAGE_TLS_DIRECTORY64.
    $tlsRva = 0
    if ([BitConverter]::ToUInt32($bytes, $optional + 108) -gt 9) {
        if ($optionalSize -lt 112 + 10 * 8) { throw 'Truncated PE TLS directory entry.' }
        $tlsRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 9 * 8)
    }
    [pscustomobject]@{
        Bytes = $bytes
        Base = $base
        Headers = $headers
        Timestamp = [BitConverter]::ToUInt32($bytes, $pe + 8)
        Sections = $sections
        EntryPoint = $base + [BitConverter]::ToUInt32($bytes, $optional + 16)
        TlsDirectory = if ($tlsRva) { $base + $tlsRva } else { [UInt64]0 }
    }
}
