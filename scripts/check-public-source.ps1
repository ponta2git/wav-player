# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [string]$SourceDirectory,
    [switch]$Tracked,
    [switch]$StrictTree
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $SourceDirectory) { $SourceDirectory = Split-Path -Parent $PSScriptRoot }
$root = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\', '/')
$manifest = Join-Path $root 'build/public-files.txt'
$files = @(Get-Content -LiteralPath $manifest | Where-Object { $_ -and -not $_.StartsWith('#') })
if ($files.Count -eq 0) { throw 'The public source manifest is empty.' }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $files) {
    if ($relative -ne $relative.Trim() -or $relative -match '[\\:*?"<>|]' -or
        $relative.StartsWith('/') -or $relative -match '(^|/)\.\.?(/|$)' -or
        $relative -match '^(private|out|external|\.git|\.vs|\.claude|\.codex|\.agents)(/|$)' -or
        $relative -match '\.(exe|dll|lib|obj|pdb|aps|zip|7z|bundle|wav|etl|log|csv|map|asm)$') {
        throw "Not a public source path: $relative"
    }
    if (-not $seen.Add($relative)) { throw "Duplicate public source path: $relative" }
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing public source file: $relative" }
    # Do not follow junctions or symlinks into an unreviewed tree.
    $node = Get-Item -LiteralPath $path -Force
    while ($node.FullName -ne $root) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Reparse point in public source path: $relative"
        }
        $node = Get-Item -LiteralPath (Split-Path -Parent $node.FullName) -Force
    }
    if ($relative.EndsWith('.ini') -and
        ([IO.File]::ReadAllText($path) -match '(?m)^[ \t]*SelectedDevice[ \t]*=[ \t]*[^\s\r\n]+')) {
        throw "Public settings must start without a selected device: $relative"
    }
}

if ($Tracked) {
    $actual = @(& git -C $root ls-files)
    if ($LASTEXITCODE) { throw 'Cannot read the tracked file list.' }
    $difference = @(Compare-Object $files $actual)
    if ($difference) { throw "Tracked files differ from the public manifest: $($difference.InputObject -join ', ')" }
}
if ($StrictTree) {
    # This mode is for a freshly exported source tree, before downloading SDKs
    # or building. No hidden state or extra files are accepted, including .git.
    $actual = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object {
        $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    })
    $difference = @(Compare-Object $files $actual)
    if ($difference) { throw "Exported files differ from the public manifest: $($difference.InputObject -join ', ')" }
    $extraDirectories = @(Get-ChildItem -LiteralPath $root -Recurse -Directory -Force | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $_.Name -in '.git', '.vs', '.claude', '.codex', '.agents', 'private', 'external', 'out'
    })
    if ($extraDirectories) { throw 'The source export contains local state or generated directories.' }
}
Write-Output "[PASS] Public source manifest: $($files.Count) files"
