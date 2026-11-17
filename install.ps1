# Installs RDR2FrameGen into the game folder.
#
#   -Game <folder>           game folder (found through Steam / Rockstar registry entries when omitted)
#   -StreamlineSdk <folder>  take NVIDIA's libraries from an unpacked Streamline SDK instead of downloading
#   -Development             use Streamline's development libraries (adds the sl.imgui debug overlay)
#   -WithIni                 overwrite RDR2FrameGen.ini in the game folder (kept otherwise)
#
# NVIDIA's Streamline libraries are not shipped with the mod. They are taken from NVIDIA's official
# release on GitHub (about 280 MB, downloaded once and kept in %LOCALAPPDATA%\RDR2FrameGen).
param(
    [string]$Game,
    [string]$StreamlineSdk,
    [switch]$Development,
    [switch]$WithIni
)
$ErrorActionPreference = 'Stop'
$streamlineVersion = '2.14.16'
$streamlineUrl = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$streamlineVersion/streamline-sdk-v$streamlineVersion.zip"
$here = $PSScriptRoot

function Find-Game {
    $candidates = @(
        @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 1174180', 'InstallLocation'),
        @('HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\Red Dead Redemption 2', 'InstallFolder'),
        @('HKLM:\SOFTWARE\Rockstar Games\Red Dead Redemption 2', 'InstallFolder')
    )
    foreach ($candidate in $candidates) {
        $value = (Get-ItemProperty -Path $candidate[0] -Name $candidate[1] -ErrorAction SilentlyContinue).($candidate[1])
        if ($value -and (Test-Path (Join-Path $value 'RDR2.exe'))) { return $value }
    }
    return $null
}

if (-not $Game) { $Game = Find-Game }
if (-not $Game) { throw 'Game folder not found. Run again with -Game "<folder that contains RDR2.exe>".' }
if (-not (Test-Path (Join-Path $Game 'RDR2.exe'))) { throw "RDR2.exe not found in $Game" }
if (Get-Process -Name RDR2 -ErrorAction SilentlyContinue) { throw 'RDR2 is running; close the game first.' }

# The mod itself: next to this script (release archive) or in the build folder (source checkout).
$asi = Join-Path $here 'RDR2FrameGen.asi'
if (-not (Test-Path $asi)) { $asi = Join-Path $here 'build\Release\RDR2FrameGen.asi' }
if (-not (Test-Path $asi)) { throw 'RDR2FrameGen.asi not found next to install.ps1 or in build\Release.' }

$files = 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll', 'NvLowLatencyVk.dll'
if ($Development) { $files += 'sl.imgui.dll' }
$subfolder = if ($Development) { 'bin/x64/development/' } else { 'bin/x64/' }

# Streamline runtime: a private folder, so nothing else in the game folder picks these libraries up.
$runtime = Join-Path $Game 'RDR2FrameGen'
New-Item -ItemType Directory -Force $runtime | Out-Null

if (-not $StreamlineSdk -and (Test-Path (Join-Path $here 'external\streamline\bin\x64\sl.interposer.dll'))) {
    $StreamlineSdk = Join-Path $here 'external\streamline'
}
if ($StreamlineSdk) {
    foreach ($file in $files) { Copy-Item (Join-Path $StreamlineSdk ($subfolder + $file)) $runtime -Force }
} else {
    $cache = Join-Path $env:LOCALAPPDATA 'RDR2FrameGen'
    $zip = Join-Path $cache "streamline-sdk-v$streamlineVersion.zip"
    if (-not (Test-Path $zip)) {
        New-Item -ItemType Directory -Force $cache | Out-Null
        Write-Host "Downloading NVIDIA Streamline $streamlineVersion (about 280 MB) ..."
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $partial = "$zip.part"
        & curl.exe --location --fail --progress-bar --output $partial $streamlineUrl
        if ($LASTEXITCODE -ne 0) { throw "Download failed. Get $streamlineUrl by hand, unpack it and run again with -StreamlineSdk <folder>." }
        Move-Item $partial $zip -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($file in $files) {
            $entry = $archive.GetEntry($subfolder + $file)
            if (-not $entry) { throw "$file is missing from the Streamline archive" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $runtime $file), $true)
        }
    } finally { $archive.Dispose() }
}
if (-not $Development) { Remove-Item (Join-Path $runtime 'sl.imgui.dll') -ErrorAction SilentlyContinue }

# The game's own DLSS library: once Streamline has initialised NGX, NGX looks for features only in the
# Streamline runtime folder, so the game's later init would otherwise find no DLSS (the option vanishes).
# Run the installer again after the game or a DLSS swapper updates nvngx_dlss.dll.
$dlss = Join-Path $Game 'nvngx_dlss.dll'
if (-not (Test-Path $dlss)) { throw "nvngx_dlss.dll not found in $Game; the game files look incomplete." }
Copy-Item $dlss $runtime -Force

Copy-Item $asi $Game -Force
$ini = Join-Path $Game 'RDR2FrameGen.ini'
if ($WithIni -or -not (Test-Path $ini)) { Copy-Item (Join-Path $here 'RDR2FrameGen.ini') $ini -Force }

Write-Host "RDR2FrameGen installed to $Game"
if (-not (Test-Path (Join-Path $Game 'ScriptHookRDR2.dll'))) {
    Write-Warning 'ScriptHookRDR2.dll is not in the game folder. The mod needs ScriptHookRDR2 and an ASI loader (dinput8.dll or version.dll).'
}
