# Loaded by verify.ps1, which provides Get-CheckedOutput. Audits the evaluated
# ClCompile settings of both projects; MSBuild only evaluates them and builds
# nothing (docs/architecture.md, instruction set and optimization policy).
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

# Returns the violations of one evaluated ClCompile item of Configuration; At
# names the item in each of them.
function Test-CompileItem {
    param($Item, [string]$Configuration, $Policy, [string]$At)
    if ($Item.LanguageStandard -cne 'stdcpp23') {
        "LanguageStandard '$($Item.LanguageStandard)', not stdcpp23 (the C++23 ceiling), for $At"
    }
    $isBaseline = $Item.FullPath -match $Policy.Baseline
    $expectedSwitches = if ($isBaseline) { $Policy.BaselineSwitches } else { $Policy.Switches }
    if ((@(-split $Item.AdditionalOptions) -join ' ') -cne $expectedSwitches) {
        "AdditionalOptions '$($Item.AdditionalOptions)', not exactly '$expectedSwitches', for $At"
    }
    if (($Item.WholeProgramOptimization -ceq 'true') -ne ($Configuration -eq 'Release' -and -not $isBaseline)) {
        "WholeProgramOptimization '$($Item.WholeProgramOptimization)' for $At"
    }
    if ($Configuration -eq 'Release' -and $Item.FullPath -match $Policy.Product) {
        $level = if ($Item.FullPath -match $Policy.SizeOptimized) { 'MinSpace' } else { 'MaxSpeed' }
        if ($Item.Optimization -cne $level) {
            "Optimization '$($Item.Optimization)', not $level, for $At"
        }
    }
}

function Assert-BuildSettings {
    param([string]$MSBuild, [string[]]$Projects, [string]$SourceRoot)
    $policy = Get-BuildSettingsPolicy $SourceRoot
    $failures = [Collections.Generic.List[string]]::new()
    $counts = [Collections.Generic.List[string]]::new()
    foreach ($project in $Projects) {
        $projectName = Split-Path -Leaf $project
        foreach ($configuration in 'Release', 'Debug') {
            $evaluation = Get-CheckedOutput $MSBuild @($project, '/nologo', '/p:Platform=x64', "/p:Configuration=$configuration",
                '/getItem:ClCompile') | Out-String | ConvertFrom-Json
            $items = @($evaluation.Items.ClCompile)
            if (-not $items.Count) {
                $failures.Add("no ClCompile items in $project ($configuration)")
            }
            foreach ($item in $items) {
                foreach ($failure in Test-CompileItem $item $configuration $policy "$($item.FullPath) ($projectName, $configuration)") {
                    $failures.Add($failure)
                }
            }
            if ($configuration -eq 'Release') {
                $counts.Add(('{0} {1} sources, {2} for size' -f $projectName, $items.Count,
                    @($items | Where-Object { $_.Optimization -ceq 'MinSpace' }).Count))
            }
        }
    }
    if ($failures.Count) {
        throw "Build settings audit failed against docs/architecture.md (build/Common.props, build/WavplaySources.props):`n$($failures -join "`n")"
    }
    Write-Output "Build settings audit (Release and Debug): $($counts -join '; ')"
}
