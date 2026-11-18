# Builds, audits and tests the x64 configurations. The evaluated build settings
# are audited first. Each build profile is rebuilt once (with
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
    # 0: wavplay_tests.exe chooses (half the logical processors, at most 4).
    [ValidateRange(0, 64)]
    [int]$Jobs = 0,
    # The two full CI workers audit the profiles they build; Release owns the shared layout check.
    [switch]$CiWorker,
    # Requires an isolated Windows logon session with no running product instance.
    [switch]$TestReleaseStartup
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

# These files define functions only; this entry point owns build/test
# orchestration and the exit-code checks above, which the audits call.
. (Join-Path $PSScriptRoot 'support/visual-studio.ps1')
. (Join-Path $PSScriptRoot 'verification/build-settings.ps1')
. (Join-Path $PSScriptRoot 'verification/release-image.ps1')
. (Join-Path $PSScriptRoot 'verification/disassembly.ps1')
. (Join-Path $PSScriptRoot 'verification/realtime-code.ps1')
. (Join-Path $PSScriptRoot 'verification/startup-code.ps1')

# The tools of the toolset and Windows SDK that MSBuild selects for the product.
function Get-BuildTools {
    param([string]$Installation, [string]$Project)
    $msbuild = Join-Path $Installation 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    $properties = (Get-CheckedOutput $msbuild @($Project, '/nologo', '/p:Platform=x64', '/p:Configuration=Release',
        '/getProperty:VCToolsInstallDir,WindowsSdkDir,WindowsTargetPlatformVersion') | Out-String | ConvertFrom-Json).Properties
    $bin = Join-Path $properties.VCToolsInstallDir 'bin\Hostx64\x64'
    [pscustomobject]@{
        MSBuild = $msbuild
        Dumpbin = Join-Path $bin 'dumpbin.exe'
        Symbolizer = Join-Path $bin 'llvm-symbolizer.exe'
        ManifestTool = Join-Path $properties.WindowsSdkDir "bin\$($properties.WindowsTargetPlatformVersion)\x64\mt.exe"
        ToolsVersion = Split-Path -Leaf $properties.VCToolsInstallDir.TrimEnd('\')
        SdkVersion = $properties.WindowsTargetPlatformVersion
    }
}

# The MSBuild arguments that rebuild one profile of the solution.
function Get-RebuildArguments {
    param([string]$Solution, [string]$BuildProfile, [bool]$Analyze)
    $arguments = @($Solution, '/nologo', '/verbosity:minimal', '/t:Rebuild', '/p:Platform=x64')
    if ($BuildProfile -eq 'Debug-ASan') {
        # Both projects clean and copy the same ASan runtime DLL in OutDir.
        # Serialize their Rebuilds so one Clean cannot race the other's copy;
        # /MP still compiles sources in parallel inside each project.
        return $arguments + @('/m:1', '/p:Configuration=Debug', '/p:EnableASAN=true')
    }
    $arguments += '/m', "/p:Configuration=$BuildProfile"
    if ($Analyze) {
        $arguments += '/p:RunCodeAnalysis=true', '/p:CodeAnalysisTreatWarningsAsErrors=true'
    }
    $arguments
}

# Audits the binaries of one built profile. Every profile embeds the application
# manifest; the Release product also carries the version information and passes
# the image and machine code audits.
function Assert-BuildOutput {
    param([string]$OutputDirectory, [string]$BuildProfile, $Tools)
    $product = Join-Path $OutputDirectory 'wavplay.exe'
    $binaries = @($product, (Join-Path $OutputDirectory 'wavplay_tests.exe'))
    foreach ($binary in $binaries) {
        $version = Assert-ApplicationManifest $binary $Tools.ManifestTool
        if ($BuildProfile -eq 'Release' -and $binary -eq $product) {
            Assert-VersionInfo $binary $version
        }
    }
    if ($BuildProfile -ne 'Release') {
        return
    }
    Assert-ReleaseImage $product $Tools.Dumpbin
    Assert-RealTimeCode $product $Tools.Dumpbin
    # Identify the audited binaries for local validation. The
    # hashes are printed, not asserted: other toolsets legitimately differ.
    Write-Output "Toolset: MSVC $($Tools.ToolsVersion), Windows SDK $($Tools.SdkVersion)"
    foreach ($binary in $binaries) {
        $hash = Get-FileHash -LiteralPath $binary -Algorithm SHA256
        Write-Output "SHA-256 $($hash.Hash)  $(Split-Path -Leaf $binary)"
    }
}

# Runs the test executable of one profile. The sanitized profile must really
# load the sanitizer runtime, and runs with its options for this call only.
function Invoke-TestExecutable {
    param([string]$TestExecutable, [string[]]$Arguments, [string]$BuildProfile, $Tools)
    if ($BuildProfile -ne 'Debug-ASan') {
        Invoke-Checked $TestExecutable $Arguments
        return
    }
    $imports = @(Get-CheckedOutput $Tools.Dumpbin @('/nologo', '/dependents', $TestExecutable))
    if (-not @($imports -match '^\s*clang_rt\.asan\w*_dynamic-x86_64\.dll\s*$').Count) {
        throw 'The ASan test executable does not import the x64 sanitizer runtime.'
    }
    $oldSymbolizer = $env:ASAN_SYMBOLIZER_PATH
    $oldOptions = $env:ASAN_OPTIONS
    try {
        $env:ASAN_SYMBOLIZER_PATH = $Tools.Symbolizer
        $env:ASAN_OPTIONS = 'halt_on_error=1'
        Invoke-Checked $TestExecutable $Arguments
    } finally {
        $env:ASAN_SYMBOLIZER_PATH = $oldSymbolizer
        $env:ASAN_OPTIONS = $oldOptions
    }
}

$clock = [Diagnostics.Stopwatch]::new()
# ci.ps1 publishes only these fixed stage names and integer durations.
function Complete-Stage([string]$Name) {
    Write-Output ('TIMING {0} {1} ms' -f $Name, $script:clock.ElapsedMilliseconds)
    $script:clock.Restart()
}

$root = Split-Path -Parent $PSScriptRoot
if ($TestReleaseStartup -and $Configuration -eq 'Debug') {
    throw 'The product startup test requires a Release build.'
}
if (-not $CiWorker -or $Configuration -ne 'Debug') {
    & (Join-Path $PSScriptRoot 'check-project-layout.ps1')
}
$sanitizerComponents = @()
if ($IncludeASan) {
    $sanitizerComponents += 'Microsoft.VisualStudio.Component.VC.ASAN'
}
$solution = Join-Path $root 'WAV Player.sln'
$project = Join-Path $root 'src\app\wavplay.vcxproj'
$tools = Get-BuildTools (Find-VisualStudio $sanitizerComponents) $project

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

$projects = @($project, (Join-Path $root 'src\wavplay\wavplay_core.vcxproj'),
    (Join-Path $root 'tests\wavplay_tests\wavplay_tests.vcxproj'))
$auditProfiles = @('Debug', 'Release')
if ($CiWorker) {
    $auditProfiles = $profiles
} elseif ($IncludeASan) {
    $auditProfiles += 'Debug-ASan'
}
$clock.Start()
Assert-BuildSettings $tools.MSBuild $projects (Join-Path $root 'src') $auditProfiles
Complete-Stage 'Settings'

foreach ($buildProfile in $profiles) {
    Write-Output "== Build $buildProfile|x64 =="
    Invoke-Checked $tools.MSBuild (Get-RebuildArguments $solution $buildProfile $Analyze)
    Complete-Stage "Build-$buildProfile"
    $outputDirectory = Join-Path $root "out\x64\$buildProfile"
    Assert-BuildOutput $outputDirectory $buildProfile $tools
    Complete-Stage "Audit-$buildProfile"

    if ($buildProfile -notin $tested) {
        continue
    }
    Write-Output "== Test $buildProfile|x64 =="
    $testExecutable = Join-Path $outputDirectory 'wavplay_tests.exe'
    Invoke-TestExecutable $testExecutable $testArguments $buildProfile $tools
    Complete-Stage "Test-$buildProfile"
    if ($TestReleaseStartup -and $buildProfile -eq 'Release') {
        Invoke-Checked $testExecutable @('--test-product-startup', (Join-Path $outputDirectory 'wavplay.exe'))
        Complete-Stage 'Release-Startup'
    }
}

Write-Output "[PASS] Built $($profiles -join ', '); tested $($tested -join ', ')"
