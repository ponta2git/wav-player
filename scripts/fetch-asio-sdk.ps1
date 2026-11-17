# Retrieves the official SDK for a local build. Nothing from it is published.
[CmdletBinding()]
param([string]$ArchivePath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'external/asiosdk'
if (Test-Path -LiteralPath $destination) {
    throw 'external/asiosdk already exists; it has not been changed. Use the existing SDK or move it aside before fetching a fresh copy.'
}
$url = 'https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip'
$expectedHash = 'D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA'
$external = Join-Path $root 'external'
[IO.Directory]::CreateDirectory($external) | Out-Null
$temporary = Join-Path $external ('.sdk-fetch-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
        Write-Output 'Fetching ASIO SDK 2.3.4 from Steinberg for this local build.'
        Write-Output 'SDK terms: https://www.steinberg.net/developers/asiosdk-open/'
        $archive = Join-Path $temporary 'sdk.zip'
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
    } else {
        $archive = (Get-Item -LiteralPath $ArchivePath -ErrorAction Stop).FullName
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne $expectedHash) {
        throw 'The archive SHA-256 does not match the pinned official ASIO SDK 2.3.4. No SDK was installed.'
    }
    $expanded = Join-Path $temporary 'expanded'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    $sdk = Join-Path $expanded 'ASIOSDK'
    foreach ($relative in @('common/iasiodrv.h', 'LICENSE.txt', 'common/LICENSE.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $sdk $relative) -PathType Leaf)) {
            throw ('The official archive is missing ' + $relative)
        }
    }
    # Directory.Move refuses an existing destination, including a concurrent creation.
    [IO.Directory]::Move($sdk, $destination)
    Write-Output 'SDK ready in external/asiosdk. It remains local and is not part of the source distribution.'
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    $allowed = [IO.Path]::GetFullPath($external).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolved).StartsWith('.sdk-fetch-'))) {
        throw 'Refusing to remove an unexpected SDK temporary directory.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
