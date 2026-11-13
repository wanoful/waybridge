param([ValidateSet('arm64','x64')][string]$Architecture = 'arm64')
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $Platform = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x64' }
    $Native = "build/native-$Architecture"
    cmake -S client -B $Native -A $Platform
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
    cmake --build $Native --config Release --target waybridge-client
    if ($LASTEXITCODE) { throw 'Native worker build failed' }
    $Output = "build/control-center-$Architecture"
    dotnet publish gui/Waybridge.Control/Waybridge.Control.csproj -c Release -r "win-$Architecture" "-p:Platform=$Platform" --self-contained true -p:WindowsAppSDKSelfContained=true -o $Output
    if ($LASTEXITCODE) { throw 'WinUI publish failed' }
    Copy-Item "$Native/Release/waybridge-client.exe", 'gui/README.md' $Output
    Copy-Item 'scripts/Diagnose-Startup.ps1', 'scripts/Diagnose-Startup.cmd' $Output
    Copy-Item LICENSE "$Output/LICENSE-Waybridge.txt"
    Write-Host "Built $Output/Waybridge.Control.exe"
} finally { Pop-Location }
