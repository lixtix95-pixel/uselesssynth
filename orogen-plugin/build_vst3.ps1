# OROGEN -- build VST3 with Visual Studio 2022 (x64)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build_vst3.ps1
#   powershell -ExecutionPolicy Bypass -File .\build_vst3.ps1 -Config Debug
#   powershell -ExecutionPolicy Bypass -File .\build_vst3.ps1 -Install
#   powershell -ExecutionPolicy Bypass -File .\build_vst3.ps1 -JucePath C:\JUCE
#
# JUCE 8.0.15 is fetched by CMake (FetchContent) on the first run.

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',
    [string] $JucePath = $env:JUCE_PATH,
    [switch] $Install,
    [switch] $Stands,         # also build console stands (tests / offline render / UI shots)
    [switch] $NoPause         # do not wait for Enter (CI / scheduled runs)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $root 'build-vs2022'
$distDir = Join-Path $root 'dist'
$script:logFile = Join-Path $buildDir 'last-build.log'

function Write-Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
# Ждём Enter, чтобы окно не схлопнулось. На ошибке — всегда (это и есть смысл:
# прочитать лог), на успехе — только если скрипт запущен двойным кликом.
function Wait-IfNeeded([switch] $Always) {
    if ($NoPause) { return }
    if ($Host.Name -ne 'ConsoleHost') { return }
    if (-not $Always) {
        $doubleClick = $false
        try {
            $parentId = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
            $doubleClick = ((Get-Process -Id $parentId -ErrorAction SilentlyContinue).ProcessName -eq 'explorer')
        } catch { $doubleClick = $false }
        if (-not $doubleClick) { return }
    }
    Write-Host ""
    Write-Host "Press Enter to close the window..." -ForegroundColor Yellow
    [void](Read-Host)
}
function Fail($text) {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Red
    Write-Host " BUILD FAILED: $text" -ForegroundColor Red
    Write-Host "============================================================" -ForegroundColor Red
    if (Test-Path $script:logFile) {
        Write-Host " Full log: $script:logFile"
        Write-Host " (open it in Notepad, or attach the whole file when reporting)"
    } else {
        Write-Host " The failure happened before the build step -- copy the text above."
    }
    Write-Host ""
    Write-Host " How to retry:"
    Write-Host "   powershell -ExecutionPolicy Bypass -File .\build_vst3.ps1 -Config $Config"
    Wait-IfNeeded -Always
    exit 1
}

Write-Host "============================================================"
Write-Host " OROGEN -- VST3 build, Visual Studio 2022, x64, $Config"
Write-Host "============================================================"

# ---------------------------------------------------------------- 1. CMake
Write-Step "Looking for CMake"
$cmake = $null
$cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($cmd) { $cmake = $cmd.Source }

if (-not $cmake) {
    # ProgramFiles(x86) может отсутствовать (нестандартная установка/пустое
    # окружение) -- тогда Join-Path бросает исключение и сообщение об ошибке
    # становится бесполезным. Проверяем переменные перед использованием.
    $pf86 = ${env:ProgramFiles(x86)}
    if (-not $pf86) { $pf86 = ${env:ProgramFiles} }
    if ($pf86) {
        $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
            if ($vsPath) {
                $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path $candidate) { $cmake = $candidate }
            }
        }
    }
}
if (-not $cmake) {
    Fail "CMake not found. Install 'Desktop development with C++' in Visual Studio 2022 (it includes CMake), or install CMake 3.22+ and add it to PATH."
}
Write-Host "    $cmake"

# ---------------------------------------------------------------- 2. JUCE
Write-Step "JUCE source"
$juceArg = @()
if ($JucePath) {
    if (Test-Path (Join-Path $JucePath 'modules')) {
        Write-Host "    local copy: $JucePath"
        $juceArg = @("-DOROGEN_JUCE_PATH=$JucePath")
    } else {
        Write-Warning "JUCE_PATH is set but '$JucePath\modules' does not exist -- falling back to download"
        $JucePath = $null
    }
}
if (-not $JucePath) {
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        Fail "git not found in PATH. CMake needs it to download JUCE 8.0.15. Install Git for Windows, or clone JUCE manually and pass -JucePath C:\JUCE."
    }
    Write-Host "    will be downloaded by CMake (first run only)"
}

# ---------------------------------------------------------------- 3. configure
Write-Step "Configuring"
$standsValue = if ($Stands) { 'ON' } else { 'OFF' }
$args1 = @(
    '-S', (Join-Path $root 'orogen-plugin'),
    '-B', $buildDir,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DOROGEN_BUILD_STANDS=$standsValue"
) + $juceArg

& $cmake @args1
if ($LASTEXITCODE -ne 0) { Fail "Configure failed (see messages above)." }

# ---------------------------------------------------------------- 4. build
Write-Step "Building OROGEN_VST3 ($Config)"
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir | Out-Null }
& $cmake --build $buildDir --config $Config --target OROGEN_VST3 --parallel 2>&1 |
    Tee-Object -FilePath $script:logFile
if ($LASTEXITCODE -ne 0) { Fail "Build failed (exit code $LASTEXITCODE, see messages above)." }

# ---------------------------------------------------------------- copy
$artifact = Join-Path $buildDir "OROGEN_artefacts\$Config\VST3\OROGEN.vst3"
if (-not (Test-Path $artifact)) { Fail "Build reported success but '$artifact' is missing." }

if (-not (Test-Path $distDir)) { New-Item -ItemType Directory -Path $distDir | Out-Null }
$target = Join-Path $distDir 'OROGEN.vst3'
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Copy-Item $artifact $target -Recurse -Force

Write-Host ""
Write-Host "------------------------------------------------------------" -ForegroundColor Green
Write-Host " OK. Bundle: $target" -ForegroundColor Green
Write-Host "------------------------------------------------------------" -ForegroundColor Green

if ($Install) {
    Write-Step "Installing to the system VST3 folder"
    $sysVst3 = Join-Path $env:CommonProgramFiles 'VST3'
    if (-not (Test-Path $sysVst3)) { New-Item -ItemType Directory -Path $sysVst3 -Force | Out-Null }
    $dest = Join-Path $sysVst3 'OROGEN.vst3'
    try {
        if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
        Copy-Item $target $dest -Recurse -Force
        Write-Host "    installed: $dest"
    } catch {
        Write-Warning "Copy to Program Files failed ($($_.Exception.Message)). Run the shell as Administrator, or copy dist\OROGEN.vst3 manually."
    }
}

Write-Host ""
Write-Host "Next: copy dist\OROGEN.vst3 to 'C:\Program Files\Common Files\VST3\' and rescan plugins in your DAW."
Write-Host "Build log: $script:logFile"
Write-Host ""
Wait-IfNeeded
