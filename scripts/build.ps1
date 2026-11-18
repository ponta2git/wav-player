# Build on the development PC; the resulting folder needs no build tools on the playback PC.
# Everything that can be checked is checked before the build, the folder is
# assembled in a temporary directory beside its destination, and an existing
# folder is never replaced.
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$FetchSdk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/staging.ps1')
. (Join-Path $PSScriptRoot 'support/visual-studio.ps1')
$root = Split-Path -Parent $PSScriptRoot

# Name in the player folder -> source in the repository. Only this explicit list
# is copied, and setup.ps1 and its helpers run from that flat folder.
$sourceFiles = [ordered]@{
    'setup.cmd' = 'packaging/player/setup.cmd'
    'setup.ps1' = 'packaging/player/setup.ps1'
    'check-asio-drivers.ps1' = 'scripts/check-asio-drivers.ps1'
    'make-playlist.ps1' = 'scripts/make-playlist.ps1'
    'settings.example.ini' = 'settings.example.ini'
    'LICENSE' = 'LICENSE'
    'README.txt' = 'packaging/player/README.txt'
}
$builtExecutable = 'out/x64/Release/wavplay.exe'

# Copies Files (name -> full source path) into a staging directory beside Output
# and moves it into place in one step. ExecutableHash is the hash of the verified
# wavplay.exe; a copy that differs from it is not delivered.
function Publish-PlayerFolder([Collections.IDictionary]$Files, [string]$ExecutableHash, [string]$Output) {
    $parent = [IO.Path]::GetDirectoryName($Output)
    if ([string]::IsNullOrWhiteSpace($parent)) { throw 'Choose a new folder below an existing drive.' }
    $stagingPrefix = '.player-build-'
    $staging = New-StagingDirectory $parent $stagingPrefix
    try {
        foreach ($name in $Files.Keys) {
            Copy-Item -LiteralPath $Files[$name] -Destination (Join-Path $staging $name)
        }
        if ((Get-FileHash -LiteralPath (Join-Path $staging 'wavplay.exe') -Algorithm SHA256).Hash -cne $ExecutableHash) {
            throw 'The Release executable changed while preparing the transfer folder; build again into a new folder.'
        }
        # The move refuses an existing destination, including a concurrent creation.
        Move-StagingDirectory $staging $Output
    } finally {
        Remove-StagingDirectory $staging $parent $stagingPrefix
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'out/player'
}
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if (Test-Path -LiteralPath $output) {
    throw 'The output folder already exists. Choose a new -OutputDirectory; existing players and settings are never replaced.'
}

# Validate the tools and inputs before downloading or spending time on the build.
$null = Find-VisualStudio
if ($FetchSdk) { & (Join-Path $PSScriptRoot 'fetch-asio-sdk.ps1') }
if (-not (Test-Path -LiteralPath (Join-Path $root 'external/asiosdk/common/iasiodrv.h') -PathType Leaf)) {
    throw 'ASIO SDK 2.3.4 is missing. Read docs/dependencies.md, or run build.cmd -FetchSdk to retrieve the pinned official archive.'
}
$files = [ordered]@{}
foreach ($name in $sourceFiles.Keys) {
    $files[$name] = Join-Path $root $sourceFiles[$name]
    if (-not (Test-Path -LiteralPath $files[$name] -PathType Leaf)) { throw ('Required file is missing: ' + $sourceFiles[$name]) }
}

& (Join-Path $PSScriptRoot 'verify.ps1') -Configuration Release

$executable = Join-Path $root $builtExecutable
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'The verified Release executable was not produced.' }
$files['wavplay.exe'] = $executable
Publish-PlayerFolder $files (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash $output

Write-Output ('Player folder: ' + $output)
Write-Output 'Copy the whole folder to your playback PC, then run setup.cmd there. No SDK or build tools are needed there.'
Write-Output 'This locally built folder is for your own PCs; the project publishes source only.'
