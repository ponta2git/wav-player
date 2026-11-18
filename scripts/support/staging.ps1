# SPDX-License-Identifier: GPL-3.0-only
# Staging directories. A result is prepared in a uniquely named directory beside
# its destination and moved into place in one step, so a failure leaves neither
# a partial result nor, normally, temporary data. This file defines functions
# only; the entry scripts dot-source it.
# A scanner or a child process that has only just exited can hold a handle for a
# moment, so moving and removing are tried a few times before they give up.

# Creates <Parent>\<Prefix><random> and returns its full path. Parent is created
# when it does not exist yet. The random part is short: staged files have longer
# paths than their destination, and Windows limits path lengths.
function New-StagingDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Prefix
    )
    $parentPath = [IO.Path]::GetFullPath($Parent)
    [void][IO.Directory]::CreateDirectory($parentPath)
    do {
        $path = Join-Path $parentPath ($Prefix + [Guid]::NewGuid().ToString('N').Substring(0, 12))
    } while (Test-Path -LiteralPath $path)
    [void][IO.Directory]::CreateDirectory($path)
    $path
}

# Moves a finished staging directory, or a directory inside one, to Destination.
# An existing destination is refused, including one created concurrently.
function Move-StagingDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $attempts = 3
    for ($attempt = 1; ; ++$attempt) {
        try {
            [IO.Directory]::Move($Path, $Destination)
            return
        } catch {
            if ($attempt -ge $attempts -or (Test-Path -LiteralPath $Destination)) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}

# Removes a directory made by New-StagingDirectory with the same Parent and
# Prefix, and nothing else: any other path is refused before anything is deleted.
# A directory that is already gone, such as one moved to its destination, is fine.
# A directory that cannot be removed is reported as a warning, not as an error:
# callers clean up in a finally block, where an error would replace the failure
# that is being reported, or fail a result that was already delivered.
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
    $attempts = 3
    $reason = 'it still exists'
    for ($attempt = 1; $attempt -le $attempts -and (Test-Path -LiteralPath $resolved); ++$attempt) {
        if ($attempt -gt 1) { Start-Sleep -Milliseconds 250 }
        try {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction Stop
        } catch {
            $reason = $_.Exception.Message
        }
    }
    if (Test-Path -LiteralPath $resolved) {
        Write-Warning "A temporary directory could not be removed and can be deleted by hand: $resolved ($reason)"
    }
}
