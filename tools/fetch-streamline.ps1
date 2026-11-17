# Downloads NVIDIA's official Streamline SDK release and unpacks it into external\streamline,
# where CMakeLists.txt and install.ps1 look for it. Needed once, for building from source.
param([string]$Version = '2.14.16')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $root 'external\streamline'
if (Test-Path (Join-Path $target 'include\sl.h')) { Write-Host "Streamline SDK already in $target"; return }

$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$Version/streamline-sdk-v$Version.zip"
$zip = Join-Path $env:TEMP "streamline-sdk-v$Version.zip"
Write-Host "Downloading $url (about 280 MB) ..."
& curl.exe --location --fail --progress-bar --output $zip $url
if ($LASTEXITCODE -ne 0) { throw 'Download failed.' }
New-Item -ItemType Directory -Force $target | Out-Null
Expand-Archive -Path $zip -DestinationPath $target -Force
Remove-Item $zip
Write-Host "Streamline SDK $Version unpacked into $target"
