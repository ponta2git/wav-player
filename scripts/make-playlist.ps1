# Lists the WAV files below a folder into a UTF-8 playlist. build.ps1 copies this
# file into the player folder, where setup.ps1 calls it, so it uses no other
# script. The new list is written beside the playlist and put in place in one
# step, so a failure leaves the existing playlist as it was.
[CmdletBinding()]
param(
    [string]$MusicDirectory,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Returns the full paths of the WAV files below Folder in a stable order.
function Get-WavPath {
    param([string]$Folder)
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $Folder -File -Recurse -Filter '*.wav') {
        $paths.Add($file.FullName)
    }
    $paths.Sort([StringComparer]::OrdinalIgnoreCase)
    , $paths.ToArray()
}

# Writes Paths beside Destination, then puts that file in place in one step. An
# existing playlist becomes a backup, whose path is returned.
function Save-Playlist {
    param([string[]]$Paths, [string]$Destination)
    $parent = [IO.Path]::GetDirectoryName($Destination)
    if (-not [IO.Directory]::Exists($parent)) {
        throw 'The output folder does not exist.'
    }
    $suffix = [Guid]::NewGuid().ToString('N')
    $temporary = Join-Path $parent ('.playlist-' + $suffix + '.tmp')
    $replacing = [IO.File]::Exists($Destination)
    try {
        [IO.File]::WriteAllLines($temporary, $Paths, [Text.UTF8Encoding]::new($true, $true))
        if ($replacing) {
            $stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture)
            $backup = $Destination + '.' + $stamp + '-' + $suffix + '.bak'
            [IO.File]::Replace($temporary, $Destination, $backup)
            return $backup
        }
        [IO.File]::Move($temporary, $Destination)
    } finally {
        # Gone after a successful replace or move; left over only by a failure.
        # It stays if a failed replacement took the playlist away, as the only
        # copy of the new list. Failing to remove it must not hide that failure.
        if ([IO.File]::Exists($temporary) -and (-not $replacing -or [IO.File]::Exists($Destination))) {
            try { [IO.File]::Delete($temporary) } catch { Write-Warning ('A temporary file could not be removed: ' + $temporary) }
        }
    }
}

# Windows PowerShell does not set PSScriptRoot while binding default parameters.
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $PSScriptRoot 'playlist.txt'
}

# This helper only lists files. The player checks the WAV contents on launch.
if ([string]::IsNullOrWhiteSpace($MusicDirectory)) {
    $MusicDirectory = Read-Host 'Music folder (absolute path; subfolders are included)'
}
$MusicDirectory = $MusicDirectory.Trim().Trim('"')
if ([string]::IsNullOrWhiteSpace($MusicDirectory)) {
    throw 'No music folder was entered.'
}
$folder = Get-Item -LiteralPath $MusicDirectory
if ($folder.PSProvider.Name -ne 'FileSystem' -or -not $folder.PSIsContainer) {
    throw 'Choose a file-system folder.'
}

$paths = Get-WavPath $folder.FullName
if ($paths.Count -eq 0) {
    throw 'No WAV files found. The existing playlist has not been changed.'
}
$destination = [IO.Path]::GetFullPath($OutputPath)
$backup = Save-Playlist $paths $destination
if ($backup) {
    Write-Output ('Previous playlist saved as: ' + $backup)
}
Write-Output ('Saved ' + $paths.Count + ' WAV paths to: ' + $destination)
Write-Output 'Close and reopen WAV Player to load this playlist. No playback was started.'
