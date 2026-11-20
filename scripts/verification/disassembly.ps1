# Loaded by verify.ps1. How a dumpbin /disasm:nobytes listing is read, shared by
# the audits that walk machine code (realtime-code.ps1, startup-code.ps1) so
# that they agree on what an instruction, a label and a branch target are.
# dumpbin prints mnemonics and registers in lower case and addresses and data
# bytes in upper case, so every match on its output is case-sensitive (-cmatch).

# The line patterns of a listing. Audits copy the ones they use into local
# variables once: their loops run for every line of the listing.
function Get-DisassemblyGrammar {
    [pscustomobject]@{
        # An instruction line: indentation, the address, then the text (group 2).
        InstructionLine = '^\s+([0-9A-F]{8,16}): (.*)$'
        # int3 padding, printed as data bytes: only CC up to the gap before the
        # character column.
        Padding = '^CC(?: CC)*(?:\s{2}|$)'
        # The text of an instruction: an optional prefix, the mnemonic, the operands.
        Instruction = '^(?:(lock|rep\w*|notrack|bnd) )?([a-z]\w*)\s*(.*?)\s*$'
        # A call through the import table; group 1 is the imported function.
        ImportOperand = '^qword ptr \[__imp_(\w+)\]$'
        # A direct target that dumpbin prints as an address, not as a name.
        AddressOperand = '^[0-9A-F]{8,16}$'
        # MSVC 14.52 can append an eight-digit RVA to a symbol's name.
        SymbolizedOperand = '^(\S.*?) \(([0-9A-F]{8})\)$'
    }
}

# A symbolized target must match both the label and its actual entry address.
function Get-SymbolizedFunctionAddress {
    param($Index, [UInt64]$ImageBase, [string]$Name, [string]$Rva)
    $address = $ImageBase + [Convert]::ToUInt32($Rva, 16)
    if ($Index.ByAddress.ContainsKey($address) -and $Index.Functions[$Index.ByAddress[$address]].Name -ceq $Name) {
        return $address
    }
}

# Returns the functions of a listing in address order with their lookup tables.
# Instructions are indented; the other lines up to the next instruction form the
# label. Captured output does not wrap labels, so joining lines is only defensive.
#   Functions  Name, the line indexes First and Last, the addresses Start and End.
#   ByName     name -> indexes into Functions. Only undecorated internal-linkage
#              labels repeat today, and dumpbin prints calls to those as
#              addresses; keeping every index is defensive.
#   ByAddress  start address -> index into Functions.
function Get-DisassemblyFunctions {
    param([string[]]$Lines)
    $instructionLine = (Get-DisassemblyGrammar).InstructionLine
    $functions = [Collections.Generic.List[object]]::new()
    $byName = [hashtable]::new([StringComparer]::Ordinal)
    $byAddress = @{}
    $label = ''
    $current = $null
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -cmatch $instructionLine) {
            $address = [Convert]::ToUInt64($Matches[1], 16)
            if ($label.EndsWith(':')) {
                $current = [pscustomobject]@{ Name = $label.TrimEnd(':'); First = $i; Last = $i; Start = $address; End = $address }
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
        } elseif ($Lines[$i] -cmatch '^\S') {
            $label += $Lines[$i]
        } else {
            $label = ''
        }
    }
    [pscustomobject]@{ Functions = $functions; ByName = $byName; ByAddress = $byAddress }
}

# Returns a copy of Lines with Instructions inserted after the instruction line
# at index After, each at that line's address: the altered listings of the
# self-checks stay readable by the same grammar.
function Add-ListingInstruction {
    param([string[]]$Lines, [int]$After, [string[]]$Instructions)
    if ($Lines[$After] -cnotmatch (Get-DisassemblyGrammar).InstructionLine) {
        throw "Self-check insertion point is not an instruction line: $($Lines[$After])"
    }
    $address = $Matches[1]
    $copy = [Collections.Generic.List[string]]::new($Lines)
    $copy.InsertRange($After + 1, [string[]]@($Instructions | ForEach-Object { "  ${address}: $_" }))
    , $copy.ToArray()
}
