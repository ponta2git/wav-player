# Build on the development PC; the resulting folder needs no build tools on the playback PC.
[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$FetchSdk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'out/player'
}
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
if (Test-Path -LiteralPath $output) {
    throw 'The output folder already exists. Choose a new -OutputDirectory; existing players and settings are never replaced.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Install Visual Studio 2026 with the repository .vsconfig, then run build.cmd again.'
}
$install = & $vswhere -latest -products '*' -version '[18,19)' -requires `
    Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.Windows11SDK.26100 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $install) {
    throw 'Install the Visual Studio 2026 C++ tools and Windows SDK 10.0.26100.0 from .vsconfig.'
}
if ($FetchSdk) { & (Join-Path $PSScriptRoot 'fetch-asio-sdk.ps1') }
if (-not (Test-Path -LiteralPath (Join-Path $root 'external/asiosdk/common/iasiodrv.h') -PathType Leaf)) {
    throw 'ASIO SDK 2.3.4 is missing. Read docs/dependencies.md, or run build.cmd -FetchSdk to retrieve the pinned official archive.'
}

# Validate inputs before spending time on the build. Only this explicit list is copied.
$files = [ordered]@{
    'setup.cmd' = 'packaging/player/setup.cmd'
    'setup.ps1' = 'packaging/player/setup.ps1'
    'check-asio-drivers.ps1' = 'scripts/check-asio-drivers.ps1'
    'make-playlist.ps1' = 'scripts/make-playlist.ps1'
    'settings.example.ini' = 'settings.example.ini'
    'LICENSE' = 'LICENSE'
    'README.txt' = 'packaging/player/README.txt'
}
foreach ($relative in $files.Values) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) { throw ('Required file is missing: ' + $relative) }
}
& (Join-Path $PSScriptRoot 'verify.ps1') -Configuration Release

$executable = Join-Path $root 'out/x64/Release/wavplay.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'The verified Release executable was not produced.' }
$executableHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
$files.Add('wavplay.exe', 'out/x64/Release/wavplay.exe')
$parent = [IO.Path]::GetDirectoryName($output)
if ([string]::IsNullOrWhiteSpace($parent)) { throw 'Choose a new folder below an existing drive.' }
[IO.Directory]::CreateDirectory($parent) | Out-Null
$temporary = Join-Path $parent ('.player-build-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    foreach ($name in $files.Keys) {
        Copy-Item -LiteralPath (Join-Path $root $files[$name]) -Destination (Join-Path $temporary $name)
    }
    if ((Get-FileHash -LiteralPath (Join-Path $temporary 'wavplay.exe') -Algorithm SHA256).Hash -cne $executableHash) {
        throw 'The Release executable changed while preparing the transfer folder; build again into a new folder.'
    }
    [IO.Directory]::Move($temporary, $output)
} finally {
    $resolved = [IO.Path]::GetFullPath($temporary)
    $allowed = [IO.Path]::GetFullPath($parent).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolved).StartsWith('.player-build-'))) {
        throw 'Refusing to remove an unexpected build temporary directory.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
Write-Output ('Player folder: ' + $output)
Write-Output 'Copy the whole folder to your playback PC, then run setup.cmd there. No SDK or build tools are needed there.'
Write-Output 'This locally built folder is for your own PCs; the project publishes source only.'
