# Audits the evaluated ClCompile settings of both projects; MSBuild only evaluates
# them and builds nothing (docs/architecture.md, instruction set and optimization
# policy). Release optimizes the whole program except the isolated baseline startup gate,
# and the directory decides the level
# of a product source: /O1 (MinSpace) exactly under src\wavplay\ui, settings and
# playlist at any depth, /O2 (MaxSpeed) for every other one. Debug does not
# optimize the whole program. Both keep the C++23 ceiling. AdditionalOptions
# are exactly the shared switches, except for the two named baseline gate TUs. MSBuild
# passes AdditionalOptions after the switches of the metadata read here, and cl
# applies the last of conflicting switches, so any other option there (/arch:AVX512,
# /Od, /GL-, -std:c++latest, /Zc:preprocessor-) would override them unseen.
# /Zc:throwingNew and /Brepro have no macro a compile-time test could read.
function Assert-BuildSettings {
    param([string]$MSBuild, [string[]]$Projects, [string]$SourceRoot)
    $product = '^' + [regex]::Escape($SourceRoot.TrimEnd('\') + '\')
    $sizeOptimized = $product + 'wavplay\\(?:ui|settings|playlist)\\'
    $switches = '/utf-8 /Zc:__cplusplus /arch:AVX2 /Zc:throwingNew /Zc:preprocessor /Brepro'
    $baseline = $product + '(?:app\\startup\.cpp|wavplay\\core\\cpu_features\.cpp)$'
    $failures = [Collections.Generic.List[string]]::new()
    $counts = [Collections.Generic.List[string]]::new()
    foreach ($project in $Projects) {
        foreach ($configuration in 'Release', 'Debug') {
            $evaluation = Get-CheckedOutput $MSBuild @($project, '/nologo', '/p:Platform=x64', "/p:Configuration=$configuration",
                '/getItem:ClCompile') | Out-String | ConvertFrom-Json
            $items = @($evaluation.Items.ClCompile)
            if (-not $items.Count) {
                $failures.Add("no ClCompile items in $project ($configuration)")
            }
            foreach ($item in $items) {
                $at = "$($item.FullPath) ($(Split-Path -Leaf $project), $configuration)"
                if ($item.LanguageStandard -cne 'stdcpp23') {
                    $failures.Add("LanguageStandard '$($item.LanguageStandard)', not stdcpp23 (the C++23 ceiling), for $at")
                }
                $isBaseline = $item.FullPath -match $baseline
                $expectedSwitches = if ($isBaseline) { $switches.Replace('/arch:AVX2', '/arch:SSE4.2') } else { $switches }
                if ((@(-split $item.AdditionalOptions) -join ' ') -cne $expectedSwitches) {
                    $failures.Add("AdditionalOptions '$($item.AdditionalOptions)', not exactly '$expectedSwitches', for $at")
                }
                if (($item.WholeProgramOptimization -ceq 'true') -ne ($configuration -eq 'Release' -and -not $isBaseline)) {
                    $failures.Add("WholeProgramOptimization '$($item.WholeProgramOptimization)' for $at")
                }
                if ($configuration -eq 'Release' -and $item.FullPath -match $product) {
                    $level = if ($item.FullPath -match $sizeOptimized) { 'MinSpace' } else { 'MaxSpeed' }
                    if ($item.Optimization -cne $level) {
                        $failures.Add("Optimization '$($item.Optimization)', not $level, for $at")
                    }
                }
            }
            if ($configuration -eq 'Release') {
                $counts.Add(('{0} {1} sources, {2} for size' -f (Split-Path -Leaf $project), $items.Count,
                    @($items | Where-Object { $_.Optimization -ceq 'MinSpace' }).Count))
            }
        }
    }
    if ($failures.Count) {
        throw "Build settings audit failed against docs/architecture.md (build/Common.props, build/WavplaySources.props):`n$($failures -join "`n")"
    }
    Write-Output "Build settings audit (Release and Debug): $($counts -join '; ')"
}
