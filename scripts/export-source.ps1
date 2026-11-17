# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'Choose a new, nonexistent source export directory.' }
& (Join-Path $PSScriptRoot 'check-public-source.ps1')
$files = @(Get-Content -LiteralPath (Join-Path $root 'build/public-files.txt') |
    Where-Object { $_ -and -not $_.StartsWith('#') })
[void][IO.Directory]::CreateDirectory($destination)
foreach ($relative in $files) {
    $target = Join-Path $destination $relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target
}
& (Join-Path $PSScriptRoot 'check-public-source.ps1') -SourceDirectory $destination -StrictTree
Write-Output 'Source export complete. No Git history, SDK, local settings or build output was copied.'
