# SPDX-License-Identifier: GPL-3.0-only
# Checks the public source manifest of a source tree: every entry is a
# publishable path and an existing regular file reached without a junction or
# symlink, and no listed settings file selects a device.
#   -Tracked     the manifest also equals the files Git tracks.
#   -StrictTree  the manifest also equals every file in the tree, and the tree
#                holds no local state: for a freshly exported source tree.
[CmdletBinding()]
param(
    [string]$SourceDirectory,
    [switch]$Tracked,
    [switch]$StrictTree
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/public-manifest.ps1')

# Directories of local state, records, SDKs and build output. No public path
# passes through one, and an exported tree contains none, not even an empty one.
$localDirectories = @('.git', '.vs', '.claude', '.codex', '.agents', 'private', 'external', 'out')
# Binaries, archives, recordings and generated listings are never source.
$generatedExtensions = @('exe', 'dll', 'lib', 'obj', 'pdb', 'aps', 'zip', '7z', 'bundle', 'wav', 'etl', 'log', 'csv', 'map', 'asm')
$localDirectoryPattern = '(^|/)(' + (@($localDirectories | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')(/|$)'
$generatedFilePattern = '\.(' + ($generatedExtensions -join '|') + ')$'

# A manifest entry is a clean relative path with forward slashes: no surrounding
# space, drive, wildcard, backslash, leading slash or '.' and '..' segment.
function Assert-PublicPath([string]$Relative) {
    if ($Relative -ne $Relative.Trim() -or $Relative -match '[\\:*?"<>|]' -or
        $Relative.StartsWith('/') -or $Relative -match '(^|/)\.\.?(/|$)' -or
        $Relative -match $localDirectoryPattern -or $Relative -match $generatedFilePattern) {
        throw "Not a public source path: $Relative"
    }
}

# Do not follow junctions or symlinks into an unreviewed tree: neither the file
# nor any directory between it and the root may be a reparse point.
function Assert-NoReparsePoint([string]$Root, [string]$Relative) {
    $path = $Root
    foreach ($segment in $Relative.Split('/')) {
        $path = Join-Path $path $segment
        if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Reparse point in public source path: $Relative"
        }
    }
}

function Assert-UnselectedDevice([string]$Path, [string]$Relative) {
    if ([IO.File]::ReadAllText($Path) -match '(?m)^[ \t]*SelectedDevice[ \t]*=[ \t]*[^\s\r\n]+') {
        throw "Public settings must start without a selected device: $Relative"
    }
}

# Fails with the paths that are on one side only. Letter case counts: Git and an
# exported tree spell every path exactly as the manifest does.
function Assert-SameFiles([string[]]$Listed, [string[]]$Actual, [string]$Description) {
    $listedSet = [Collections.Generic.HashSet[string]]::new($Listed, [StringComparer]::Ordinal)
    $actualSet = [Collections.Generic.HashSet[string]]::new($Actual, [StringComparer]::Ordinal)
    $absent = @($Listed | Where-Object { -not $actualSet.Contains($_) })
    $unlisted = @($Actual | Where-Object { -not $listedSet.Contains($_) })
    if ($absent.Count -or $unlisted.Count) {
        throw "$Description differ from the public manifest: listed but absent [$($absent -join ', ')]; present but unlisted [$($unlisted -join ', ')]"
    }
}

if (-not $SourceDirectory) { $SourceDirectory = Split-Path -Parent $PSScriptRoot }
$root = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
$files = @(Read-PublicManifest $root)
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $files) {
    Assert-PublicPath $relative
    if (-not $seen.Add($relative)) { throw "Duplicate public source path: $relative" }
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing public source file: $relative" }
    Assert-NoReparsePoint $root $relative
    if ($relative.EndsWith('.ini')) { Assert-UnselectedDevice $path $relative }
}

if ($Tracked) {
    $trackedFiles = @(& git -C $root ls-files)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the tracked file list.' }
    Assert-SameFiles $files $trackedFiles 'Tracked files'
}
if ($StrictTree) {
    # This mode is for a freshly exported source tree, before downloading SDKs
    # or building. No hidden state or extra files are accepted, including .git.
    $treeFiles = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
        $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    })
    Assert-SameFiles $files $treeFiles 'Exported files'
    $extraDirectories = @(Get-ChildItem -LiteralPath $root -Recurse -Directory -Force | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $_.Name -in $localDirectories
    })
    if ($extraDirectories) { throw 'The source export contains local state or generated directories.' }
}
Write-Output "[PASS] Public source manifest: $($files.Count) files"
