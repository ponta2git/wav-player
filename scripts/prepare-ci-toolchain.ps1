# SPDX-License-Identifier: GPL-3.0-only
# A hosted image can change between the PR and main runs. Install the same
# toolset on either runner when the image does not already provide it.
[CmdletBinding()]
param([switch]$IncludeASan)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'support/visual-studio.ps1')

$required = Get-RequiredMsvcVersion
if (Find-VisualStudio -IncludeASan:$IncludeASan -AllowMissing) {
    Write-Output "MSVC $required is available."
    return
}
if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_ENVIRONMENT -cne 'github-hosted' -or
    [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    throw "Install MSVC $required using the repository .vsconfig. Automatic installation is for GitHub-hosted runners."
}

# The fixed Visual Studio 2026 Build Tools 18.12.1 bootstrapper is published at
# https://learn.microsoft.com/en-us/visualstudio/releases/2026/release-history.
# Update this package together with the pinned MSVC version.
$url = 'https://download.visualstudio.microsoft.com/download/pr/646c5a89-4d3a-4f43-985e-9c4c6771f304/6a3058c089e2564cac188178cf918d4cca106ea007f04c9f8bc52d7f5d7fe20f/vs_BuildTools.exe'
$sha256 = '6A3058C089E2564CAC188178CF918D4CCA106EA007F04C9F8BC52D7F5D7FE20F'
$bootstrapper = Join-Path $env:RUNNER_TEMP ("vs_BuildTools-{0}.exe" -f [guid]::NewGuid())
$installPath = Join-Path $env:RUNNER_TEMP 'wavplay-build-tools'
$arguments = @('--quiet', '--wait', '--norestart', '--nocache', '--installPath', ('"{0}"' -f $installPath),
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.VC.14.52.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100')
if ($IncludeASan) {
    $arguments += '--add', 'Microsoft.VisualStudio.Component.VC.ASAN'
}

Write-Output "Installing MSVC $required with Visual Studio 2026 Build Tools 18.12.1."
try {
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $bootstrapper -TimeoutSec 60
    } catch {
        throw 'Could not download the fixed Microsoft Visual Studio Build Tools bootstrapper.'
    }
    if ((Get-FileHash -LiteralPath $bootstrapper -Algorithm SHA256).Hash -cne $sha256) {
        throw 'The Visual Studio Build Tools bootstrapper SHA-256 did not match.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -cne 'Microsoft Corporation') {
        throw 'The Visual Studio Build Tools bootstrapper does not have a valid Microsoft signature.'
    }
    $process = Start-Process -FilePath $bootstrapper -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Visual Studio Build Tools installer failed (exit $($process.ExitCode))."
    }
    if (-not (Find-VisualStudio -IncludeASan:$IncludeASan -AllowMissing)) {
        throw "Visual Studio Build Tools installation finished without the required MSVC $required toolchain."
    }
    Write-Output "MSVC $required is available."
} finally {
    if (Test-Path -LiteralPath $bootstrapper -PathType Leaf) {
        Remove-Item -LiteralPath $bootstrapper
    }
}
