# SPDX-License-Identifier: GPL-3.0-only
# Locates the Visual Studio installation that .vsconfig describes. This file
# defines functions only; the entry scripts dot-source it.

# Keep the pin in MSBuild so the same setting covers the IDE and command line.
function Get-RequiredMsvcVersion {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    [xml]$props = Get-Content -LiteralPath (Join-Path $root 'Directory.Build.props') -Raw
    $pin = $props.SelectSingleNode('/Project/PropertyGroup/WavplayMsvcVersion')
    if ($null -eq $pin -or $pin.InnerText -cnotmatch '\A14\.[0-9]{2}\.[0-9]{5}\z') {
        throw 'Directory.Build.props must specify a complete MSVC toolset version.'
    }
    $pin.InnerText
}

# Return the x64 tool paths only if all the requested files are present.
function Get-MsvcTools {
    param([string]$ToolsetDirectory, [switch]$IncludeASan)
    if ([string]::IsNullOrWhiteSpace($ToolsetDirectory)) { return }
    $folder = Split-Path -Leaf ($ToolsetDirectory.TrimEnd('\', '/'))
    if ($folder -cnotmatch '\A14\.([0-9]{2})\.[0-9]{5}\z') { return }
    $minorVersion = [int]$Matches[1]
    $bin = Join-Path $ToolsetDirectory 'bin\Hostx64\x64'
    $names = @{ Compiler = 'cl.exe'; Linker = 'link.exe'; Dumpbin = 'dumpbin.exe' }
    if ($IncludeASan) {
        $names.Symbolizer = 'llvm-symbolizer.exe'
        $names.ASanRuntime = 'clang_rt.asan_dynamic-x86_64.dll'
    }
    $paths = @{}
    foreach ($name in $names.Keys) {
        $path = Join-Path $bin $names[$name]
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return }
        $paths[$name] = $path
    }
    foreach ($name in 'Compiler', 'Linker', 'Dumpbin') {
        $file = (Get-Item -LiteralPath $paths[$name]).VersionInfo
        $majorVersion = if ($name -eq 'Compiler') { 19 } else { 14 }
        if ($file.FileMajorPart -ne $majorVersion -or $file.FileMinorPart -ne $minorVersion) { return }
        $paths["${name}Version"] = '{0}.{1}.{2}.{3}' -f $file.FileMajorPart, $file.FileMinorPart, $file.FileBuildPart, $file.FilePrivatePart
    }
    [pscustomobject]$paths
}

# Inspect the tool files. The versioned Installer component may not be registered
# when the pinned tools arrived via the current default component.
function Find-VisualStudio {
    param([switch]$IncludeASan, [switch]$AllowMissing)
    $version = Get-RequiredMsvcVersion
    $programFiles = ${env:ProgramFiles(x86)}
    $vswhere = if ($programFiles) { Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe' }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $instances = @(& $vswhere -products '*' -version '[18.12,19)' -requires Microsoft.VisualStudio.Component.Windows11SDK.26100 -property installationPath)
        if ($LASTEXITCODE -ne 0) { throw 'vswhere could not enumerate Visual Studio installations.' }
        foreach ($instance in $instances) {
            if ([string]::IsNullOrWhiteSpace($instance)) { continue }
            $msbuild = Join-Path $instance 'MSBuild\Current\Bin\amd64\MSBuild.exe'
            $toolset = Join-Path $instance "VC\Tools\MSVC\$version"
            if ((Test-Path -LiteralPath $msbuild -PathType Leaf) -and (Get-MsvcTools $toolset -IncludeASan:$IncludeASan)) {
                return $instance
            }
        }
    }
    if (-not $AllowMissing) {
        $asan = if ($IncludeASan) { ' and the x64 ASan tools' } else { '' }
        throw "Install Visual Studio 2026 (18.12 or later), MSVC $version, Windows SDK 10.0.26100.0$asan using .vsconfig."
    }
}
