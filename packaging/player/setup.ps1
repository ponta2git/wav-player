# Local first-run configuration only. Does not download, build, install a driver or start playback.
# Runs from the flat player folder that build.ps1 assembles, beside wavplay.exe
# and the two helper scripts, and uses nothing else. Every input is validated
# before the first file is written, and an existing settings.ini or playlist.txt
# is replaced only on explicit request.
[CmdletBinding()]
param(
    [string]$DeviceClsid,
    [string]$MusicDirectory,
    [switch]$UpdatePlaylist,
    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Fails unless the folder holds the player and both helper scripts.
function Assert-PlayerFolder {
    param([string]$Executable, [string[]]$Helpers)
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw 'wavplay.exe is missing. Run build.cmd on the build PC and copy the complete player folder to this PC.'
    }
    foreach ($helper in $Helpers) {
        if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
            throw ('The player folder is incomplete: ' + [IO.Path]::GetFileName($helper) + ' is missing. Copy the complete folder again.')
        }
    }
}

# The configuration is not changed while the player runs in this session.
function Assert-PlayerClosed {
    $sessionId = (Get-Process -Id $PID).SessionId
    if (@(Get-Process -Name wavplay -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sessionId }).Count) {
        throw 'Close WAV Player before running setup. Existing settings have not been changed.'
    }
}

# Returns whether a configuration file exists; a directory of that name is refused.
function Test-ConfigurationFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return $false
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw ('A directory occupies a configuration filename: ' + [IO.Path]::GetFileName($Path))
    }
    $true
}

# Returns the Clsid and Name of the usable x64 driver that RequestedClsid names
# or, without one, that the user picks from the numbered list.
function Select-AsioDriver {
    param([string]$InventoryScript, [string]$RequestedClsid)
    $inventory = (& $InventoryScript -Json | Out-String) | ConvertFrom-Json
    $drivers = @($inventory.Drivers | Where-Object { $_.LoadableByX64App })
    if (-not $drivers.Count) {
        throw 'No usable x64 ASIO driver registration was found. Install your device manufacturer''s x64 driver, then run setup again.'
    }
    if ([string]::IsNullOrWhiteSpace($RequestedClsid)) {
        for ($index = 0; $index -lt $drivers.Count; $index++) {
            Write-Host ('{0}. {1}' -f ($index + 1), $drivers[$index].Description)
        }
        $answer = Read-Host 'ASIO driver number'
        $number = 0
        if (-not [int]::TryParse($answer, [ref]$number) -or $number -lt 1 -or $number -gt $drivers.Count) {
            throw 'Choose one of the displayed driver numbers.'
        }
        $RequestedClsid = [string]$drivers[$number - 1].CLSID
    }
    $parsed = [Guid]::Empty
    if (-not [Guid]::TryParse($RequestedClsid, [ref]$parsed) -or $parsed -eq [Guid]::Empty) { throw 'The ASIO device CLSID is invalid.' }
    $selected = @($drivers | Where-Object { $_.CLSID -ieq $parsed.ToString('B') })
    if ($selected.Count -ne 1) { throw 'The selected CLSID is not a usable registered x64 ASIO driver.' }
    [PSCustomObject]@{ Clsid = $parsed.ToString('B').ToUpperInvariant(); Name = [string]$selected[0].Description }
}

# The initial settings.ini for one device. Its name stays on one line of the file.
function Get-SettingsText {
    param([string]$Clsid, [string]$Name)
    $line = $Name.Replace("`r", ' ').Replace("`n", ' ').Replace([char]0, ' ')
    "[General]`r`nSelectedDevice=$Clsid`r`n`r`n[Device.$Clsid]`r`nName=$line`r`nOutputPair=1`r`nBufferSize=0`r`nStartSilence=100`r`n"
}

# Returns the WAV folder that Requested names or, without one, that the user
# enters, after checking that it is an absolute path on a local drive.
function Resolve-LocalMusicFolder {
    param([string]$Requested)
    if ([string]::IsNullOrWhiteSpace($Requested)) { $Requested = Read-Host 'Local WAV folder (including subfolders)' }
    $musicPath = $Requested.Trim().Trim('"')
    if ($musicPath -notmatch '^[A-Za-z]:[\\/]') { throw 'Choose an absolute local WAV folder, for example C:\Music.' }
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($musicPath))
    if ($drive.DriveType -notin @([IO.DriveType]::Fixed, [IO.DriveType]::Removable, [IO.DriveType]::CDRom, [IO.DriveType]::Ram)) {
        throw 'The WAV folder must be on a local drive.'
    }
    $musicPath
}

# Writes the playlist of MusicPath with the helper script. Replacing an existing
# playlist keeps a backup; a first playlist never replaces a file that appeared
# while the prompts were open.
function Save-MusicPlaylist {
    param([string]$PlaylistScript, [string]$MusicPath, [string]$PlaylistPath, [bool]$ReplaceExisting)
    if ($ReplaceExisting) {
        & $PlaylistScript -MusicDirectory $MusicPath -OutputPath $PlaylistPath
        return
    }
    $prepared = Join-Path ([IO.Path]::GetDirectoryName($PlaylistPath)) ('.setup-playlist-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        & $PlaylistScript -MusicDirectory $MusicPath -OutputPath $prepared
        [IO.File]::Move($prepared, $PlaylistPath)
    } finally {
        if (Test-Path -LiteralPath $prepared -PathType Leaf) { Remove-Item -LiteralPath $prepared }
    }
}

# Creates the settings file. CreateNew also protects a file created while the
# prompts were open; a file that could not be written completely is removed
# again, so that the next run starts clean.
function New-SettingsFile {
    param([string]$Path, [string]$Text)
    $bytes = [Text.UTF8Encoding]::new($false, $true).GetBytes($Text)
    $file = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $written = $false
    try {
        $file.Write($bytes, 0, $bytes.Length)
        $file.Flush($true)
        $written = $true
    } finally {
        $file.Dispose()
        if (-not $written) { [IO.File]::Delete($Path) }
    }
}

$executable = Join-Path $PSScriptRoot 'wavplay.exe'
$inventoryScript = Join-Path $PSScriptRoot 'check-asio-drivers.ps1'
$playlistScript = Join-Path $PSScriptRoot 'make-playlist.ps1'
$settingsPath = Join-Path $PSScriptRoot 'settings.ini'
$playlistPath = Join-Path $PSScriptRoot 'playlist.txt'

Assert-PlayerFolder $executable @($inventoryScript, $playlistScript)
Assert-PlayerClosed
$hasSettings = Test-ConfigurationFile $settingsPath
$hasPlaylist = Test-ConfigurationFile $playlistPath
if ($hasSettings -and -not [string]::IsNullOrWhiteSpace($DeviceClsid)) {
    throw 'settings.ini already exists. It has been preserved; edit it while WAV Player is closed to change the device.'
}
if ($hasPlaylist -and -not $UpdatePlaylist -and -not [string]::IsNullOrWhiteSpace($MusicDirectory)) {
    throw 'playlist.txt already exists. Use -UpdatePlaylist to replace it with a backup, or omit -MusicDirectory to keep it.'
}

# Choose and validate the device first; settings.ini is written last, after the
# playlist, so that rejected music leaves no configuration behind.
$settingsText = $null
if (-not $hasSettings) {
    $driver = Select-AsioDriver $inventoryScript $DeviceClsid
    $settingsText = Get-SettingsText $driver.Clsid $driver.Name
}
if (-not $hasPlaylist -or $UpdatePlaylist) {
    $musicPath = Resolve-LocalMusicFolder $MusicDirectory
    Save-MusicPlaylist $playlistScript $musicPath $playlistPath $hasPlaylist
} else { Write-Output 'Keeping the existing playlist.txt.' }
if (-not $hasSettings) {
    New-SettingsFile $settingsPath $settingsText
} else { Write-Output 'Keeping the existing settings.ini.' }
Write-Output 'Setup complete. Check Device and the track list; press Play when ready.'
if (-not $NoLaunch) {
    # This is the visible application the user requested. It starts stopped.
    Start-Process -FilePath $executable -WorkingDirectory $PSScriptRoot | Out-Null
}
