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
    $CliStage = "build/cli-$Architecture"
    if (Test-Path $CliStage) { Remove-Item -Recurse -Force $CliStage }
    dotnet publish gui/Waybridge.Cli/Waybridge.Cli.csproj -c Release -r "win-$Architecture" --self-contained true -o $CliStage
    if ($LASTEXITCODE) { throw 'CLI publish failed' }
    Copy-Item "$CliStage/waybridge.exe", "$CliStage/waybridge.dll", "$CliStage/waybridge.deps.json", "$CliStage/waybridge.runtimeconfig.json" $Output
    Copy-Item "$Native/Release/waybridge-client.exe", 'README.md' $Output
    New-Item -ItemType Directory -Force "$Output/docs" | Out-Null
    Copy-Item 'docs/*.md' "$Output/docs/"
    Copy-Item 'scripts/Diagnose-Startup.ps1', 'scripts/Diagnose-Startup.cmd' $Output
    Copy-Item LICENSE "$Output/LICENSE-Waybridge.txt"
    New-Item -ItemType Directory -Force "$Output/licenses" | Out-Null
    Copy-Item "$Native/SDL3-LICENSE.txt" "$Output/licenses/"
    Copy-Item "$Native/Opus-LICENSE.txt" "$Output/licenses/"
    Copy-Item "$Native/nanors-LICENSE.txt" "$Output/licenses/"
    Write-Host "Built $Output/Waybridge.Control.exe"
} finally { Pop-Location }
