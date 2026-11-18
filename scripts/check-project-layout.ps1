# Checks that the tree is consistent with itself without building it: the
# Visual Studio registration of every source, the layer dependencies of
# docs/architecture.md, the SDK boundary, integer-only product text, the test
# suite table and its layering, the Solution Items, and the links between the
# documents. Each check is one function below; the first violation fails with
# the file it was found in. The policy the checks enforce is the data that
# follows; a deliberate design change edits it together with the documents.
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $root 'src'
$suiteDirectory = Join-Path $root 'tests\wavplay_tests\suites'

# Keep the dependency direction in architecture.md executable. This checks
# source includes; it does not claim to prove the runtime callback contract.
$layerDependencies = @{
    core = @('core')
    audio = @('core', 'audio')
    media = @('core', 'audio', 'media')
    stream = @('core', 'audio', 'media', 'stream')
    asio = @('core', 'audio', 'asio')
    settings = @('core', 'asio', 'settings')
    playlist = @('core', 'media', 'playlist')
    playback = @('core', 'audio', 'media', 'stream', 'asio', 'playback')
    control = @('core', 'asio', 'playback', 'settings', 'playlist', 'control')
    ui = @('core', 'asio', 'control', 'ui')
}
# src/app holds only the executable's entry point, which reaches playback through the ui layer.
$executableDependencies = @('core', 'ui')
# Layers whose other headers are implementation detail: other layers include only
# these. The real-time renderer, the request signal and the segment and request
# machinery stay private to playback.
$publicHeaders = @{
    playback = @('wavplay/playback/play_tracks.h', 'wavplay/playback/track_player.h', 'wavplay/playback/playback_types.h')
    control = @('wavplay/control/playback_controller.h')
}
# The Steinberg SDK enters the product only through this header.
$sdkHeader = 'wavplay/asio/asio_sdk.h'
$sdkHeaderFile = 'wavplay\asio\asio_sdk.h'
# Product text is written with integer code only (architecture.md, ui row):
# std::format, std::to_chars, the iostreams and locales bring large code whose
# generation differed between rebuilds. Lines are checked without // comments.
# <charconv> stays allowed for std::from_chars.
$formatting = '^\s*#\s*include\s*<(format|print|iostream|ostream|istream|fstream|sstream|syncstream|spanstream|strstream|iomanip|locale)>|\bstd::(v?format(_to(_n)?)?|formatted_size|print(ln)?|to_chars)\b'
# Tests are graded coarser than product code: suites of the lower layers stay
# free of the playback engine, controller, GUI and the playback runner fixture,
# and below asio they do not need the ASIO doubles either.
$lowerSuiteLayers = @('core', 'audio', 'media', 'stream', 'asio', 'settings', 'playlist')
$lowerSuiteForbidden = '^(wavplay/(playback|control|ui)/|fixtures/playback_fixture\.h$)'
# Development and distribution files that the solution lists as Solution Items:
# these files of the root and every file below these directories.
$supportRootFiles = @('.editorconfig', '.gitattributes', '.gitignore', '.vsconfig', 'AGENTS.md',
    'LICENSE', 'README.md', 'settings.example.ini', 'Directory.Build.props', 'build.cmd')
$supportDirectories = @('docs', 'build', 'scripts', '.github', 'packaging')

function Get-SourceFiles([string]$Directory) {
    Get-ChildItem -LiteralPath $Directory -Recurse -File |
        Where-Object { $_.Extension -in '.cpp', '.h', '.rc' } |
        ForEach-Object { $_.FullName }
}

# The sources that Manifest registers are exactly the source files below
# Directory. Prefix is the property reference that starts every Include of a
# shared list.
function Assert-RegisteredSources([string]$Manifest, [string]$Directory, [string]$Prefix = '') {
    [xml]$project = Get-Content -LiteralPath $Manifest -Raw
    $registered = @($project.SelectNodes('//*[local-name()="ClCompile" or local-name()="ClInclude" or local-name()="ResourceCompile"][@Include]') |
        ForEach-Object {
            $include = $_.GetAttribute('Include')
            if ($Prefix) {
                if (-not $include.StartsWith($Prefix, [StringComparison]::Ordinal)) {
                    throw "Unexpected source path in ${Manifest}: $include"
                }
                $include = $include.Substring($Prefix.Length)
            }
            [IO.Path]::GetFullPath((Join-Path $Directory $include))
        })
    $duplicates = @($registered | Group-Object | Where-Object Count -gt 1)
    if ($duplicates) { throw "Duplicate source registrations in ${Manifest}: $($duplicates.Name -join ', ')" }
    $difference = @(Compare-Object @(Get-SourceFiles $Directory) $registered)
    if ($difference) {
        throw "Source/Visual Studio registration mismatch in ${Manifest}: $($difference.InputObject -join ', ')"
    }
}

# The product sources are registered once, in the shared list that both projects
# import; each project registers the sources of its own directory.
function Assert-SourceRegistration {
    Assert-RegisteredSources (Join-Path $root 'build\WavplaySources.props') (Join-Path $sourceRoot 'wavplay') '$(WavplaySourceDir)\'
    foreach ($directory in 'src\app', 'tests\wavplay_tests') {
        $projectDirectory = Join-Path $root $directory
        $project = @(Get-ChildItem -LiteralPath $projectDirectory -Filter '*.vcxproj')
        if ($project.Count -ne 1) { throw "Expected one project in $projectDirectory" }
        Assert-RegisteredSources $project[0].FullName $projectDirectory
        [xml]$definition = Get-Content -LiteralPath $project[0].FullName -Raw
        $imports = @($definition.SelectNodes('//*[local-name()="Import"]') |
            Where-Object { $_.GetAttribute('Project') -eq '$(RepoRoot)build\WavplaySources.props' })
        if ($imports.Count -ne 1) { throw "$directory must import the shared product sources exactly once." }
    }
}

# One include of a product header: Source, a path below src with backslashes,
# may depend on the layer of Target ('wavplay/<layer>/<header>'), may see that
# header of the layer, and the header exists.
function Assert-ProductInclude([string]$Source, [string]$Target) {
    $isExecutable = $Source.StartsWith('app\', [StringComparison]::Ordinal)
    $layer = $Source.Split('\')[1]
    $targetLayer = $Target.Split('/')[1]
    if ($isExecutable) {
        if ($targetLayer -notin $executableDependencies) { throw "Forbidden executable dependency: $Source -> $Target" }
    } elseif ($targetLayer -notin $layerDependencies[$layer]) {
        throw "Forbidden layer dependency: $Source -> $Target"
    }
    if (($isExecutable -or $targetLayer -ne $layer) -and $publicHeaders.ContainsKey($targetLayer) -and
        $Target -notin $publicHeaders[$targetLayer]) {
        throw "Internal header of another layer: $Source -> $Target"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $Target) -PathType Leaf)) {
        throw "Missing product include: $Source -> $Target"
    }
}

# Checks every product source line by line: no formatting library, only allowed
# product includes, and the SDK headers only in the SDK boundary header. Returns
# the include graph: 'wavplay/ui/player_window.h' -> its 'wavplay/...' includes.
function Get-CheckedProductIncludes {
    $productIncludes = @{}
    foreach ($file in Get-SourceFiles $sourceRoot) {
        $relative = $file.Substring($sourceRoot.Length + 1)
        if (-not $relative.StartsWith('app\', [StringComparison]::Ordinal) -and
            -not $layerDependencies.ContainsKey($relative.Split('\')[1])) {
            throw "Undocumented product layer: $relative"
        }
        $edges = [Collections.Generic.List[string]]::new()
        $productIncludes[$relative.Replace('\', '/')] = $edges
        $number = 0
        foreach ($line in Get-Content -LiteralPath $file) {
            ++$number
            if (($line -replace '^\s*//.*$', '' -replace '\s//\s.*$', '') -match $formatting) {
                throw "Formatting library in product code (integer text only): ${relative}:${number}: $($Matches[0])"
            }
            if ($line -match '^\s*#include\s+[<"](wavplay/[^/]+/[^\">]+)[>"]') {
                $edges.Add($Matches[1])
                Assert-ProductInclude $relative $Matches[1]
            }
            if ($line -match '^\s*#include\s+[<"](?:asio|asiosys|iasiodrv)\.h[>"]' -and $relative -ne $sdkHeaderFile) {
                throw "Steinberg SDK include outside asio_sdk.h: $relative"
            }
        }
    }
    $productIncludes
}

# The GUI, the executable and the controller interface stay SDK-free through
# every product header they include, not only their direct includes. Angle
# includes of system and SDK headers are leaves; the SDK enters only through
# asio_sdk.h, which Get-CheckedProductIncludes checks.
function Assert-SdkFreeSources([hashtable]$ProductIncludes) {
    $sdkFreeRoots = @($ProductIncludes.Keys | Where-Object {
        $_.StartsWith('wavplay/ui/', [StringComparison]::Ordinal) -or $_.StartsWith('wavplay/settings/', [StringComparison]::Ordinal)
    }) + 'app/main.cpp' + 'wavplay/control/playback_controller.h'
    foreach ($start in $sdkFreeRoots | Sort-Object) {
        if (-not $ProductIncludes.ContainsKey($start)) { throw "Missing SDK-free source: $start" }
        $reachedFrom = @{ $start = $null }
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue($start)
        while ($pending.Count) {
            $current = $pending.Dequeue()
            if ($current -eq $sdkHeader) {
                $chain = [Collections.Generic.List[string]]::new()
                for ($node = $current; $node; $node = $reachedFrom[$node]) { $chain.Insert(0, $node) }
                throw "Steinberg SDK reaches SDK-free code: $($chain -join ' -> ')"
            }
            foreach ($next in $ProductIncludes[$current]) {
                if (-not $reachedFrom.ContainsKey($next)) {
                    $reachedFrom[$next] = $current
                    $pending.Enqueue($next)
                }
            }
        }
    }
}

# Every suites/<layer>/<name>_tests.cpp runs from exactly one table entry, and
# every entry names an existing suite file.
function Assert-SuiteTable {
    $suiteFiles = @(Get-ChildItem -LiteralPath $suiteDirectory -Recurse -File |
        Where-Object { $_.Name -like '*_tests.cpp' } |
        ForEach-Object { $_.Name.Substring(0, $_.Name.Length - '_tests.cpp'.Length) })
    $duplicateSuites = @($suiteFiles | Group-Object | Where-Object Count -gt 1)
    if ($duplicateSuites) { throw "Suite name used by more than one file: $($duplicateSuites.Name -join ', ')" }
    $suiteTable = Get-Content -LiteralPath (Join-Path $suiteDirectory 'suites.h') -Raw
    $suiteEntries = @([regex]::Matches($suiteTable, 'Suite\s*\{\s*L"([^"]*)"\s*,\s*run_(\w+)_tests\s*(?:,\s*SuitePriority::\w+\s*)?\}') |
        ForEach-Object {
            if ($_.Groups[1].Value -cne $_.Groups[2].Value) { throw "Suite entry name does not match its runner: $($_.Value)" }
            $_.Groups[1].Value
        })
    foreach ($name in $suiteFiles) {
        $count = @($suiteEntries | Where-Object { $_ -ceq $name }).Count
        if ($count -ne 1) { throw "Suite file needs exactly one suites.h entry, found ${count}: $name" }
    }
    foreach ($name in $suiteEntries) {
        if ($name -cnotin $suiteFiles) { throw "Suite table entry without a suite file: $name" }
    }
}

# The suites of the lower layers include nothing of the upper layers.
function Assert-SuiteLayering {
    foreach ($layer in $lowerSuiteLayers) {
        $directory = Join-Path $suiteDirectory $layer
        if (-not (Test-Path -LiteralPath $directory)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $directory -Recurse -File -Filter '*.cpp') {
            foreach ($line in Get-Content -LiteralPath $file.FullName) {
                if ($line -match '^\s*#include\s+"([^"]+)"') {
                    $target = $Matches[1]
                    if ($target -match $lowerSuiteForbidden -or ($layer -ne 'asio' -and $target.StartsWith('doubles/', [StringComparison]::Ordinal))) {
                        throw "Forbidden test dependency: suites\$layer\$($file.Name) -> $target"
                    }
                }
            }
        }
    }
}

# The solution lists every support file, and every listed item exists.
function Assert-SolutionItems {
    $solution = Get-Content -LiteralPath (Join-Path $root 'WAV Player.sln') -Raw
    $items = @([regex]::Matches($solution, '(?ms)^\s*ProjectSection\(SolutionItems\).*?^\s*EndProjectSection') |
        ForEach-Object {
            [regex]::Matches($_.Value, '(?m)^[\t ]*([^=\r\n]+?)[\t ]*=[\t ]*\1[\t ]*\r?$') |
                ForEach-Object { $_.Groups[1].Value.Trim() }
        })
    $supportFiles = @($supportRootFiles)
    foreach ($directory in $supportDirectories) {
        $supportFiles += Get-ChildItem -LiteralPath (Join-Path $root $directory) -Recurse -File |
            ForEach-Object { $_.FullName.Substring($root.Length + 1) }
    }
    foreach ($file in $supportFiles) {
        if ($file -notin $items) { throw "Missing Solution Item: $file" }
    }
    foreach ($item in $items) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $item) -PathType Leaf)) {
            throw "Solution Item does not exist: $item"
        }
    }
}

# Returns the fragments that name the headings of a Markdown file, as GitHub
# derives anchors: lower case, letters, digits, '_', '-' and spaces only, spaces
# as '-', and -1, -2, ... after repeats. Fenced code blocks have no headings.
function Get-HeadingFragments([string]$Path) {
    $fragments = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $repeats = @{}
    $fenced = $false
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -match '^```') { $fenced = -not $fenced; continue }
        if ($fenced -or $line -notmatch '^#{1,6}\s+(.*)$') { continue }
        $fragment = ($Matches[1].Trim().ToLowerInvariant() -replace '[^\p{L}\p{M}\p{N}\p{Pc} -]', '') -replace ' ', '-'
        if ($repeats.ContainsKey($fragment)) {
            $repeats[$fragment]++
            $fragment = "$fragment-$($repeats[$fragment])"
        } else {
            $repeats[$fragment] = 0
        }
        [void]$fragments.Add($fragment)
    }
    , $fragments
}

# Every relative link in README.md and docs/*.md names an existing file, and its
# optional fragment names a heading of that file.
function Assert-DocumentLinks {
    $headings = @{}
    $documents = @(Get-Item -LiteralPath (Join-Path $root 'README.md')) + @(Get-ChildItem -LiteralPath (Join-Path $root 'docs') -Filter '*.md' -File)
    foreach ($document in $documents) {
        $text = [IO.File]::ReadAllText($document.FullName)
        foreach ($link in [regex]::Matches($text, '\]\(<?([^#)\s>]*)(?:#([^)\s>]+))?>?\)')) {
            $target = [uri]::UnescapeDataString($link.Groups[1].Value)
            if ($target -match '^[a-z]+:') { continue }
            $fragment = [uri]::UnescapeDataString($link.Groups[2].Value)
            $path = if ($target) { [IO.Path]::GetFullPath((Join-Path $document.DirectoryName $target)) } else { $document.FullName }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "Link to a missing file: $($document.Name) -> $target#$fragment"
            }
            if ($fragment -and -not $headings.ContainsKey($path)) { $headings[$path] = Get-HeadingFragments $path }
            if ($fragment -and -not $headings[$path].Contains($fragment)) {
                throw "Link to a missing heading: $($document.Name) -> $target#$fragment"
            }
        }
    }
}

Assert-SourceRegistration
Assert-SdkFreeSources (Get-CheckedProductIncludes)
Assert-SuiteTable
Assert-SuiteLayering
Assert-SolutionItems
Assert-DocumentLinks
Write-Output '[PASS] Source registration, layer dependencies and public headers, SDK-free closures, integer-only product text, suite table and layering, Solution Items, document heading links'
