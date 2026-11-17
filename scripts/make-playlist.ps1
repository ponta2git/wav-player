[CmdletBinding()]
param(
    [string]$MusicDirectory,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

$paths = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $folder.FullName -File -Recurse -Filter '*.wav') {
    $paths.Add($file.FullName)
}
if ($paths.Count -eq 0) {
    throw 'No WAV files found. The existing playlist has not been changed.'
}
$paths.Sort([StringComparer]::OrdinalIgnoreCase)
$destination = [IO.Path]::GetFullPath($OutputPath)
$parent = [IO.Path]::GetDirectoryName($destination)
if (-not [IO.Directory]::Exists($parent)) {
    throw 'The output folder does not exist.'
}

# Write beside the target, then atomically replace it while retaining a backup.
$suffix = [Guid]::NewGuid().ToString('N')
$temporary = Join-Path $parent ('.playlist-' + $suffix + '.tmp')
[IO.File]::WriteAllLines($temporary, $paths, [Text.UTF8Encoding]::new($true, $true))
if ([IO.File]::Exists($destination)) {
    $backup = $destination + '.' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '-' + $suffix + '.bak'
    [IO.File]::Replace($temporary, $destination, $backup)
    Write-Output ('Previous playlist saved as: ' + $backup)
} else {
    [IO.File]::Move($temporary, $destination)
}
Write-Output ('Saved ' + $paths.Count + ' WAV paths to: ' + $destination)
Write-Output 'Close and reopen WAV Player to load this playlist. No playback was started.'
