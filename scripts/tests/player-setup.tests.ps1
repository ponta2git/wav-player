# Exercises setup with a fake registry query and an inert EXE; no driver or player is loaded.
# Each group below builds the fixtures it needs in the test directory, so groups
# do not depend on one another's results.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workParent = Join-Path $root 'out'
$workPrefix = 'setup-tests-'
$work = New-StagingDirectory $workParent $workPrefix
$utf8 = Get-TestEncoding
$device = '{A116D20A-4907-4F39-8231-A6701B01DBE1}'
$inertExecutable = 'inert setup test fixture; never execute'
$japanese = [string][char]0x65E5 + [char]0x672C
# The files of a player folder before setup has written any configuration.
$transferredFileCount = 5

# A module path as a PowerShell 7 session passes it on to its children: its own
# modules ahead of those of Windows PowerShell. Without PowerShell 7 the path
# names a directory that does not exist.
$foreignModulePath = Join-Path $work 'foreign PowerShell modules'
$coreModules = $null
$coreShell = Get-Command pwsh -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($coreShell) {
    $candidate = Join-Path (Split-Path -Parent $coreShell.Source) 'Modules'
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        $coreModules = $candidate
        $foreignModulePath = $coreModules + ';' + (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/Modules')
    }
}

# A music folder with files whose path order differs from their name order.
function New-MusicFolder([string]$Name) {
    $music = Join-Path $work $Name
    [void][IO.Directory]::CreateDirectory((Join-Path $music 'sub folder'))
    Write-TestFile (Join-Path $music '02.wav') 'fixture'
    Write-TestFile (Join-Path $music 'sub folder/01.wav') 'fixture'
    $music
}

# A transferred player folder: the real setup scripts, an inert executable and a
# driver inventory that reports one usable device without reading the registry.
function New-PlayerFolder([string]$Name) {
    $folder = Join-Path $work $Name
    [void][IO.Directory]::CreateDirectory($folder)
    Copy-Item -LiteralPath (Join-Path $root 'packaging/player/setup.ps1') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $root 'packaging/player/setup.cmd') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $root 'scripts/make-playlist.ps1') -Destination $folder
    Write-TestFile (Join-Path $folder 'wavplay.exe') $inertExecutable
    $query = "param([switch]`$Json)`r`n'{`"Drivers`": [{`"Description`": `"Test device`", `"CLSID`": `"$device`", `"LoadableByX64App`": true}]}'"
    Write-TestFile (Join-Path $folder 'check-asio-drivers.ps1') $query
    $folder
}

function Invoke-Setup([string]$Folder, [hashtable]$Options) {
    & (Join-Path $Folder 'setup.ps1') @Options -NoLaunch | Out-Null
}

# A player folder already configured for the test device and Music.
function New-ConfiguredPlayerFolder([string]$Name, [string]$Music) {
    $folder = New-PlayerFolder $Name
    Invoke-Setup $folder @{ DeviceClsid = $device; MusicDirectory = $Music }
    $folder
}

function Assert-Configured([string]$Folder, [string]$Music, [string]$Description) {
    $settings = [IO.File]::ReadAllText((Join-Path $Folder 'settings.ini'))
    Assert-True $settings.Contains('SelectedDevice=' + $device) ($Description + ' did not configure the selected device.')
    $expected = @(Get-ChildItem -LiteralPath $Music -File -Recurse -Filter '*.wav').Count
    $listed = [IO.File]::ReadAllLines((Join-Path $Folder 'playlist.txt'), $utf8).Count
    Assert-True ($listed -eq $expected) ($Description + ' did not list every WAV file in the playlist.')
}

function Assert-SetupSucceeded($Result, [string]$Message) {
    Assert-True ($Result.Code -eq 0 -and $Result.Error.Length -eq 0 -and $Result.Output.Contains('Setup complete.')) $Message
}

function Test-InitialSetup {
    $music = New-MusicFolder ('music ' + [char]0x97F3 + [char]0x697D)
    $folder = New-PlayerFolder ('player ' + $japanese)
    Push-Location $root
    try { Invoke-Setup $folder @{ DeviceClsid = $device; MusicDirectory = $music } } finally { Pop-Location }
    Assert-True ([IO.File]::ReadAllText((Join-Path $folder 'settings.ini')).Contains('SelectedDevice=' + $device)) 'Selected device was not written.'
    $paths = [IO.File]::ReadAllLines((Join-Path $folder 'playlist.txt'), $utf8)
    Assert-True ($paths.Count -eq 2 -and $paths[0] -ceq (Join-Path $music '02.wav') -and $paths[1] -ceq (Join-Path $music 'sub folder/01.wav')) 'Unicode playlist or ordering differs.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $folder 'wavplay.exe')) -ceq $inertExecutable) 'EXE fixture changed.'
    Complete-TestGroup 'Initial setup from a different working directory, Unicode and spaces, no launch'
}

function Test-ExistingConfigurationIsKept {
    $music = New-MusicFolder 'kept music'
    $folder = New-ConfiguredPlayerFolder 'kept player' $music
    $settings = Join-Path $folder 'settings.ini'
    $playlist = Join-Path $folder 'playlist.txt'
    $originalIni = [IO.File]::ReadAllBytes($settings)
    $originalList = [IO.File]::ReadAllBytes($playlist)
    Invoke-Setup $folder @{}
    Assert-SameBytes $settings $originalIni 'Existing settings changed.'
    Assert-SameBytes $playlist $originalList 'Existing playlist changed.'
    Assert-Fails { Invoke-Setup $folder @{ DeviceClsid = $device } } 'settings.ini already exists'
    Assert-Fails { Invoke-Setup $folder @{ MusicDirectory = $music } } 'playlist.txt already exists'
    Assert-SameBytes $settings $originalIni 'A refused device change altered the settings.'
    Assert-SameBytes $playlist $originalList 'A refused playlist change altered the playlist.'
    Complete-TestGroup 'Existing configuration remains byte-identical; replacement requires explicit intent'
}

function Test-PlaylistUpdate {
    $music = New-MusicFolder 'updated music'
    $folder = New-ConfiguredPlayerFolder 'updated player' $music
    $settings = Join-Path $folder 'settings.ini'
    $playlist = Join-Path $folder 'playlist.txt'
    $originalIni = [IO.File]::ReadAllBytes($settings)
    $originalList = [IO.File]::ReadAllBytes($playlist)
    $emptyMusic = Join-Path $work 'empty update music'
    [void][IO.Directory]::CreateDirectory($emptyMusic)
    Assert-Fails { Invoke-Setup $folder @{ MusicDirectory = $emptyMusic; UpdatePlaylist = $true } } 'No WAV files found'
    Assert-SameBytes $playlist $originalList 'Failed update changed the playlist.'
    Assert-True (@(Get-ChildItem -LiteralPath $folder -Force).Count -eq $transferredFileCount + 2) 'Failed update left temporary data in the player folder.'
    Write-TestFile (Join-Path $music '03.wav') 'fixture'
    Invoke-Setup $folder @{ MusicDirectory = $music; UpdatePlaylist = $true }
    $backups = @(Get-ChildItem -LiteralPath $folder -Filter 'playlist.txt.*.bak' -File)
    Assert-True ($backups.Count -eq 1) 'Explicit update did not preserve one backup.'
    Assert-SameBytes $backups[0].FullName $originalList 'Playlist backup differs.'
    Assert-True ([IO.File]::ReadAllLines($playlist, $utf8).Count -eq 3) 'Explicit update did not list the added file.'
    Assert-SameBytes $settings $originalIni 'Playlist update changed settings.'
    Complete-TestGroup 'Empty update is harmless; explicit update creates an exact backup'
}

function Test-IncompleteFolder {
    $music = New-MusicFolder 'incomplete music'
    $missing = New-PlayerFolder 'missing executable'
    Remove-Item -LiteralPath (Join-Path $missing 'wavplay.exe')
    Assert-Fails { Invoke-Setup $missing @{ DeviceClsid = $device; MusicDirectory = $music } } 'wavplay.exe is missing'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $missing 'settings.ini'))) 'Missing EXE wrote configuration.'
    $missing = New-PlayerFolder 'missing helper'
    Remove-Item -LiteralPath (Join-Path $missing 'make-playlist.ps1')
    Assert-Fails { Invoke-Setup $missing @{ DeviceClsid = $device; MusicDirectory = $music } } 'folder is incomplete'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $missing 'settings.ini'))) 'Missing helper wrote configuration.'
    Complete-TestGroup 'Incomplete transferred folders fail before configuration'
}

function Test-InvalidInput {
    $music = New-MusicFolder 'invalid input music'
    $emptyMusic = Join-Path $work 'empty music'
    [void][IO.Directory]::CreateDirectory($emptyMusic)
    $invalid = New-PlayerFolder 'invalid input'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid = 'invalid'; MusicDirectory = $music } } 'CLSID is invalid'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid = '{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}'; MusicDirectory = $music } } 'not a usable registered'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid = $device; MusicDirectory = '\\server\music' } } 'absolute local WAV folder'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid = $device; MusicDirectory = $emptyMusic } } 'No WAV files found'
    Write-TestFile (Join-Path $invalid 'check-asio-drivers.ps1') 'param([switch]$Json) ''{"Drivers": []}'''
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid = $device; MusicDirectory = $music } } 'No usable x64 ASIO'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $invalid 'settings.ini')) -and -not (Test-Path -LiteralPath (Join-Path $invalid 'playlist.txt'))) 'Rejected input wrote configuration.'
    Assert-True (@(Get-ChildItem -LiteralPath $invalid -Force).Count -eq $transferredFileCount) 'Rejected input left temporary data in the player folder.'
    Complete-TestGroup 'Invalid or absent drivers and invalid music leave configuration untouched'
}

function Test-BuildRefusesExistingOutput {
    $existingOutput = Join-Path $work 'existing output'
    [void][IO.Directory]::CreateDirectory($existingOutput)
    Write-TestFile (Join-Path $existingOutput 'sentinel.txt') 'keep'
    Assert-Fails { & (Join-Path $root 'scripts/build.ps1') -OutputDirectory $existingOutput } 'output folder already exists'
    Assert-True ([IO.File]::ReadAllText((Join-Path $existingOutput 'sentinel.txt')) -ceq 'keep') 'Build changed the existing output.'
    Complete-TestGroup 'Build refuses an existing transfer folder before building'
}

# Runs the real build.ps1 in a copy of the tree that has an inert SDK, an inert
# Release executable and a stand-in for verify.ps1.
function Test-Packaging {
    $buildFixture = Join-Path $work 'build fixture'
    foreach ($directory in @('scripts/support', 'packaging/player', 'external/asiosdk/common', 'out/x64/Release')) {
        [void][IO.Directory]::CreateDirectory((Join-Path $buildFixture $directory))
    }
    foreach ($relative in @('Directory.Build.props', 'scripts/build.ps1', 'scripts/support/staging.ps1', 'scripts/support/visual-studio.ps1',
        'scripts/fetch-asio-sdk.ps1', 'scripts/check-asio-drivers.ps1', 'scripts/make-playlist.ps1',
        'packaging/player/setup.cmd', 'packaging/player/setup.ps1', 'packaging/player/README.txt',
        'settings.example.ini', 'LICENSE')) {
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination (Join-Path $buildFixture $relative)
    }
    Write-TestFile (Join-Path $buildFixture 'external/asiosdk/common/iasiodrv.h') 'fixture SDK; never compile'
    Write-TestFile (Join-Path $buildFixture 'out/x64/Release/wavplay.exe') 'inert verified output fixture'
    $mockVerify = Join-Path $buildFixture 'scripts/verify.ps1'
    $transferParent = Join-Path $work 'transfer'
    $transfer = Join-Path $transferParent 'player'

    $props = Join-Path $buildFixture 'Directory.Build.props'
    $originalProps = [IO.File]::ReadAllText($props)
    Write-TestFile $props ($originalProps -replace '<WavplayMsvcVersion>[^<]+</WavplayMsvcVersion>',
        '<WavplayMsvcVersion>14.99.99999</WavplayMsvcVersion>')
    Assert-Fails { & (Join-Path $buildFixture 'scripts/build.ps1') -OutputDirectory $transfer } 'MSVC 14.99.99999'
    Assert-True (-not (Test-Path -LiteralPath $transfer)) 'A missing toolset produced a transfer folder.'
    Write-TestFile $props $originalProps

    Write-TestFile $mockVerify 'param([string]$Configuration) throw ''fixture verification failure'''
    Assert-Fails { & (Join-Path $buildFixture 'scripts/build.ps1') -OutputDirectory $transfer } 'fixture verification failure'
    Assert-True (-not (Test-Path -LiteralPath $transfer)) 'A failed verification produced a transfer folder.'

    Write-TestFile $mockVerify 'param([string]$Configuration) if ($Configuration -cne ''Release'') { throw ''Expected Release verification'' }'
    & (Join-Path $buildFixture 'scripts/build.ps1') -OutputDirectory $transfer | Out-Null
    $expected = @('LICENSE', 'README.txt', 'check-asio-drivers.ps1', 'make-playlist.ps1', 'settings.example.ini', 'setup.cmd', 'setup.ps1', 'wavplay.exe') | Sort-Object
    $actual = @(Get-ChildItem -LiteralPath $transfer -Recurse -File | ForEach-Object { $_.Name }) | Sort-Object
    Assert-True (-not @(Compare-Object $expected $actual).Count) 'The transfer folder contains unexpected or missing files.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $transfer 'wavplay.exe')) -ceq 'inert verified output fixture') 'The transfer EXE differs.'
    Assert-True (@(Get-ChildItem -LiteralPath $transferParent -Force).Count -eq 1) 'Packaging left temporary data beside the transfer folder.'
    Complete-TestGroup 'Failed verification blocks packaging; successful packaging copies only the player allowlist'
}

function Test-SdkFetch {
    $fetchFixture = Join-Path $work 'fetch fixture'
    [void][IO.Directory]::CreateDirectory((Join-Path $fetchFixture 'scripts/support'))
    Copy-Item -LiteralPath (Join-Path $root 'scripts/fetch-asio-sdk.ps1') -Destination (Join-Path $fetchFixture 'scripts')
    Copy-Item -LiteralPath (Join-Path $root 'scripts/support/staging.ps1') -Destination (Join-Path $fetchFixture 'scripts/support')
    $badArchive = Join-Path $fetchFixture 'not-the-official-sdk.zip'
    Write-TestFile $badArchive 'wrong archive fixture'
    Assert-Fails { & (Join-Path $fetchFixture 'scripts/fetch-asio-sdk.ps1') -ArchivePath $badArchive } 'SHA-256 does not match'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fetchFixture 'external/asiosdk'))) 'A rejected archive installed an SDK.'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $fetchFixture 'external') -Force).Count -eq 0) 'Fetch left temporary data.'
    [void][IO.Directory]::CreateDirectory((Join-Path $fetchFixture 'external/asiosdk'))
    Assert-Fails { & (Join-Path $fetchFixture 'scripts/fetch-asio-sdk.ps1') -ArchivePath $badArchive } 'already exists'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $fetchFixture 'external/asiosdk') -Force).Count -eq 0) 'Fetch changed the existing SDK directory.'
    Complete-TestGroup 'SDK fetch rejects an untrusted archive and never replaces an existing SDK'
}

function Test-CmdEntryPoint {
    $music = New-MusicFolder 'CMD music'
    $folder = New-ConfiguredPlayerFolder ('CMD player ' + $japanese) $music
    $result = Invoke-TestCmd -Entry (Join-Path $folder 'setup.cmd') -Arguments '-NoLaunch' -WorkingDirectory $root
    Assert-SetupSucceeded $result 'setup.cmd failed from a different working directory.'
    Complete-TestGroup 'The CMD entry point invokes Windows PowerShell without launching the player'
}

function Test-PlaybackOnlyEnvironment {
    $music = New-MusicFolder ('playback music ' + $japanese)
    $playbackOnly = New-PlayerFolder ('playback only ' + $japanese)
    # Model a playback PC: standard Windows tools only, no compiler/SDK discovery.
    $launch = @{
        FileName = Get-WindowsPowerShell
        Arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
            (Join-Path $playbackOnly 'setup.ps1') + '" -DeviceClsid ' + $device + ' -MusicDirectory "' + $music + '" -NoLaunch'
        WorkingDirectory = $work
        Environment = @{
            'PATH' = (Join-Path $env:SystemRoot 'System32') + ';' + (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0')
            'ProgramFiles(x86)' = Join-Path $work 'no developer installation'
        }
        RemoveEnvironment = @('VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir', 'WindowsSdkDir',
            'WindowsSDKVersion', 'VisualStudioVersion', 'VSCMD_VER', 'VCTargetsPath', 'INCLUDE', 'LIB', 'LIBPATH')
    }
    $result = Invoke-TestProcess @launch
    Assert-SetupSucceeded $result 'Setup depended on build-PC tools or environment.'
    Assert-Configured $playbackOnly $music 'Playback-only setup'
    Complete-TestGroup 'Initial setup works with Windows-only PATH and no compiler or SDK environment'
}

# build.cmd must hand build.ps1 the modules of Windows PowerShell even when a
# PowerShell 7 session started it. The stand-in build.ps1 uses the two module
# commands that the SDK download needs.
function Test-BuildCmdModules {
    $cmdBuild = Join-Path $work 'CMD build modules'
    [void][IO.Directory]::CreateDirectory((Join-Path $cmdBuild 'scripts'))
    [void][IO.Directory]::CreateDirectory((Join-Path $cmdBuild 'content'))
    Copy-Item -LiteralPath (Join-Path $root 'build.cmd') -Destination $cmdBuild
    Write-TestFile (Join-Path $cmdBuild 'content/probe.txt') 'Windows module fixture'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $cmdBuild 'content'), (Join-Path $cmdBuild 'probe.zip'))
    Write-TestFile (Join-Path $cmdBuild 'scripts/build.ps1') @'
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$archive = Join-Path $root 'probe.zip'
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($hash.Length -ne 64) { throw 'Missing SDK download hash command.' }
Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $root 'expanded')
if ([IO.File]::ReadAllText((Join-Path $root 'expanded/probe.txt')) -cne 'Windows module fixture') {
    throw 'Archive extraction differed.'
}
Write-Output 'WINDOWS_MODULES_OK'
'@
    $environment = @{ PSModulePath = $foreignModulePath }
    if ($coreModules) {
        # A negative control for the observed pwsh -> cmd -> Windows PowerShell failure.
        $legacy = [IO.File]::ReadAllText((Join-Path $cmdBuild 'build.cmd')) -replace '(?m)^set "PSModulePath=[^\r\n]*\r?\n', ''
        Write-TestFile (Join-Path $cmdBuild 'legacy-build.cmd') $legacy
        $result = Invoke-TestCmd -Entry (Join-Path $cmdBuild 'legacy-build.cmd') -WorkingDirectory $work -Environment $environment
        Assert-True ($result.Code -ne 0 -and $result.Error.Contains('Get-FileHash')) 'The legacy CMD did not reproduce the PowerShell module collision.'
    }
    $result = Invoke-TestCmd -Entry (Join-Path $cmdBuild 'build.cmd') -WorkingDirectory $work -Environment $environment
    Assert-True ($result.Code -eq 0 -and $result.Error.Length -eq 0 -and $result.Output.Contains('WINDOWS_MODULES_OK')) 'build.cmd could not load Windows hashing/archive modules with an inherited foreign PSModulePath.'
    Complete-TestGroup 'build.cmd restores Windows modules for hashing and ZIP extraction'
}

function Test-SetupCmdModules {
    $music = New-MusicFolder 'CMD first music'
    $cmdSetup = New-PlayerFolder ('CMD first setup ' + $japanese)
    $launch = @{
        Entry = Join-Path $cmdSetup 'setup.cmd'
        Arguments = '-DeviceClsid ' + $device + ' -MusicDirectory "' + $music + '" -NoLaunch'
        WorkingDirectory = $work
        Environment = @{ PSModulePath = $foreignModulePath }
    }
    $result = Invoke-TestCmd @launch
    Assert-SetupSucceeded $result 'setup.cmd failed initial configuration with an inherited foreign PSModulePath.'
    Assert-Configured $cmdSetup $music 'CMD initial setup'
    Complete-TestGroup 'setup.cmd completes first configuration with an inherited foreign PSModulePath'
}

try {
    Test-InitialSetup
    Test-ExistingConfigurationIsKept
    Test-PlaylistUpdate
    Test-IncompleteFolder
    Test-InvalidInput
    Test-BuildRefusesExistingOutput
    Test-Packaging
    Test-SdkFetch
    Test-CmdEntryPoint
    Test-PlaybackOnlyEnvironment
    Test-BuildCmdModules
    Test-SetupCmdModules
    Write-Output ('[PASS] ' + (Get-TestGroupCount) + ' setup/build safeguards; no player or ASIO driver was loaded.')
} finally {
    Remove-StagingDirectory $work $workParent $workPrefix
}
