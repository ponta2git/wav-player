# Builds, audits and tests the x64 configurations. The evaluated build settings
# of both projects are audited first. Each build profile is rebuilt once (with
# static analysis when -Analyze is given). The test suites run once per runtime
# they can tell apart: Release, and the Debug runtime checks either in Debug or,
# with -IncludeASan, in Debug-ASan, which adds the sanitizer to the same checks.
# wavplay_tests.exe runs the suites in parallel processes.
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string]$Configuration = 'All',
    [switch]$Analyze,
    [switch]$IncludeASan,
    [ValidateRange(1, 64)]
    [int]$Jobs = 0 # 0: wavplay_tests.exe chooses (half the logical processors, at most 4).
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Runs a tool whose output streams to the caller, such as a build or the tests.
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program returned exit code $LASTEXITCODE."
    }
}

# Runs a tool whose output the caller parses and returns that output. A failure
# would discard it with the caller's pipeline, and MSBuild and dumpbin print their
# diagnostics to standard output, so its end goes into the error.
function Get-CheckedOutput {
    param([string]$Program, [string[]]$Arguments)
    $output = @(& $Program @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "$Program returned exit code ${LASTEXITCODE}:`n$(@($output | Select-Object -Last 40) -join "`n")"
    }
    $output
}

# A rule that stopped rejecting its construct would leave verify green. So the
# header and disassembly audits also run on in-memory copies of their real input,
# each with one violation introduced, and must reject every copy for that
# violation. The build settings, manifest and version information audits have no
# such self-check.
function Assert-Rejected {
    param([string]$Audit, [string]$Case, [string[]]$Failures, [string]$Expected)
    if (-not @($Failures | Where-Object { $_.Contains($Expected) }).Count) {
        throw "$Audit self-check: $Case was not rejected for '$Expected' ($($Failures.Count) other findings: $(@($Failures | Select-Object -First 3) -join ' | '))."
    }
}

$root = Split-Path -Parent $PSScriptRoot
# These files define audits only; this entry point owns build/test orchestration.
. (Join-Path $PSScriptRoot 'verification/build-settings.ps1')
. (Join-Path $PSScriptRoot 'verification/release-image.ps1')
. (Join-Path $PSScriptRoot 'verification/realtime-code.ps1')
. (Join-Path $PSScriptRoot 'verification/startup-code.ps1')
& (Join-Path $PSScriptRoot 'check-project-layout.ps1')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$components = @('Microsoft.VisualStudio.Component.VC.Tools.x86.x64', 'Microsoft.VisualStudio.Component.Windows11SDK.26100')
if ($IncludeASan) {
    $components += 'Microsoft.VisualStudio.Component.VC.ASAN'
}
$install = & $vswhere -latest -products '*' -version '[18,19)' -requires $components -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $install) {
    throw 'Install the Visual Studio 2026 components listed in .vsconfig.'
}
$msbuild = Join-Path $install 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$solution = Join-Path $root 'WAV Player.sln'
$project = Join-Path $root 'src\app\wavplay.vcxproj'
$buildPaths = (Get-CheckedOutput $msbuild @($project, '/nologo', '/p:Platform=x64', '/p:Configuration=Release',
    '/getProperty:VCToolsInstallDir,WindowsSdkDir,WindowsTargetPlatformVersion') | Out-String | ConvertFrom-Json).Properties
$tools = Join-Path $buildPaths.VCToolsInstallDir 'bin\Hostx64\x64'
$dumpbin = Join-Path $tools 'dumpbin.exe'
$manifestTool = Join-Path $buildPaths.WindowsSdkDir "bin\$($buildPaths.WindowsTargetPlatformVersion)\x64\mt.exe"

$profiles = @($Configuration)
if ($Configuration -eq 'All') {
    $profiles = @('Debug', 'Release')
}
if ($IncludeASan) {
    $profiles += 'Debug-ASan'
}
# Debug-ASan runs the Debug runtime checks (iterator debugging, CRT reports)
# with the sanitizer, so with it the Debug profile is built and analyzed only.
$tested = @($profiles | Where-Object { -not ($_ -eq 'Debug' -and $IncludeASan) })
$testArguments = @()
if ($Jobs) {
    $testArguments = @('--jobs', "$Jobs")
}
$stages = [Collections.Generic.List[string]]::new()
$clock = [Diagnostics.Stopwatch]::StartNew()
function Complete-Stage([string]$Name) {
    $stages.Add(('{0,-24} {1,6:N0} s' -f $Name, $clock.Elapsed.TotalSeconds))
    $clock.Restart()
}

Assert-BuildSettings $msbuild @($project, (Join-Path $root 'tests\wavplay_tests\wavplay_tests.vcxproj')) (Join-Path $root 'src')
Complete-Stage 'Build settings'

foreach ($profile in $profiles) {
    $buildOptions = @($solution, '/nologo', '/verbosity:minimal', '/t:Rebuild', '/p:Platform=x64')
    if ($profile -eq 'Debug-ASan') {
        # Both projects clean and copy the same ASan runtime DLL in OutDir.
        # Serialize their Rebuilds so one Clean cannot race the other's copy;
        # /MP still compiles sources in parallel inside each project.
        $buildOptions += '/m:1', '/p:Configuration=Debug', '/p:EnableASAN=true'
    } else {
        $buildOptions += '/m', "/p:Configuration=$profile"
        if ($Analyze) {
            $buildOptions += '/p:RunCodeAnalysis=true', '/p:CodeAnalysisTreatWarningsAsErrors=true'
        }
    }
    Write-Output "== Build $profile|x64 =="
    Invoke-Checked $msbuild $buildOptions

    $outputDir = Join-Path $root "out\x64\$profile"
    foreach ($binary in 'wavplay.exe', 'wavplay_tests.exe') {
        $version = Assert-ApplicationManifest (Join-Path $outputDir $binary) $manifestTool
        if ($profile -eq 'Release' -and $binary -eq 'wavplay.exe') {
            Assert-VersionInfo (Join-Path $outputDir $binary) $version
        }
    }
    if ($profile -eq 'Release') {
        Assert-ReleaseImage (Join-Path $outputDir 'wavplay.exe') $dumpbin
        Assert-RealTimeCode (Join-Path $outputDir 'wavplay.exe') $dumpbin
        # Identify the audited binaries for local validation. The
        # hashes are printed, not asserted: other toolsets legitimately differ.
        Write-Output "Toolset: MSVC $(Split-Path -Leaf $buildPaths.VCToolsInstallDir.TrimEnd('\')), Windows SDK $($buildPaths.WindowsTargetPlatformVersion)"
        foreach ($binary in 'wavplay.exe', 'wavplay_tests.exe') {
            $hash = Get-FileHash -LiteralPath (Join-Path $outputDir $binary) -Algorithm SHA256
            Write-Output "SHA-256 $($hash.Hash)  $binary"
        }
    }
    Complete-Stage "Build $profile"

    if ($profile -notin $tested) {
        continue
    }
    Write-Output "== Test $profile|x64 =="
    $testExe = Join-Path $outputDir 'wavplay_tests.exe'
    if ($profile -eq 'Debug-ASan') {
        $imports = Get-CheckedOutput $dumpbin @('/nologo', '/dependents', $testExe)
        if (-not ($imports -match '^\s*clang_rt\.asan\w*_dynamic-x86_64\.dll\s*$')) {
            throw 'The ASan test executable does not import the x64 sanitizer runtime.'
        }
        $oldSymbolizer = $env:ASAN_SYMBOLIZER_PATH
        $oldOptions = $env:ASAN_OPTIONS
        try {
            $env:ASAN_SYMBOLIZER_PATH = Join-Path $tools 'llvm-symbolizer.exe'
            $env:ASAN_OPTIONS = 'halt_on_error=1'
            Invoke-Checked $testExe $testArguments
        } finally {
            $env:ASAN_SYMBOLIZER_PATH = $oldSymbolizer
            $env:ASAN_OPTIONS = $oldOptions
        }
    } else {
        Invoke-Checked $testExe $testArguments
    }
    Complete-Stage "Test $profile"
}

Write-Output '== Stage times =='
$stages | ForEach-Object { Write-Output $_ }
Write-Output "[PASS] Built $($profiles -join ', '); tested $($tested -join ', ')"
