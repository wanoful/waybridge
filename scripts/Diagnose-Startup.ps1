# Run beside Waybridge.Control.exe; compatible with Windows PowerShell 5.1.
# Collects startup diagnostics only, not profiles, SSH keys, tokens or clipboard data.
[CmdletBinding()]
param([int]$WaitSeconds = 15)
$ErrorActionPreference = 'Stop'
$AppDirectory = $PSScriptRoot
$Executable = Join-Path $AppDirectory 'Waybridge.Control.exe'
if (-not (Test-Path -LiteralPath $Executable)) { throw 'Place this script beside Waybridge.Control.exe in the extracted package.' }
$Stamp = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$ReportDirectory = Join-Path $env:LOCALAPPDATA "Waybridge\startup-reports\$Stamp"
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$Report = Join-Path $ReportDirectory 'report.txt'
function Record([string]$Text) { $Text | Out-File -LiteralPath $Report -Encoding utf8 -Append; Write-Host $Text }
function Get-PeMachine([string]$Path) {
    $Stream = [System.IO.File]::OpenRead($Path)
    $Reader = New-Object System.IO.BinaryReader($Stream)
    try {
        if ($Reader.ReadUInt16() -ne 0x5A4D) { return 'invalid-MZ' }
        $Stream.Position = 0x3c; $Offset = $Reader.ReadUInt32()
        $Stream.Position = $Offset
        if ($Reader.ReadUInt32() -ne 0x4550) { return 'invalid-PE' }
        return ('0x{0:X4}' -f $Reader.ReadUInt16())
    } finally { $Reader.Dispose(); $Stream.Dispose() }
}
Record "Waybridge startup report: $(Get-Date -Format o)"
Record "Directory: $AppDirectory"
Record "PowerShell: $($PSVersionTable.PSVersion); process architecture: $env:PROCESSOR_ARCHITECTURE; native override: $env:PROCESSOR_ARCHITEW6432"
try {
    $Windows = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Record "Windows: $($Windows.ProductName) $($Windows.DisplayVersion) build $($Windows.CurrentBuild).$($Windows.UBR)"
} catch { Record "Windows version query: $($_.Exception.Message)" }
foreach ($Name in @('Waybridge.Control.exe','Waybridge.Control.dll','Waybridge.Control.Core.dll','waybridge-client.exe','hostfxr.dll','hostpolicy.dll','coreclr.dll','Microsoft.WindowsAppRuntime.dll','Microsoft.ui.xaml.dll','WinRT.Runtime.dll','Microsoft.Windows.SDK.NET.dll')) {
    $Path = Join-Path $AppDirectory $Name
    if (Test-Path -LiteralPath $Path) {
        Record "$Name : $(Get-PeMachine $Path); $((Get-Item -LiteralPath $Path).Length) bytes; SHA256 $((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash)"
    } else { Record "MISSING: $Name" }
}
$ChecksumFile = Join-Path $AppDirectory 'SHA256SUMS.txt'
if (Test-Path -LiteralPath $ChecksumFile) {
    $Checked = 0; $Failures = 0
    foreach ($Line in [System.IO.File]::ReadLines($ChecksumFile)) {
        if ($Line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { continue }
        $Expected = $Matches[1]; $Relative = $Matches[2]
        $File = Join-Path $AppDirectory $Relative
        if (-not (Test-Path -LiteralPath $File)) { Record "CHECKSUM MISSING: $Relative"; $Failures++; continue }
        if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Expected) { Record "CHECKSUM MISMATCH: $Relative"; $Failures++ }
        $Checked++
    }
    Record "Package verification: $Checked files; $Failures missing or mismatched"
}
Record "Existing GUI processes: $(@(Get-Process -Name 'Waybridge.Control' -ErrorAction SilentlyContinue).Count)"
$ResourceIndex = Join-Path $AppDirectory 'resources.pri'
if (Test-Path -LiteralPath $ResourceIndex) {
    Record "resources.pri : $((Get-Item -LiteralPath $ResourceIndex).Length) bytes; SHA256 $((Get-FileHash -LiteralPath $ResourceIndex -Algorithm SHA256).Hash)"
} else { Record 'MISSING: default resources.pri (WinUI themes cannot be resolved in this self-contained package).' }
$Variables = @{
    'DOTNET_HOST_TRACE' = '1'; 'DOTNET_HOST_TRACE_VERBOSITY' = '4';
    'DOTNET_HOST_TRACEFILE' = (Join-Path $ReportDirectory 'dotnet-host.txt');
    'COREHOST_TRACE' = '1'; 'COREHOST_TRACE_VERBOSITY' = '4';
    'COREHOST_TRACEFILE' = (Join-Path $ReportDirectory 'dotnet-host.txt');
    'WAYBRIDGE_STARTUP_LOG' = (Join-Path $ReportDirectory 'startup.log')
}
$Previous = @{}
$Started = Get-Date
try {
    foreach ($Key in $Variables.Keys) {
        $Previous[$Key] = [Environment]::GetEnvironmentVariable($Key, 'Process')
        [Environment]::SetEnvironmentVariable($Key, $Variables[$Key], 'Process')
    }
    $Child = Start-Process -FilePath $Executable -WorkingDirectory $AppDirectory -PassThru `
        -RedirectStandardOutput (Join-Path $ReportDirectory 'stdout.txt') `
        -RedirectStandardError (Join-Path $ReportDirectory 'stderr.txt')
    $Handle = $Child.Handle # Keep the handle so ExitCode is available after fast exits in PS 5.1.
    Record "Started PID: $($Child.Id)"
    if ($Child.WaitForExit([Math]::Max(1,[Math]::Min(60,$WaitSeconds)) * 1000)) {
        $Code = $Child.ExitCode
        $Unsigned = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$Code),0)
        Record "Exited: $Code (0x$($Unsigned.ToString('X8')))"
    } else {
        $Child.Refresh()
        Record "Process still running; window handle: $($Child.MainWindowHandle). Left running for inspection."
    }
} catch { Record "Launch exception: $($_.Exception.ToString())" }
finally {
    foreach ($Key in $Previous.Keys) { [Environment]::SetEnvironmentVariable($Key, $Previous[$Key], 'Process') }
}
Start-Sleep -Seconds 2 # Let Windows write crash and .NET Runtime events.
try {
    $Events = @(Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$Started.AddSeconds(-2)} -MaxEvents 100 -ErrorAction Stop | Where-Object {
        $_.ProviderName -in @('.NET Runtime','Application Error','Windows Error Reporting','SideBySide') -and $_.Message -match 'Waybridge\.Control'
    })
    if ($Events.Count) {
        $Events | Format-List TimeCreated,ProviderName,Id,LevelDisplayName,Message | Out-File -LiteralPath (Join-Path $ReportDirectory 'events.txt') -Encoding utf8 -Width 300
        Record "Related Windows events: $($Events.Count)"
    } else { Record 'No matching Windows Application events found.' }
} catch { Record "Event query: $($_.Exception.Message)" }
if (-not (Test-Path -LiteralPath (Join-Path $ReportDirectory 'startup.log'))) {
    Record 'No managed startup.log: failure occurred before Program.Main, or logging could not be created. Inspect dotnet-host.txt and events.txt.'
}
$Archive = $ReportDirectory + '.zip'
try {
    Compress-Archive -Path (Join-Path $ReportDirectory '*') -DestinationPath $Archive -Force
    Record "Report ZIP: $Archive"
} catch { Record "ZIP creation failed: $($_.Exception.Message)" }
Write-Host "`nPlease share report.txt, startup.log (if present), events.txt and stderr.txt, or the report ZIP."
