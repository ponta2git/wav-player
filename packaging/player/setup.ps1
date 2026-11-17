# Local first-run configuration only. Does not download, build, install a driver or start playback.
[CmdletBinding()]
param(
    [string]$DeviceClsid,
    [string]$MusicDirectory,
    [switch]$UpdatePlaylist,
    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$executable = Join-Path $PSScriptRoot 'wavplay.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'wavplay.exe is missing. Run build.cmd on the build PC and copy the complete player folder to this PC.'
}
foreach ($name in @('check-asio-drivers.ps1', 'make-playlist.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $name) -PathType Leaf)) {
        throw ('The player folder is incomplete: ' + $name + ' is missing. Copy the complete folder again.')
    }
}
$sessionId = (Get-Process -Id $PID).SessionId
if (@(Get-Process -Name wavplay -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sessionId }).Count) {
    throw 'Close WAV Player before running setup. Existing settings have not been changed.'
}
$settingsPath = Join-Path $PSScriptRoot 'settings.ini'
$playlistPath = Join-Path $PSScriptRoot 'playlist.txt'
$hasSettings = Test-Path -LiteralPath $settingsPath
$hasPlaylist = Test-Path -LiteralPath $playlistPath
foreach ($path in @($settingsPath, $playlistPath)) {
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw ('A directory occupies a configuration filename: ' + [IO.Path]::GetFileName($path))
    }
}
if ($hasSettings -and -not [string]::IsNullOrWhiteSpace($DeviceClsid)) {
    throw 'settings.ini already exists. It has been preserved; edit it while WAV Player is closed to change the device.'
}
if ($hasPlaylist -and -not $UpdatePlaylist -and -not [string]::IsNullOrWhiteSpace($MusicDirectory)) {
    throw 'playlist.txt already exists. Use -UpdatePlaylist to replace it with a backup, or omit -MusicDirectory to keep it.'
}

$ini = $null
if (-not $hasSettings) {
    $inventory = (& (Join-Path $PSScriptRoot 'check-asio-drivers.ps1') -Json | Out-String) | ConvertFrom-Json
    $drivers = @($inventory.Drivers | Where-Object { $_.LoadableByX64App })
    if (-not $drivers.Count) {
        throw 'No usable x64 ASIO driver registration was found. Install your device manufacturer''s x64 driver, then run setup again.'
    }
    if ([string]::IsNullOrWhiteSpace($DeviceClsid)) {
        for ($index = 0; $index -lt $drivers.Count; $index++) {
            Write-Host ('{0}. {1}' -f ($index + 1), $drivers[$index].Description)
        }
        $answer = Read-Host 'ASIO driver number'
        $number = 0
        if (-not [int]::TryParse($answer, [ref]$number) -or $number -lt 1 -or $number -gt $drivers.Count) {
            throw 'Choose one of the displayed driver numbers.'
        }
        $DeviceClsid = [string]$drivers[$number - 1].CLSID
    }
    $parsed = [Guid]::Empty
    if (-not [Guid]::TryParse($DeviceClsid, [ref]$parsed) -or $parsed -eq [Guid]::Empty) { throw 'The ASIO device CLSID is invalid.' }
    $selected = @($drivers | Where-Object { $_.CLSID -ieq $parsed.ToString('B') })
    if ($selected.Count -ne 1) { throw 'The selected CLSID is not a usable registered x64 ASIO driver.' }
    $guid = $parsed.ToString('B').ToUpperInvariant()
    $name = ([string]$selected[0].Description).Replace("`r", ' ').Replace("`n", ' ').Replace([char]0, ' ')
    $ini = "[General]`r`nSelectedDevice=$guid`r`n`r`n[Device.$guid]`r`nName=$name`r`nOutputPair=1`r`nBufferSize=0`r`nStartSilence=100`r`n"
}

if (-not $hasPlaylist -or $UpdatePlaylist) {
    if ([string]::IsNullOrWhiteSpace($MusicDirectory)) { $MusicDirectory = Read-Host 'Local WAV folder (including subfolders)' }
    $musicPath = $MusicDirectory.Trim().Trim('"')
    if ($musicPath -notmatch '^[A-Za-z]:[\\/]') { throw 'Choose an absolute local WAV folder, for example C:\Music.' }
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($musicPath))
    if ($drive.DriveType -notin @([IO.DriveType]::Fixed, [IO.DriveType]::Removable, [IO.DriveType]::CDRom, [IO.DriveType]::Ram)) {
        throw 'The WAV folder must be on a local drive.'
    }
    if ($hasPlaylist) {
        & (Join-Path $PSScriptRoot 'make-playlist.ps1') -MusicDirectory $musicPath -OutputPath $playlistPath
    } else {
        $preparedPlaylist = Join-Path $PSScriptRoot ('.setup-playlist-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            & (Join-Path $PSScriptRoot 'make-playlist.ps1') -MusicDirectory $musicPath -OutputPath $preparedPlaylist
            [IO.File]::Move($preparedPlaylist, $playlistPath)
        } finally {
            if (Test-Path -LiteralPath $preparedPlaylist -PathType Leaf) { Remove-Item -LiteralPath $preparedPlaylist }
        }
    }
} else { Write-Output 'Keeping the existing playlist.txt.' }
if (-not $hasSettings) {
    # CreateNew also protects a file created while the prompts were open.
    $bytes = [Text.UTF8Encoding]::new($false, $true).GetBytes($ini)
    $file = [IO.File]::Open($settingsPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
} else { Write-Output 'Keeping the existing settings.ini.' }
Write-Output 'Setup complete. Check Device and the track list; press Play when ready.'
if (-not $NoLaunch) {
    # This is the visible application the user requested. It starts stopped.
    Start-Process -FilePath $executable -WorkingDirectory $PSScriptRoot | Out-Null
}
