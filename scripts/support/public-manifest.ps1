# SPDX-License-Identifier: GPL-3.0-only
# The public source manifest, build/public-files.txt, names every published
# file: one repository-relative path with forward slashes per line. Empty lines
# and lines starting with # are not entries. This file defines functions only;
# the entry scripts dot-source it.

# Returns the entries of the manifest below Root in file order, unvalidated;
# check-public-source.ps1 decides whether each one may be published.
function Read-PublicManifest {
    param([Parameter(Mandatory = $true)][string]$Root)
    $manifest = Join-Path $Root 'build/public-files.txt'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw 'The public source manifest build/public-files.txt is missing.'
    }
    $entries = @([IO.File]::ReadAllLines($manifest) | Where-Object { $_ -and -not $_.StartsWith('#') })
    if ($entries.Count -eq 0) { throw 'The public source manifest is empty.' }
    $entries
}
