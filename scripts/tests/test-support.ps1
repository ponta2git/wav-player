# SPDX-License-Identifier: GPL-3.0-only
# Shared helpers of the PowerShell regression tests in this directory; each test
# script dot-sources this file. A test works in its own directory below out/,
# made and removed with the staging helpers loaded here.
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'support/staging.ps1')

$script:completedTestGroups = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# Requires Action to fail with an error whose message contains Expected. An
# error with another message passes through as the failure it is.
function Assert-Fails([scriptblock]$Action, [string]$Expected) {
    try { & $Action | Out-Null } catch {
        if (-not $_.Exception.Message.Contains($Expected)) { throw }
        return
    }
    throw "Expected failure: $Expected"
}

# Requires the file at Path to hold exactly the bytes Expected.
function Assert-SameBytes([string]$Path, [byte[]]$Expected, [string]$Message) {
    Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) -ceq [Convert]::ToBase64String($Expected)) $Message
}

# Reports a finished group of checks and counts it for Get-TestGroupCount.
function Complete-TestGroup([string]$Name) {
    $script:completedTestGroups++
    Write-Output ('PASS ' + $Name)
}

function Get-TestGroupCount {
    $script:completedTestGroups
}

# Text fixtures are UTF-8 without a byte order mark; invalid text is an error.
function Get-TestEncoding {
    [Text.UTF8Encoding]::new($false, $true)
}

function Write-TestFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (Get-TestEncoding))
}

# The Windows PowerShell that the CMD entry points and CI start.
function Get-WindowsPowerShell {
    Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
}

# Runs a program without a window and returns its exit Code, Output and Error
# text. Environment sets variables of the child and RemoveEnvironment removes
# them. Both streams are read while the child runs, so a full pipe cannot block
# it, and a child that does not finish in time is stopped and fails the test.
function Invoke-TestProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FileName,
        [string]$Arguments = '',
        [string]$WorkingDirectory = '',
        [hashtable]$Environment = @{},
        [string[]]$RemoveEnvironment = @(),
        [int]$TimeoutSeconds = 120
    )
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $FileName
    $start.Arguments = $Arguments
    if ($WorkingDirectory) { $start.WorkingDirectory = $WorkingDirectory }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($name in $RemoveEnvironment) { $start.EnvironmentVariables.Remove($name) }
    foreach ($name in $Environment.Keys) { $start.EnvironmentVariables[$name] = [string]$Environment[$name] }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorText = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            throw "A test process did not finish within $TimeoutSeconds s: $FileName $Arguments"
        }
        [PSCustomObject]@{ Code = $process.ExitCode; Output = $output.Result; Error = $errorText.Result }
    } finally { $process.Dispose() }
}

# Runs a CMD entry point as a double click would, with no console input: the
# final pause of the entry points returns at once.
function Invoke-TestCmd {
    param(
        [Parameter(Mandatory = $true)][string]$Entry,
        [string]$Arguments = '',
        [string]$WorkingDirectory = '',
        [hashtable]$Environment = @{}
    )
    $commandLine = '/d /s /c ""' + $Entry + '" ' + $Arguments + ' < NUL"'
    Invoke-TestProcess -FileName $env:ComSpec -Arguments $commandLine -WorkingDirectory $WorkingDirectory -Environment $Environment
}
