# Builds NVLOW into a single self-contained dist\NVLOW.exe.
#   .\build.ps1            Release build
#   .\build.ps1 -Debug     Debug build
#   .\build.ps1 -Installer Release build + dist\NVLOW-Setup-<version>.exe (needs Inno Setup 6)
param([switch]$Debug, [switch]$Installer)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Find-Exe($name, $globs) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($g in $globs) {
        $hit = Get-ChildItem $g -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
$gxx = Find-Exe 'g++' @("$winget\BrechtSanders.WinLibs*\mingw64\bin\g++.exe", 'C:\msys64\ucrt64\bin\g++.exe', 'C:\mingw64\bin\g++.exe')
if (-not $gxx) { throw "No C++ compiler found. Install one with:  winget install BrechtSanders.WinLibs.POSIX.UCRT" }
$binDir = Split-Path $gxx
$env:PATH = "$binDir;$env:PATH"

$cmake = Find-Exe 'cmake' @("$binDir\cmake.exe", 'C:\Program Files\CMake\bin\cmake.exe')
if (-not $cmake) { throw "CMake not found. Install it with:  winget install Kitware.CMake" }
$ninja = Find-Exe 'ninja' @("$binDir\ninja.exe")
$generator = if ($ninja) { 'Ninja' } else { 'MinGW Makefiles' }

$icon = Join-Path $root 'res\nvlow.ico'
if (-not (Test-Path $icon)) { & (Join-Path $root 'tools\make_icon.ps1') }

$config = if ($Debug) { 'Debug' } else { 'Release' }
$build = Join-Path $root "build\$($config.ToLower())"

& $cmake -S $root -B $build -G $generator "-DCMAKE_BUILD_TYPE=$config" "-DCMAKE_CXX_COMPILER=$gxx"
if ($LASTEXITCODE) { throw "CMake configure failed" }
& $cmake --build $build --parallel
if ($LASTEXITCODE) { throw "Build failed" }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $build 'NVLOW.exe') $dist -Force
# NVLOW uses Windows Media Foundation now; remove FFmpeg copies left by older builds.
Remove-Item (Join-Path $dist 'ffmpeg.exe'), (Join-Path $dist 'ffprobe.exe') -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "NVLOW built: $dist\NVLOW.exe" -ForegroundColor Green

if ($Installer) {
    $iscc = Find-Exe 'ISCC' @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
    if (-not $iscc) { throw "Inno Setup not found. Install it with:  winget install JRSoftware.InnoSetup" }
    # Single source of truth for the version: project(... VERSION x.y.z) in CMakeLists.txt
    $version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'VERSION\s+(\d+\.\d+\.\d+)\s*\r?\n\s*DESCRIPTION').Groups[1].Value
    & $iscc /Q "/DAppVersion=$version" (Join-Path $root 'installer\NVLOW.iss')
    if ($LASTEXITCODE) { throw "Installer build failed" }
    Write-Host "Installer built: $dist\NVLOW-Setup-$version.exe" -ForegroundColor Green
}
