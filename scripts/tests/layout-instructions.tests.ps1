# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'verification/disassembly.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'verification/layout-instructions.ps1')

# Each listing is paired with independent raw PE bytes. Give the function a
# section of exactly this length so its last instruction has no following label.
function Read-TestBody([long]$Start, [byte[]]$Bytes, [string[]]$Listing) {
    $image = [pscustomobject]@{
        Pe = [pscustomobject]@{ Base = [long]0; Bytes = $Bytes; Sections = @(
            [pscustomobject]@{ Rva = $Start; Raw = 0; Size = $Bytes.Length; VirtualSize = $Bytes.Length }) }
        Symbols = @([pscustomobject]@{ Name = 'test'; IsFunction = $true; Group = 'Callback'; Rva = $Start })
    }
    (Get-LayoutInstructionBodies $image $Listing)[$Start]
}

$exact = Read-TestBody 0x103A (0xC7,0xC0,1,0,0,0,0xC3) @(
    '  0000103A: C7 C0 01 00 00 00  mov         eax,1'
    '  00001040: C3                 ret')
Assert-True ($exact.Instructions.Count -eq 2 -and $exact.Crossings.Count -eq 0) 'Ending exactly on a cache-line boundary is not a crossing.'

$wrapped = Read-TestBody 0x103F (0x48,0xFF,0x25,0xBA,0,0,0) @(
    '  0000103F: 48 FF 25 BA 00 00  jmp         qword ptr [1100h]'
    '                    00')
Assert-True ($wrapped.Crossings.Count -eq 1 -and $wrapped.Crossings[0].Offset -eq 0 -and
    $wrapped.Crossings[0].Length -eq 7 -and $wrapped.IndirectJumps -eq 1) 'A final wrapped instruction that straddles a line was lost.'

$table = Read-TestBody 0x103B (0xFF,0xE0,0x48,0x8D,0x0D,0xBC,0,0,0) @(
    '  0000103B: FF E0              jmp         rax'
    '  0000103D: 48 8D 0D BC 00 00  lea         rcx,[1100h]'
    '                    00')
Assert-True ($table.IndirectJumps -eq 1 -and $table.Instructions.Count -eq 1 -and $table.Crossings.Count -eq 0) 'Bytes after an unresolved indirect jump were counted as code.'
Complete-TestGroup 'cache-line boundaries, wrapped last instruction and inline data'

# A function whose return ends just before a page boundary and whose throwing
# tail lies beyond it: test eax,eax / je tail / ret / tail: call / int 3.
$tail = Read-TestBody 0x1FF8 (0x85,0xC0,0x74,0x01,0xC3,0xE8,0xFE,0x0F,0,0,0xCC) @(
    '  00001FF8: 85 C0              test        eax,eax'
    '  00001FFA: 74 01              je          00001FFD'
    '  00001FFC: C3                 ret'
    '  00001FFD: E8 FE 0F 00 00     call        thrower'
    '  00002002: CC                 int         3')
Assert-True ($tail.Instructions.Count -eq 5 -and $tail.Returning.Count -eq 3 -and
    -not @($tail.Returning | Where-Object { $_.Rva -ge 0x1FFD }).Count) 'Instructions that only reach a trap were counted as returning.'
$bodies = @{ [long]0x1FF8 = $tail }
$footprint = Get-LayoutFootprint $bodies 'Callback'
Assert-True ($footprint.Functions -eq 1 -and $footprint.OpenFunctions -eq 0 -and $footprint.Instructions -eq 3 -and
    $footprint.Bytes -eq 5 -and $footprint.TrapOnlyInstructions -eq 2) 'The footprint does not separate the trap-only tail.'
Assert-True ($footprint.Pages -eq 1 -and $footprint.Lines -eq 1 -and $footprint.PageRvas.Count -eq 1 -and
    $footprint.PageRvas[0] -eq 0x1000) 'A trap-only tail beyond a page boundary added a page to the footprint.'
$other = Get-LayoutFootprint $bodies 'Refill'
Assert-True ($other.Functions -eq 0 -and $other.Pages -eq 0 -and $other.Lines -eq 0) 'A footprint counted another path''s function.'

# The same bytes two bytes later: now the return itself crosses the boundary.
$crossing = Read-TestBody 0x1FFC (0x85,0xC0,0x74,0x01,0xC3,0xE8,0xFA,0x0F,0,0,0xCC) @(
    '  00001FFC: 85 C0              test        eax,eax'
    '  00001FFE: 74 01              je          00002001'
    '  00002000: C3                 ret'
    '  00002001: E8 FA 0F 00 00     call        thrower'
    '  00002006: CC                 int         3')
$footprint = Get-LayoutFootprint @{ [long]0x1FFC = $crossing } 'Callback'
Assert-True ($footprint.Pages -eq 2 -and $footprint.Lines -eq 2 -and $footprint.Instructions -eq 3) 'A returning instruction on the next page was not counted.'

# A tail transfer leaves the body like a return; an unresolved indirect jump
# leaves it too, and marks the function open because the walk stops there.
$transfer = Read-TestBody 0x1000 (0x85,0xC0,0xE9,0xF9,0x0F,0,0) @(
    '  00001000: 85 C0              test        eax,eax'
    '  00001002: E9 F9 0F 00 00     jmp         elsewhere')
Assert-True ($transfer.Returning.Count -eq 2) 'A direct tail jump out of the function was treated as a trap.'
$footprint = Get-LayoutFootprint @{ [long]0x103B = $table } 'Callback'
Assert-True ($footprint.OpenFunctions -eq 1 -and $footprint.Instructions -eq 1) 'An unresolved indirect jump did not mark its function open.'
Complete-TestGroup 'path footprints: returning instructions, trap-only tails, page boundaries and open functions'

$before = Read-TestBody 0x1000 (0xE8,0xFB,0x0F,0,0,0xC3) @(
    '  00001000: E8 FB 0F 00 00     call        name_from_debug_symbols'
    '  00001005: C3                 ret')
$after = Read-TestBody 0x1100 (0xE8,0xFB,0x10,0,0,0xC3) @(
    '  00001100: E8 FB 10 00 00     call        same_or_different_display_name'
    '  00001105: C3                 ret')
$targets = @{ [long]0x2000 = [long]0x2200 }
Assert-True ((Compare-LayoutFunctionBody $before $before @{}) -ceq 'RawEqual') 'Identical encodings must remain observable without a target name.'
Assert-True ((Compare-LayoutFunctionBody $before $after $targets) -ceq 'Relocated') 'An encoded direct call to the same mapped target should be recognized.'
Assert-True ((Compare-LayoutFunctionBody $before $after @{}) -ceq 'Unknown') 'A similar disassembly name is not evidence for an unmapped target.'
$targets[[long]0x2000] = [long]0x2201
Assert-True ((Compare-LayoutFunctionBody $before $after $targets) -ceq 'Changed') 'A call to a different mapped target was accepted as relocation.'

$before = Read-TestBody 0x1000 (0x83,0x3D,0xF9,0x0F,0,0,1,0xC3) @(
    '  00001000: 83 3D F9 0F 00 00  cmp         dword ptr [2000h],1'
    '                    01'
    '  00001007: C3                 ret')
$after = Read-TestBody 0x1100 (0x83,0x3D,0xF9,0x10,0,0,2,0xC3) @(
    '  00001100: 83 3D F9 10 00 00  cmp         dword ptr [2200h],2'
    '                    02'
    '  00001107: C3                 ret')
$targets[[long]0x2000] = [long]0x2200
Assert-True ((Compare-LayoutFunctionBody $before $after $targets) -ceq 'Changed') 'Relocation must preserve the immediate after a RIP-relative displacement.'

$before = Read-TestBody 0x1000 (0x83,0xF8,1,0xC3) @(
    '  00001000: 83 F8 01           cmp         eax,1'
    '  00001003: C3                 ret')
$after = Read-TestBody 0x1000 (0x83,0xF8,2,0xC3) @(
    '  00001000: 83 F8 02           cmp         eax,2'
    '  00001003: C3                 ret')
Assert-True ((Compare-LayoutFunctionBody $before $after @{}) -ceq 'Changed') 'An immediate-only comparison change was missed.'
Complete-TestGroup 'encoded references, unresolved targets and scalar immediate changes'

Assert-Fails { Read-TestBody 0x1000 (0x90) @('  00001000: C3                 ret') } 'Disassembly bytes do not match'
$missing = Read-TestBody 0x1000 (0x90) @('no decoded instruction at the entry')
Assert-True ($missing.Missing -and (Compare-LayoutFunctionBody $missing $missing @{}) -ceq 'Unknown') 'A missing instruction was reported as identical.'
$unavailable = Compare-LayoutInstructions -Baseline $null -Candidate $null -SymbolComparison $null
Assert-True ($unavailable.Status -ceq 'Unavailable' -and $null -eq $unavailable.BaselineCrossings -and
    $null -eq $unavailable.CandidateCrossings) 'Missing dumpbin was reported as a numeric result.'
Complete-TestGroup 'unavailable instructions and disassembly that disagrees with the PE'
