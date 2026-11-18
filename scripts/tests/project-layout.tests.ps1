# SPDX-License-Identifier: GPL-3.0-only
# Runs check-project-layout.ps1 on a copy of the public tree, unchanged and with
# one violation at a time, and requires each violation to be rejected for its
# own reason. Nothing is built; the copy holds only the files of the manifest.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'support/public-manifest.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workParent = Join-Path $root 'out'
$workPrefix = 'layout-tests-'
$work = New-StagingDirectory $workParent $workPrefix
$tree = Join-Path $work 'tree'
# Every change to the copy is recorded here and undone after the check that needed it.
$changes = [Collections.Generic.Stack[object]]::new()

function Copy-PublicTree {
    foreach ($relative in Read-PublicManifest $root) {
        $target = Join-Path $tree $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target
    }
}

function Get-TreeText([string]$Relative) {
    [IO.File]::ReadAllText((Join-Path $tree $Relative))
}

# Records the present state of a file of the copy, so that Restore-Tree returns to it.
function Save-TreeFile([string]$Path) {
    $previous = if (Test-Path -LiteralPath $Path -PathType Leaf) { , [IO.File]::ReadAllBytes($Path) } else { $null }
    $changes.Push([PSCustomObject]@{ Path = $Path; Bytes = $previous })
}

function Set-TreeText([string]$Relative, [string]$Text) {
    $path = Join-Path $tree $Relative
    Save-TreeFile $path
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    Write-TestFile $path $Text
}

function Remove-TreeFile([string]$Relative) {
    $path = Join-Path $tree $Relative
    Save-TreeFile $path
    Remove-Item -LiteralPath $path
}

function Add-TreeLine([string]$Relative, [string]$Line) {
    Set-TreeText $Relative ((Get-TreeText $Relative) + "`n" + $Line + "`n")
}

# Replaces the first match of Pattern in a file of the copy; the pattern must match.
function Edit-TreeText([string]$Relative, [string]$Pattern, [string]$Replacement) {
    $text = Get-TreeText $Relative
    $regex = [regex]::new($Pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
    Assert-True $regex.IsMatch($text) "The layout fixture no longer has the text to alter in ${Relative}: $Pattern"
    Set-TreeText $Relative $regex.Replace($text, $Replacement, 1)
}

function Restore-Tree {
    while ($changes.Count) {
        $change = $changes.Pop()
        if ($null -eq $change.Bytes) {
            Remove-Item -LiteralPath $change.Path
        } else {
            [IO.File]::WriteAllBytes($change.Path, $change.Bytes)
        }
    }
}

function Invoke-LayoutCheck {
    & (Join-Path $tree 'scripts/check-project-layout.ps1') | Out-Null
}

# The copy with its recorded changes must pass, or fail for Expected.
function Assert-Accepted {
    try { Invoke-LayoutCheck } finally { Restore-Tree }
}

function Assert-Rejected([string]$Expected) {
    try { Assert-Fails { Invoke-LayoutCheck } $Expected } finally { Restore-Tree }
}

# The first file of a directory of the copy, as a path relative to the copy.
function Get-FirstFile([string]$Directory, [string]$Filter, [string[]]$Except = @()) {
    $file = Get-ChildItem -LiteralPath (Join-Path $tree $Directory) -File -Filter $Filter |
        Where-Object { $_.Name -notin $Except } | Sort-Object Name | Select-Object -First 1
    Assert-True ($null -ne $file) "The layout fixture has no $Filter file in $Directory."
    $Directory + '/' + $file.Name
}

function Test-UnchangedTree {
    Assert-Accepted
    Complete-TestGroup 'the public tree passes unchanged'
}

function Test-SourceRegistration {
    Set-TreeText 'src/wavplay/core/unregistered.h' "#pragma once`n"
    Assert-Rejected 'Source/Visual Studio registration mismatch'
    Edit-TreeText 'build/WavplaySources.props' '^([ \t]*<ClInclude Include="[^"]+" />\r?\n)' '$1$1'
    Assert-Rejected 'Duplicate source registrations'
    Edit-TreeText 'tests/wavplay_tests/wavplay_tests.vcxproj' '^.*WavplaySources\.props.*\r?\n' ''
    Assert-Rejected 'must import the shared product sources exactly once'
    Complete-TestGroup 'unregistered and doubly registered sources and a missing shared import are rejected'
}

function Test-LayerDependencies {
    $coreHeader = Get-FirstFile 'src/wavplay/core' '*.h'
    $uiHeader = Get-FirstFile 'src/wavplay/ui' '*.h'
    $internalHeader = Get-FirstFile 'src/wavplay/playback' '*.h' @('play_tracks.h', 'track_player.h', 'playback_types.h')
    Add-TreeLine $coreHeader ('#include "' + $uiHeader.Substring('src/'.Length) + '"')
    Assert-Rejected 'Forbidden layer dependency'
    Add-TreeLine 'src/wavplay/control/playback_controller.cpp' ('#include "' + $internalHeader.Substring('src/'.Length) + '"')
    Assert-Rejected 'Internal header of another layer'
    Add-TreeLine 'src/app/main.cpp' ('#include "' + $internalHeader.Substring('src/'.Length) + '"')
    Assert-Rejected 'Forbidden executable dependency'
    Add-TreeLine $coreHeader '#include "wavplay/core/no_such_header.h"'
    Assert-Rejected 'Missing product include'
    Complete-TestGroup 'forbidden layer and executable dependencies, internal headers and missing includes are rejected'
}

function Test-SdkBoundary {
    $coreHeader = Get-FirstFile 'src/wavplay/core' '*.h'
    $uiHeader = Get-FirstFile 'src/wavplay/ui' '*.h'
    Add-TreeLine $coreHeader '#include "asio.h"'
    Assert-Rejected 'Steinberg SDK include outside asio_sdk.h'
    Add-TreeLine $uiHeader '#include "wavplay/asio/asio_sdk.h"'
    Assert-Rejected 'Steinberg SDK reaches SDK-free code'
    Complete-TestGroup 'SDK includes outside the boundary and SDK-reaching GUI headers are rejected'
}

function Test-ProductTextFormatting {
    $coreHeader = Get-FirstFile 'src/wavplay/core' '*.h'
    foreach ($line in '#include <format>', '#include <sstream>',
        'inline void format_fixture(char* first, char* last) { std::to_chars(first, last, 1); }',
        'inline auto format_fixture() { return std::format("{}", 1); }') {
        Add-TreeLine $coreHeader $line
        Assert-Rejected 'Formatting library in product code'
    }
    Add-TreeLine $coreHeader '// std::format, std::to_chars and <sstream> stay out of product code.'
    Assert-Accepted
    Add-TreeLine $coreHeader 'inline int format_fixture() { return 1; } // not std::to_chars'
    Assert-Accepted
    Add-TreeLine $coreHeader '#include <charconv>'
    Assert-Accepted
    Complete-TestGroup 'formatting libraries are rejected in product code and accepted in comments'
}

function Test-SuiteTable {
    $table = 'tests/wavplay_tests/suites/suites.h'
    $entry = '^([ \t]*)Suite\{L"(\w+)", run_\w+_tests\},[ \t]*\r?\n'
    Edit-TreeText $table $entry ''
    Assert-Rejected 'Suite file needs exactly one suites.h entry'
    Edit-TreeText $table $entry ('$0$1Suite{L"ghost", run_ghost_tests},' + "`n")
    Assert-Rejected 'Suite table entry without a suite file'
    Edit-TreeText $table $entry ('$0$1Suite{L"ghost", run_other_tests},' + "`n")
    Assert-Rejected 'Suite entry name does not match its runner'
    $coreSuite = Get-FirstFile 'tests/wavplay_tests/suites/core' '*.cpp'
    Add-TreeLine $coreSuite '#include "fixtures/playback_fixture.h"'
    Assert-Rejected 'Forbidden test dependency'
    Add-TreeLine $coreSuite '#include "doubles/mock_asio.h"'
    Assert-Rejected 'Forbidden test dependency'
    Complete-TestGroup 'suite table mismatches and playback or ASIO doubles in lower suites are rejected'
}

function Test-SolutionItems {
    Set-TreeText 'docs/unlisted.md' "# Unlisted`n"
    Assert-Rejected 'Missing Solution Item'
    Remove-TreeFile 'packaging/player/README.txt'
    Assert-Rejected 'Solution Item does not exist'
    Complete-TestGroup 'unlisted support files and Solution Items without a file are rejected'
}

function Test-DocumentLinks {
    Add-TreeLine 'README.md' '[missing file](docs/no-such-file.md)'
    Assert-Rejected 'Link to a missing file'
    Add-TreeLine 'README.md' '[missing heading](docs/architecture.md#no-such-heading)'
    Assert-Rejected 'Link to a missing heading'
    # README.md links to headings of the requirements; renaming them all breaks those links.
    $requirements = (Get-TreeText 'docs/requirements.md') -replace '(?m)^(#{2,6} .*?)[ \t]*\r?$', '$1 renamed'
    Set-TreeText 'docs/requirements.md' $requirements
    Assert-Rejected 'Link to a missing heading'
    Add-TreeLine 'README.md' '[same file](#no-such-heading)'
    Assert-Rejected 'Link to a missing heading'
    Add-TreeLine 'README.md' '[external](https://example.invalid/page#fragment)'
    Assert-Accepted
    Complete-TestGroup 'links to missing files and headings are rejected; external links are not followed'
}

try {
    Copy-PublicTree
    Test-UnchangedTree
    Test-SourceRegistration
    Test-LayerDependencies
    Test-SdkBoundary
    Test-ProductTextFormatting
    Test-SuiteTable
    Test-SolutionItems
    Test-DocumentLinks
    Write-Output ('[PASS] Project layout tests: ' + (Get-TestGroupCount) + ' groups')
} finally {
    Remove-StagingDirectory $work $workParent $workPrefix
}
