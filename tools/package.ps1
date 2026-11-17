# Builds the release archive: dist\RDR2FrameGen-<version>.zip with the mod, its settings file,
# the installer and the documentation. NVIDIA's libraries are not included; the installer fetches them.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path (Join-Path $root 'src\version.h') -Pattern 'FG_VERSION "(.+)"').Matches[0].Groups[1].Value
$asi = Join-Path $root 'build\Release\RDR2FrameGen.asi'
if (-not (Test-Path $asi)) { throw 'Build the mod first: cmake --build build --config Release' }

$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "RDR2FrameGen-$version"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($file in 'RDR2FrameGen.ini', 'install.ps1', 'install.bat', 'README.md', 'README.ru.md', 'LICENSE', 'THIRD_PARTY.md') {
    Copy-Item (Join-Path $root $file) $stage
}
Copy-Item $asi $stage
$zip = "$stage.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "Packed $zip"
