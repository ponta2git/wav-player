# SPDX-License-Identifier: GPL-3.0-only
# Locates the Visual Studio installation that .vsconfig describes. This file
# defines functions only; the entry scripts dot-source it.

# Returns the installation path of Visual Studio 2026 with the C++ tools, the
# pinned Windows SDK and any additional components, or fails naming what to install.
function Find-VisualStudio {
    param([string[]]$AdditionalComponents = @())
    $programFiles = ${env:ProgramFiles(x86)}
    $vswhere = if ($programFiles) { Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe' }
    if (-not $vswhere -or -not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio was not found. Install Visual Studio 2026 with the repository .vsconfig.'
    }
    $components = @('Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        'Microsoft.VisualStudio.Component.Windows11SDK.26100') + $AdditionalComponents
    $found = @(& $vswhere -latest -products '*' -version '[18,19)' -requires $components -property installationPath)
    if ($LASTEXITCODE -ne 0 -or -not $found.Count -or [string]::IsNullOrWhiteSpace($found[0])) {
        throw "Install the Visual Studio 2026 components listed in .vsconfig. Required here: $($components -join ', ')"
    }
    ([string]$found[0]).Trim()
}
