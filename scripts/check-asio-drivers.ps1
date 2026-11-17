[CmdletBinding()]
param(
    [switch]$Include32Bit,
    [switch]$Json,
    [switch]$RequireX64
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = $null
    $reader = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            return 'Invalid'
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) {
            return 'Invalid'
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            return 'Invalid'
        }
        switch ($reader.ReadUInt16()) {
            0x014c { return 'x86' }
            0x8664 { return 'x64' }
            0xAA64 { return 'ARM64' }
            default { return 'Unknown' }
        }
    } catch {
        return 'Unreadable'
    } finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        } elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Get-AsioDriversForView {
    param(
        [Parameter(Mandatory = $true)]
        [Microsoft.Win32.RegistryView]$View
    )

    $viewName = $View.ToString()
    $expectedMachine = if ($View -eq [Microsoft.Win32.RegistryView]::Registry64) {
        'x64'
    } else {
        'x86'
    }

    $baseKey = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        $View)
    try {
        $asioKey = $baseKey.OpenSubKey('SOFTWARE\ASIO')
        if ($null -eq $asioKey) {
            return
        }
        try {
            foreach ($keyName in $asioKey.GetSubKeyNames()) {
                $driverKey = $asioKey.OpenSubKey($keyName)
                if ($null -eq $driverKey) {
                    continue
                }
                try {
                    $description = [string]$driverKey.GetValue(
                        'Description',
                        $keyName,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    $clsidText = [string]$driverKey.GetValue(
                        'CLSID',
                        '',
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    # The x64 host reads a braced GUID from a REG_SZ value.
                    $parsedClsid = [Guid]::Empty
                    $validClsid = $clsidText.Length -eq 38 -and
                        [Guid]::TryParseExact($clsidText, 'B', [ref]$parsedClsid) -and
                        $driverKey.GetValueKind('CLSID') -eq [Microsoft.Win32.RegistryValueKind]::String
                } finally {
                    $driverKey.Dispose()
                }

                $rawDllPath = $null
                if ($validClsid) {
                    $comPath = 'SOFTWARE\Classes\CLSID\{0}\InprocServer32' -f $parsedClsid.ToString('B')
                    $comKey = $baseKey.OpenSubKey($comPath)
                    if ($null -ne $comKey) {
                        try {
                            $rawDllPath = $comKey.GetValue(
                                $null,
                                $null,
                                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                        } finally {
                            $comKey.Dispose()
                        }
                    }
                }

                $dllPath = ''
                if ($null -ne $rawDllPath) {
                    $dllPath = [Environment]::ExpandEnvironmentVariables([string]$rawDllPath).Trim().Trim('"')
                }
                $dllExists = $dllPath.Length -gt 0 -and (Test-Path -LiteralPath $dllPath -PathType Leaf)
                $machine = if ($dllExists) { Get-PeMachine -Path $dllPath } else { 'Missing' }
                $fileVersion = ''
                if ($dllExists) {
                    $fileVersion = (Get-Item -LiteralPath $dllPath).VersionInfo.FileVersion
                }

                $status = if (-not $validClsid) {
                    'Invalid CLSID'
                } elseif ($dllPath.Length -eq 0) {
                    'COM registration missing'
                } elseif (-not $dllExists) {
                    'Driver DLL missing'
                } elseif ($machine -ne $expectedMachine) {
                    'Bitness mismatch'
                } elseif ($View -eq [Microsoft.Win32.RegistryView]::Registry32) {
                    '32-bit only; not loadable by wavplay.exe'
                } else {
                    'Ready for x64 host load test'
                }

                [PSCustomObject][ordered]@{
                    RegistryView = $viewName
                    KeyName = $keyName
                    Description = $description
                    CLSID = $clsidText
                    DllPath = $dllPath
                    DllExists = $dllExists
                    Machine = $machine
                    FileVersion = $fileVersion
                    LoadableByX64App = (
                        $View -eq [Microsoft.Win32.RegistryView]::Registry64 -and
                        $dllExists -and
                        $machine -eq 'x64')
                    Status = $status
                }
            }
        } finally {
            $asioKey.Dispose()
        }
    } finally {
        $baseKey.Dispose()
    }
}

$drivers = @(Get-AsioDriversForView -View ([Microsoft.Win32.RegistryView]::Registry64))
if ($Include32Bit) {
    $drivers += @(Get-AsioDriversForView -View ([Microsoft.Win32.RegistryView]::Registry32))
}

$ready = @($drivers | Where-Object { $_.LoadableByX64App })

if ($Json) {
    [PSCustomObject][ordered]@{
        X64ReadyCount = $ready.Count
        Drivers = $drivers
    } | ConvertTo-Json -Depth 4
} else {
    if ($drivers.Count -eq 0) {
        Write-Warning 'No ASIO registry entries were found.'
    } else {
        $drivers | Format-Table RegistryView, Description, Machine, FileVersion, LoadableByX64App, Status -AutoSize
        foreach ($driver in $drivers) {
            Write-Output ("[{0}] {1}" -f $driver.RegistryView, $driver.Description)
            Write-Output ("  CLSID : {0}" -f $driver.CLSID)
            Write-Output ("  DLL   : {0}" -f $driver.DllPath)
            Write-Output ("  Status: {0}" -f $driver.Status)
        }
    }
    Write-Output ("x64 drivers ready for a later host-load test: {0}" -f $ready.Count)
    Write-Output 'No driver DLL was loaded and no device setting was changed.'
}

if ($RequireX64 -and $ready.Count -eq 0) {
    throw 'No valid x64 ASIO driver registration was found.'
}
