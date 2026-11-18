# Retrieves the official SDK for a local build. Nothing from it is published.
# The archive is verified and expanded in a temporary directory and moved into
# place in one step; an existing external/asiosdk is never changed.
[CmdletBinding()]
param([string]$ArchivePath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/staging.ps1')

$url = 'https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip'
$expectedHash = 'D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA'
# Files the build and the license notice need from the expanded archive.
$requiredFiles = @('common/iasiodrv.h', 'LICENSE.txt', 'common/LICENSE.txt')

# Downloads the archive. A transient network failure is retried; the pinned
# hash, checked by the caller, decides whether the result is accepted.
function Save-OfficialArchive([string]$Uri, [string]$Path) {
    $attempts = 3
    for ($attempt = 1; ; ++$attempt) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Path
            return
        } catch {
            if ($attempt -ge $attempts) { throw }
            Write-Warning "Download attempt $attempt of $attempts failed; trying again."
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
}

$root = Split-Path -Parent $PSScriptRoot
$external = Join-Path $root 'external'
$destination = Join-Path $external 'asiosdk'
if (Test-Path -LiteralPath $destination) {
    throw 'external/asiosdk already exists; it has not been changed. Use the existing SDK or move it aside before fetching a fresh copy.'
}
$stagingPrefix = '.sdk-fetch-'
$staging = New-StagingDirectory $external $stagingPrefix
try {
    if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
        Write-Output 'Fetching ASIO SDK 2.3.4 from Steinberg for this local build.'
        Write-Output 'SDK terms: https://www.steinberg.net/developers/asiosdk-open/'
        $archive = Join-Path $staging 'sdk.zip'
        # Windows PowerShell redraws its progress display for every received
        # block, which dominates the download time.
        $ProgressPreference = 'SilentlyContinue'
        Save-OfficialArchive $url $archive
    } else {
        $archive = (Get-Item -LiteralPath $ArchivePath -ErrorAction Stop).FullName
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne $expectedHash) {
        throw 'The archive SHA-256 does not match the pinned official ASIO SDK 2.3.4. No SDK was installed.'
    }
    $expanded = Join-Path $staging 'expanded'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    $sdk = Join-Path $expanded 'ASIOSDK'
    foreach ($relative in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdk $relative) -PathType Leaf)) {
            throw ('The official archive is missing ' + $relative)
        }
    }
    # Directory.Move refuses an existing destination, including a concurrent creation.
    [IO.Directory]::Move($sdk, $destination)
    Write-Output 'SDK ready in external/asiosdk. It remains local and is not part of the source distribution.'
} finally {
    Remove-StagingDirectory $staging $external $stagingPrefix
}
