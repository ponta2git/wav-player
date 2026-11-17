# Exercises setup with a fake registry query and an inert EXE; no driver or player is loaded.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $root ('out/setup-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$device = '{A116D20A-4907-4F39-8231-A6701B01DBE1}'
$passed = 0
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

function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }
function Assert-True([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function New-Fixture([string]$Name) {
    $folder = Join-Path $work $Name
    [IO.Directory]::CreateDirectory($folder) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'packaging/player/setup.ps1') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $root 'packaging/player/setup.cmd') -Destination $folder
    Copy-Item -LiteralPath (Join-Path $root 'scripts/make-playlist.ps1') -Destination $folder
    Write-Text (Join-Path $folder 'wavplay.exe') 'inert setup test fixture; never execute'
    $query = "param([switch]`$Json)`r`n'{`"Drivers`": [{`"Description`": `"Test device`", `"CLSID`": `"$device`", `"LoadableByX64App`": true}]}'"
    Write-Text (Join-Path $folder 'check-asio-drivers.ps1') $query
    return $folder
}
function Invoke-Setup([string]$Folder, [hashtable]$Options) {
    & (Join-Path $Folder 'setup.ps1') @Options -NoLaunch | Out-Null
}
function Assert-Fails([scriptblock]$Action, [string]$Text) {
    $failed = $false
    try { & $Action } catch { $failed = $true; if (-not $_.Exception.Message.Contains($Text)) { throw } }
    Assert-True $failed ('Expected failure: ' + $Text)
}
function Pass([string]$Name) { $script:passed++; Write-Output ('PASS ' + $Name) }
function Invoke-CmdWithForeignModulePath([string]$Entry, [string]$Arguments) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /s /c ""' + $Entry + '" ' + $Arguments + ' < NUL"'
    $start.WorkingDirectory = $work
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # ProcessStartInfo does not apply pwsh's compatibility fix for Windows PowerShell.
    $start.EnvironmentVariables['PSModulePath'] = $foreignModulePath
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd()
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        return [PSCustomObject]@{ Code=$process.ExitCode; Output=$output; Error=$errorText }
    } finally { $process.Dispose() }
}

try {
    $music = Join-Path $work ('music ' + [char]0x97F3 + [char]0x697D)
    [IO.Directory]::CreateDirectory((Join-Path $music 'sub folder')) | Out-Null
    Write-Text (Join-Path $music '02.wav') 'fixture'
    Write-Text (Join-Path $music 'sub folder/01.wav') 'fixture'
    $folder = New-Fixture ('player ' + [char]0x65E5 + [char]0x672C)
    $settings = Join-Path $folder 'settings.ini'
    $playlist = Join-Path $folder 'playlist.txt'
    Push-Location $root
    try { Invoke-Setup $folder @{ DeviceClsid=$device; MusicDirectory=$music } } finally { Pop-Location }
    Assert-True ([IO.File]::ReadAllText($settings).Contains('SelectedDevice=' + $device)) 'Selected device was not written.'
    $paths = [IO.File]::ReadAllLines($playlist, $utf8)
    Assert-True ($paths.Count -eq 2 -and $paths[0] -eq (Join-Path $music '02.wav') -and $paths[1] -eq (Join-Path $music 'sub folder/01.wav')) 'Unicode playlist or ordering differs.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $folder 'wavplay.exe')) -eq 'inert setup test fixture; never execute') 'EXE fixture changed.'
    Pass 'Initial setup from a different working directory, Unicode and spaces, no launch'

    $originalIni = [IO.File]::ReadAllBytes($settings)
    $originalList = [IO.File]::ReadAllBytes($playlist)
    Invoke-Setup $folder @{}
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($settings)) -eq [Convert]::ToBase64String($originalIni)) 'Existing settings changed.'
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($playlist)) -eq [Convert]::ToBase64String($originalList)) 'Existing playlist changed.'
    Assert-Fails { Invoke-Setup $folder @{ DeviceClsid=$device } } 'settings.ini already exists'
    Assert-Fails { Invoke-Setup $folder @{ MusicDirectory=$music } } 'playlist.txt already exists'
    Pass 'Existing configuration remains byte-identical; replacement requires explicit intent'

    $emptyMusic = Join-Path $work 'empty music'
    [IO.Directory]::CreateDirectory($emptyMusic) | Out-Null
    Assert-Fails { Invoke-Setup $folder @{ MusicDirectory=$emptyMusic; UpdatePlaylist=$true } } 'No WAV files found'
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($playlist)) -eq [Convert]::ToBase64String($originalList)) 'Failed update changed the playlist.'
    Write-Text (Join-Path $music '03.wav') 'fixture'
    Invoke-Setup $folder @{ MusicDirectory=$music; UpdatePlaylist=$true }
    $backups = @(Get-ChildItem -LiteralPath $folder -Filter 'playlist.txt.*.bak' -File)
    Assert-True ($backups.Count -eq 1) 'Explicit update did not preserve one backup.'
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backups[0].FullName)) -eq [Convert]::ToBase64String($originalList)) 'Playlist backup differs.'
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($settings)) -eq [Convert]::ToBase64String($originalIni)) 'Playlist update changed settings.'
    Pass 'Empty update is harmless; explicit update creates an exact backup'

    $missing = New-Fixture 'missing executable'
    Remove-Item -LiteralPath (Join-Path $missing 'wavplay.exe')
    Assert-Fails { Invoke-Setup $missing @{ DeviceClsid=$device; MusicDirectory=$music } } 'wavplay.exe is missing'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $missing 'settings.ini'))) 'Missing EXE wrote configuration.'
    $missing = New-Fixture 'missing helper'
    Remove-Item -LiteralPath (Join-Path $missing 'make-playlist.ps1')
    Assert-Fails { Invoke-Setup $missing @{ DeviceClsid=$device; MusicDirectory=$music } } 'folder is incomplete'
    Pass 'Incomplete transferred folders fail before configuration'

    $invalid = New-Fixture 'invalid input'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid='invalid'; MusicDirectory=$music } } 'CLSID is invalid'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid='{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}'; MusicDirectory=$music } } 'not a usable registered'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid=$device; MusicDirectory='\\server\music' } } 'absolute local WAV folder'
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid=$device; MusicDirectory=$emptyMusic } } 'No WAV files found'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $invalid 'settings.ini')) -and -not (Test-Path -LiteralPath (Join-Path $invalid 'playlist.txt'))) 'Rejected input wrote configuration.'
    Write-Text (Join-Path $invalid 'check-asio-drivers.ps1') 'param([switch]$Json) ''{"Drivers": []}'''
    Assert-Fails { Invoke-Setup $invalid @{ DeviceClsid=$device; MusicDirectory=$music } } 'No usable x64 ASIO'
    Pass 'Invalid or absent drivers and invalid music leave configuration untouched'

    $existingOutput = Join-Path $work 'existing output'
    [IO.Directory]::CreateDirectory($existingOutput) | Out-Null
    Write-Text (Join-Path $existingOutput 'sentinel.txt') 'keep'
    Assert-Fails { & (Join-Path $root 'scripts/build.ps1') -OutputDirectory $existingOutput } 'output folder already exists'
    Assert-True ([IO.File]::ReadAllText((Join-Path $existingOutput 'sentinel.txt')) -eq 'keep') 'Build changed the existing output.'
    Pass 'Build refuses an existing transfer folder before building'

    $buildFixture = Join-Path $work 'build fixture'
    foreach ($directory in @('scripts', 'packaging/player', 'external/asiosdk/common', 'out/x64/Release')) {
        [IO.Directory]::CreateDirectory((Join-Path $buildFixture $directory)) | Out-Null
    }
    foreach ($relative in @('scripts/build.ps1', 'scripts/fetch-asio-sdk.ps1', 'scripts/check-asio-drivers.ps1',
        'scripts/make-playlist.ps1', 'packaging/player/setup.cmd', 'packaging/player/setup.ps1',
        'packaging/player/README.txt', 'settings.example.ini', 'LICENSE')) {
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination (Join-Path $buildFixture $relative)
    }
    Write-Text (Join-Path $buildFixture 'external/asiosdk/common/iasiodrv.h') 'fixture SDK; never compile'
    Write-Text (Join-Path $buildFixture 'out/x64/Release/wavplay.exe') 'inert verified output fixture'
    $mockVerify = Join-Path $buildFixture 'scripts/verify.ps1'
    Write-Text $mockVerify 'param([string]$Configuration) throw ''fixture verification failure'''
    $transfer = Join-Path $work 'transfer output'
    Assert-Fails { & (Join-Path $buildFixture 'scripts/build.ps1') -OutputDirectory $transfer } 'fixture verification failure'
    Assert-True (-not (Test-Path -LiteralPath $transfer)) 'A failed verification produced a transfer folder.'
    Write-Text $mockVerify 'param([string]$Configuration) if ($Configuration -cne ''Release'') { throw ''Expected Release verification'' }'
    & (Join-Path $buildFixture 'scripts/build.ps1') -OutputDirectory $transfer | Out-Null
    $expected = @('LICENSE', 'README.txt', 'check-asio-drivers.ps1', 'make-playlist.ps1', 'settings.example.ini', 'setup.cmd', 'setup.ps1', 'wavplay.exe') | Sort-Object
    $actual = @(Get-ChildItem -LiteralPath $transfer -Recurse -File | ForEach-Object { $_.Name }) | Sort-Object
    Assert-True (-not @(Compare-Object $expected $actual).Count) 'The transfer folder contains unexpected or missing files.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $transfer 'wavplay.exe')) -eq 'inert verified output fixture') 'The transfer EXE differs.'
    Pass 'Failed verification blocks packaging; successful packaging copies only the player allowlist'

    $fetchFixture = Join-Path $work 'fetch fixture'
    [IO.Directory]::CreateDirectory((Join-Path $fetchFixture 'scripts')) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'scripts/fetch-asio-sdk.ps1') -Destination (Join-Path $fetchFixture 'scripts')
    $badArchive = Join-Path $fetchFixture 'not-the-official-sdk.zip'
    Write-Text $badArchive 'wrong archive fixture'
    Assert-Fails { & (Join-Path $fetchFixture 'scripts/fetch-asio-sdk.ps1') -ArchivePath $badArchive } 'SHA-256 does not match'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fetchFixture 'external/asiosdk'))) 'A rejected archive installed an SDK.'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $fetchFixture 'external') -Force).Count -eq 0) 'Fetch left temporary data.'
    [IO.Directory]::CreateDirectory((Join-Path $fetchFixture 'external/asiosdk')) | Out-Null
    Assert-Fails { & (Join-Path $fetchFixture 'scripts/fetch-asio-sdk.ps1') -ArchivePath $badArchive } 'already exists'
    Pass 'SDK fetch rejects an untrusted archive and never replaces an existing SDK'

    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /s /c ""' + (Join-Path $folder 'setup.cmd') + '" -NoLaunch < NUL"'
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd()
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        Assert-True ($process.ExitCode -eq 0 -and $errorText.Length -eq 0 -and $output.Contains('Setup complete.')) 'setup.cmd failed from a different working directory.'
    } finally { $process.Dispose() }
    Pass 'The CMD entry point invokes Windows PowerShell without launching the player'

    $playbackOnly = New-Fixture ('playback only ' + [char]0x65E5 + [char]0x672C)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $start.Arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
        (Join-Path $playbackOnly 'setup.ps1') + '" -DeviceClsid ' + $device + ' -MusicDirectory "' + $music + '" -NoLaunch'
    $start.WorkingDirectory = $work
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Model a playback PC: standard Windows tools only, no compiler/SDK discovery.
    $start.EnvironmentVariables['PATH'] = (Join-Path $env:SystemRoot 'System32') + ';' +
        (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0')
    foreach ($name in @('VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir', 'WindowsSdkDir',
        'WindowsSDKVersion', 'VisualStudioVersion', 'VSCMD_VER', 'VCTargetsPath', 'INCLUDE', 'LIB', 'LIBPATH')) {
        $start.EnvironmentVariables.Remove($name)
    }
    $start.EnvironmentVariables['ProgramFiles(x86)'] = Join-Path $work 'no developer installation'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd()
        $errorText = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        Assert-True ($process.ExitCode -eq 0 -and $errorText.Length -eq 0 -and $output.Contains('Setup complete.')) 'Setup depended on build-PC tools or environment.'
        Assert-True ([IO.File]::ReadAllText((Join-Path $playbackOnly 'settings.ini')).Contains('SelectedDevice=' + $device)) 'Playback-only setup did not configure the selected device.'
        Assert-True ([IO.File]::ReadAllLines((Join-Path $playbackOnly 'playlist.txt'), $utf8).Count -eq 3) 'Playback-only setup did not create the local playlist.'
    } finally { $process.Dispose() }
    Pass 'Initial setup works with Windows-only PATH and no compiler or SDK environment'

    $cmdBuild = Join-Path $work 'CMD build modules'
    [IO.Directory]::CreateDirectory((Join-Path $cmdBuild 'scripts')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $cmdBuild 'content')) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'build.cmd') -Destination $cmdBuild
    Write-Text (Join-Path $cmdBuild 'content/probe.txt') 'Windows module fixture'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $cmdBuild 'content'), (Join-Path $cmdBuild 'probe.zip'))
    Write-Text (Join-Path $cmdBuild 'scripts/build.ps1') @'
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
    if ($coreModules) {
        # A negative control for the observed pwsh -> cmd -> Windows PowerShell failure.
        $legacy = [IO.File]::ReadAllText((Join-Path $cmdBuild 'build.cmd')) -replace '(?m)^set "PSModulePath=[^\r\n]*\r?\n', ''
        Write-Text (Join-Path $cmdBuild 'legacy-build.cmd') $legacy
        $result = Invoke-CmdWithForeignModulePath (Join-Path $cmdBuild 'legacy-build.cmd') ''
        Assert-True ($result.Code -ne 0 -and $result.Error.Contains('Get-FileHash')) 'The legacy CMD did not reproduce the PowerShell module collision.'
    }
    $result = Invoke-CmdWithForeignModulePath (Join-Path $cmdBuild 'build.cmd') ''
    Assert-True ($result.Code -eq 0 -and $result.Error.Length -eq 0 -and $result.Output.Contains('WINDOWS_MODULES_OK')) 'build.cmd could not load Windows hashing/archive modules with an inherited foreign PSModulePath.'
    Pass 'build.cmd restores Windows modules for hashing and ZIP extraction'

    $cmdSetup = New-Fixture ('CMD first setup ' + [char]0x65E5 + [char]0x672C)
    $result = Invoke-CmdWithForeignModulePath (Join-Path $cmdSetup 'setup.cmd') ('-DeviceClsid ' + $device + ' -MusicDirectory "' + $music + '" -NoLaunch')
    Assert-True ($result.Code -eq 0 -and $result.Error.Length -eq 0 -and $result.Output.Contains('Setup complete.')) 'setup.cmd failed initial configuration with an inherited foreign PSModulePath.'
    Assert-True ([IO.File]::ReadAllText((Join-Path $cmdSetup 'settings.ini')).Contains('SelectedDevice=' + $device)) 'CMD initial setup did not configure the selected device.'
    Assert-True ([IO.File]::ReadAllLines((Join-Path $cmdSetup 'playlist.txt'), $utf8).Count -eq 3) 'CMD initial setup did not create the local playlist.'
    Pass 'setup.cmd completes first configuration with an inherited foreign PSModulePath'

    Write-Output ('[PASS] ' + $passed + ' setup/build safeguards; no player or ASIO driver was loaded.')
} finally {
    $resolved = [IO.Path]::GetFullPath($work)
    $allowed = [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([IO.Path]::GetFileName($resolved).StartsWith('setup-tests-'))) { throw 'Unexpected test directory; refusing cleanup.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
