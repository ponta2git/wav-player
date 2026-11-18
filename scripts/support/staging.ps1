# SPDX-License-Identifier: GPL-3.0-only
# Staging directories. A result is prepared in a uniquely named directory beside
# its destination and moved into place in one step, so a failure leaves neither
# a partial result nor temporary data. This file defines functions only; the
# entry scripts dot-source it.

# Creates <Parent>\<Prefix><random> and returns its full path. Parent is created
# when it does not exist yet.
function New-StagingDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Prefix
    )
    $path = Join-Path ([IO.Path]::GetFullPath($Parent)) ($Prefix + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($path)
    $path
}

# Removes a directory made by New-StagingDirectory with the same Parent and
# Prefix, and nothing else: any other path is refused before anything is deleted.
# A directory that is already gone, such as one moved to its destination, is fine.
function Remove-StagingDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Prefix
    )
    $resolved = [IO.Path]::GetFullPath($Path)
    $actualParent = ([string][IO.Path]::GetDirectoryName($resolved)).TrimEnd('\', '/')
    $expectedParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    if (-not $actualParent.Equals($expectedParent, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith($Prefix, [StringComparison]::Ordinal)) {
        throw "Refusing to remove an unexpected temporary directory: $resolved"
    }
    # A scanner or a child process that has only just exited can still hold a
    # handle for a moment, so removal is tried a few times before it fails.
    $attempts = 3
    for ($attempt = 1; Test-Path -LiteralPath $resolved; ++$attempt) {
        try {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
        } catch {
            if ($attempt -ge $attempts) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}
