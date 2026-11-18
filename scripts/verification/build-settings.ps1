# Loaded by verify.ps1, which provides Get-CheckedOutput. Audits the evaluated
# ClCompile settings of the library and both executables. MSBuild evaluates
# each selected profile without building (docs/architecture.md).
# Release optimizes the whole program except the isolated baseline startup gate,
# and the directory decides the level of a product source: /O1 (MinSpace)
# exactly under src\wavplay\ui, settings and playlist at any depth, /O2
# (MaxSpeed) for every other one. Debug does not optimize the whole program.
# Both keep the C++23 ceiling. AdditionalOptions are exactly the shared
# switches, except for the two named baseline gate TUs. MSBuild passes
# AdditionalOptions after the switches of the metadata read here, and cl applies
# the last of conflicting switches, so any other option there (/arch:AVX512,
# /Od, /GL-, -std:c++latest, /Zc:preprocessor-) would override them unseen.
# /Zc:throwingNew and /Brepro have no macro a compile-time test could read.

# The expected settings as patterns on the full path of a source below SourceRoot.
function Get-BuildSettingsPolicy {
    param([string]$SourceRoot)
    $product = '^' + [regex]::Escape($SourceRoot.TrimEnd('\') + '\')
    $switches = '/utf-8 /Zc:__cplusplus /arch:AVX2 /Zc:throwingNew /Zc:preprocessor /Brepro'
    [pscustomobject]@{
        Product = $product
        SizeOptimized = $product + 'wavplay\\(?:ui|settings|playlist)\\'
        Baseline = $product + '(?:app\\startup\.cpp|wavplay\\core\\cpu_features\.cpp)$'
        Switches = $switches
        BaselineSwitches = $switches.Replace('/arch:AVX2', '/arch:SSE4.2')
    }
}

# Returns the violations of one evaluated ClCompile item for the selected profile.
function Test-CompileItem {
    param($Item, [string]$BuildProfile, $Policy, [string]$At)
    $release = $BuildProfile -eq 'Release'
    $runtime = if ($release) { 'MultiThreaded' } else { 'MultiThreadedDebug' }
    if ($Item.RuntimeLibrary -cne $runtime) {
        "RuntimeLibrary '$($Item.RuntimeLibrary)', not $runtime, for $At"
    }
    if (($Item.EnableASAN -eq 'true') -ne ($BuildProfile -eq 'Debug-ASan')) {
        "EnableASAN '$($Item.EnableASAN)' for $At"
    }
    if ($Item.LanguageStandard -cne 'stdcpp23') {
        "LanguageStandard '$($Item.LanguageStandard)', not stdcpp23 (the C++23 ceiling), for $At"
    }
    $isBaseline = $Item.FullPath -match $Policy.Baseline
    $expectedSwitches = if ($isBaseline) { $Policy.BaselineSwitches } else { $Policy.Switches }
    if ((@(-split $Item.AdditionalOptions) -join ' ') -cne $expectedSwitches) {
        "AdditionalOptions '$($Item.AdditionalOptions)', not exactly '$expectedSwitches', for $At"
    }
    if (($Item.WholeProgramOptimization -ceq 'true') -ne ($release -and -not $isBaseline)) {
        "WholeProgramOptimization '$($Item.WholeProgramOptimization)' for $At"
    }
    if ($release -and $Item.FullPath -match $Policy.Product) {
        $level = if ($Item.FullPath -match $Policy.SizeOptimized) { 'MinSpace' } else { 'MaxSpeed' }
        if ($Item.Optimization -cne $level) {
            "Optimization '$($Item.Optimization)', not $level, for $At"
        }
    }
}

function Assert-BuildSettings {
    param([string]$MSBuild, [string[]]$Projects, [string]$SourceRoot,
        [ValidateNotNullOrEmpty()][ValidateSet('Debug', 'Release', 'Debug-ASan')][string[]]$BuildProfiles = @('Release', 'Debug'))
    $policy = Get-BuildSettingsPolicy $SourceRoot
    $failures = [Collections.Generic.List[string]]::new()
    $counts = [Collections.Generic.List[string]]::new()
    foreach ($project in $Projects) {
        $projectName = Split-Path -Leaf $project
        foreach ($profile in $BuildProfiles) {
            $configuration = if ($profile -eq 'Debug-ASan') { 'Debug' } else { $profile }
            $arguments = @($project, '/nologo', '/p:Platform=x64', "/p:Configuration=$configuration", '/getItem:ClCompile')
            if ($profile -eq 'Debug-ASan') { $arguments += '/p:EnableASAN=true' }
            $evaluation = Get-CheckedOutput $MSBuild $arguments | Out-String | ConvertFrom-Json
            $items = @($evaluation.Items.ClCompile)
            if (-not $items.Count) {
                $failures.Add("no ClCompile items in $project ($profile)")
            }
            foreach ($item in $items) {
                foreach ($failure in Test-CompileItem $item $profile $policy "$($item.FullPath) ($projectName, $profile)") {
                    $failures.Add($failure)
                }
            }
            $counts.Add(('{0} {1} {2} sources' -f $projectName, $profile, $items.Count))
        }
    }
    if ($failures.Count) {
        throw "Build settings audit failed against docs/architecture.md (build/Common.props, build/WavplaySources.props):`n$($failures -join "`n")"
    }
    Write-Output "Build settings audit: $($counts -join '; ')"
}
