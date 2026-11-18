# SPDX-License-Identifier: GPL-3.0-only
# Writes the files of the public source manifest, and nothing else, to a new
# directory. The export is staged beside the destination and checked there, so a
# failure leaves no partial tree behind.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/public-manifest.ps1')
. (Join-Path $PSScriptRoot 'support/staging.ps1')
$root = Split-Path -Parent $PSScriptRoot
$checkPublicSource = Join-Path $PSScriptRoot 'check-public-source.ps1'

$destination = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if (Test-Path -LiteralPath $destination) { throw 'Choose a new, nonexistent source export directory.' }
$parent = [IO.Path]::GetDirectoryName($destination)
if ([string]::IsNullOrWhiteSpace($parent)) { throw 'Choose a new source export directory below an existing drive.' }

& $checkPublicSource
$stagingPrefix = '.source-export-'
$staging = New-StagingDirectory $parent $stagingPrefix
try {
    foreach ($relative in Read-PublicManifest $root) {
        $target = Join-Path $staging $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $target
    }
    & $checkPublicSource -SourceDirectory $staging -StrictTree
    # Directory.Move refuses an existing destination, including a concurrent creation.
    [IO.Directory]::Move($staging, $destination)
} finally {
    Remove-StagingDirectory $staging $parent $stagingPrefix
}
Write-Output 'Source export complete. No Git history, SDK, local settings or build output was copied.'
