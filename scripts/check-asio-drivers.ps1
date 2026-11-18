# Lists the registered ASIO drivers from the registry and the header of each
# driver DLL. No driver DLL is loaded and no device setting is changed.
# build.ps1 copies this file into the player folder, where setup.ps1 reads its
# -Json output (Description, CLSID, LoadableByX64App), so it uses no other script.
[CmdletBinding()]
param(
    [switch]$Include32Bit,
    [switch]$Json,
    [switch]$RequireX64
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Returns the machine type in the PE header of a file, without loading it.
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

# One row of the inventory. Every row has every property, whatever was readable.
function New-DriverRecord {
    param(
        [Microsoft.Win32.RegistryView]$View,
        [string]$KeyName,
        [string]$Description,
        [string]$Clsid = '',
        [string]$DllPath = '',
        [bool]$DllExists = $false,
        [string]$Machine = 'Missing',
        [string]$FileVersion = '',
        [Parameter(Mandatory = $true)][string]$Status
    )
    [PSCustomObject][ordered]@{
        RegistryView = $View.ToString()
        KeyName = $KeyName
        Description = $Description
        CLSID = $Clsid
        DllPath = $DllPath
        DllExists = $DllExists
        Machine = $Machine
        FileVersion = $FileVersion
        LoadableByX64App = (
            $View -eq [Microsoft.Win32.RegistryView]::Registry64 -and
            $DllExists -and
            $Machine -eq 'x64')
        Status = $Status
    }
}

# Returns the in-process server path that COM registers for Clsid in the
# registry view of BaseKey, or an empty string when there is none.
function Get-InprocServerPath {
    param([Microsoft.Win32.RegistryKey]$BaseKey, [Guid]$Clsid)
    $comKey = $BaseKey.OpenSubKey('SOFTWARE\Classes\CLSID\{0}\InprocServer32' -f $Clsid.ToString('B'))
    if ($null -eq $comKey) {
        return ''
    }
    try {
        $rawPath = $comKey.GetValue(
            $null,
            $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    } finally {
        $comKey.Dispose()
    }
    if ($null -eq $rawPath) {
        return ''
    }
    [Environment]::ExpandEnvironmentVariables([string]$rawPath).Trim().Trim('"')
}

# Returns the inventory row of the driver registered as KeyName below AsioKey,
# or nothing when that key cannot be opened.
function Get-AsioDriver {
    param(
        [Microsoft.Win32.RegistryKey]$BaseKey,
        [Microsoft.Win32.RegistryKey]$AsioKey,
        [string]$KeyName,
        [Microsoft.Win32.RegistryView]$View
    )
    $driverKey = $AsioKey.OpenSubKey($KeyName)
    if ($null -eq $driverKey) {
        return
    }
    try {
        $description = [string]$driverKey.GetValue(
            'Description',
            $KeyName,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $clsidText = [string]$driverKey.GetValue(
            'CLSID',
            '',
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        # The x64 host reads a braced GUID from a REG_SZ value.
        $clsid = [Guid]::Empty
        $validClsid = $clsidText.Length -eq 38 -and
            [Guid]::TryParseExact($clsidText, 'B', [ref]$clsid) -and
            $driverKey.GetValueKind('CLSID') -eq [Microsoft.Win32.RegistryValueKind]::String
    } finally {
        $driverKey.Dispose()
    }
    if (-not $validClsid) {
        return New-DriverRecord -View $View -KeyName $KeyName -Description $description -Clsid $clsidText -Status 'Invalid CLSID'
    }

    $dllPath = Get-InprocServerPath $BaseKey $clsid
    $dllExists = $dllPath.Length -gt 0 -and (Test-Path -LiteralPath $dllPath -PathType Leaf)
    $machine = 'Missing'
    $fileVersion = ''
    if ($dllExists) {
        $machine = Get-PeMachine -Path $dllPath
        $fileVersion = (Get-Item -LiteralPath $dllPath).VersionInfo.FileVersion
    }
    $expectedMachine = if ($View -eq [Microsoft.Win32.RegistryView]::Registry64) { 'x64' } else { 'x86' }
    $status = if ($dllPath.Length -eq 0) {
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
    $record = @{
        View = $View; KeyName = $KeyName; Description = $description; Clsid = $clsidText; DllPath = $dllPath
        DllExists = $dllExists; Machine = $machine; FileVersion = $fileVersion; Status = $status
    }
    New-DriverRecord @record
}

# Returns the inventory rows of one registry view. A registration that cannot be
# read is reported as a row of its own, so that it does not hide the others.
function Get-AsioDriversForView {
    param(
        [Parameter(Mandatory = $true)]
        [Microsoft.Win32.RegistryView]$View
    )

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
                try {
                    Get-AsioDriver -BaseKey $baseKey -AsioKey $asioKey -KeyName $keyName -View $View
                } catch {
                    Write-Warning ('Cannot read the ASIO registration {0}: {1}' -f $keyName, $_.Exception.Message)
                    New-DriverRecord -View $View -KeyName $keyName -Description $keyName -Machine 'Unknown' -Status 'Registration unreadable'
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
