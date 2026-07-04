param(
    [ValidateSet("x86_64", "x86_32")]
    [string]$Arch = $(if ($env:PACKAGE_ARCH) { $env:PACKAGE_ARCH } else { "x86_64" }),
    [string]$QtRoot = $env:QT_ROOT,
    [string]$BuildDir = $env:BUILD_DIR,
    [string]$DistDir = $env:DIST_DIR,
    [string]$PackageId = $(if ($env:PACKAGE_ID) { $env:PACKAGE_ID } else { "openai-reasoning-guard" }),
    [string]$AppName = $(if ($env:APP_NAME) { $env:APP_NAME } else { "OpenAI Reasoning Guard" }),
    [string]$Version = $env:VERSION,
    [string]$Configuration = $(if ($env:CONFIGURATION) { $env:CONFIGURATION } else { "Release" }),
    [string]$CMakeGenerator = $env:CMAKE_GENERATOR,
    [string]$CMakeArchitecture = $env:CMAKE_GENERATOR_PLATFORM,
    [switch]$SkipBuild,
    [switch]$Clean,
    [switch]$BuildTests
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectDir "build-package-windows-$Arch"
}
if (-not $DistDir) {
    $DistDir = Join-Path $ProjectDir "dist"
}
if (-not $Version) {
    $cmakeText = Get-Content (Join-Path $ProjectDir "CMakeLists.txt") -Raw
    if ($cmakeText -match 'project\([^)]*VERSION\s+([0-9A-Za-z_.+-]+)') {
        $Version = $Matches[1]
    } else {
        $Version = "0.1.0"
    }
}
if (-not $QtRoot) {
    throw "QT_ROOT or -QtRoot is required. It must point to a Qt 5 SDK containing bin/moc.exe."
}

$QtRoot = (Resolve-Path $QtRoot).Path
$QtBin = Join-Path $QtRoot "bin"
$QtPlugins = Join-Path $QtRoot "plugins"
$MocPath = Join-Path $QtBin "moc.exe"
if (-not (Test-Path $MocPath)) {
    throw "Qt moc not found: $MocPath"
}

$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$DistDir = [System.IO.Path]::GetFullPath($DistDir)
$StageDir = Join-Path $DistDir "$PackageId-windows-$Arch"
$ZipPath = Join-Path $DistDir "$PackageId-windows-$Arch-$Version.zip"

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Find-BuiltExe {
    param([string]$Name)
    $candidates = @(
        (Join-Path $BuildDir $Name),
        (Join-Path (Join-Path $BuildDir $Configuration) $Name)
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    throw "Built executable not found: $Name under $BuildDir"
}

if ($Clean) {
    Remove-Item -Recurse -Force $StageDir -ErrorAction SilentlyContinue
    Remove-Item -Force $ZipPath -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

if (-not $SkipBuild) {
    $buildTestsValue = if ($BuildTests) { "ON" } else { "OFF" }
    $configureArgs = @(
        "-S", $ProjectDir,
        "-B", $BuildDir,
        "-DNET_TUNNEL_QT_SDK_ROOT=$QtRoot",
        "-DNET_TUNNEL_BUILD_TESTS=$buildTestsValue"
    )
    if ($CMakeGenerator) {
        $configureArgs += @("-G", $CMakeGenerator)
    }
    if ($CMakeArchitecture) {
        $configureArgs += @("-A", $CMakeArchitecture)
    }
    if (-not $CMakeGenerator -or $CMakeGenerator -notmatch "Visual Studio") {
        $configureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
    }
    Invoke-Checked "cmake" $configureArgs
    Invoke-Checked "cmake" @("--build", $BuildDir, "--config", $Configuration, "--parallel")
}

Remove-Item -Recurse -Force $StageDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "plugins/platforms") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "fonts") | Out-Null

Copy-Item (Find-BuiltExe "net-tunnel-gui.exe") (Join-Path $StageDir "$PackageId-gui.exe")
Copy-Item (Find-BuiltExe "net-tunnel-cli.exe") (Join-Path $StageDir "$PackageId-cli.exe")

foreach ($file in @("config.example.json", "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md")) {
    $source = Join-Path $ProjectDir $file
    if (Test-Path $source) {
        Copy-Item $source $StageDir
    }
}

foreach ($dll in @("Qt5Core.dll", "Qt5Network.dll", "Qt5Gui.dll", "Qt5Widgets.dll")) {
    $source = Join-Path $QtBin $dll
    if (-not (Test-Path $source)) {
        throw "Required Qt runtime DLL missing: $source"
    }
    Copy-Item $source $StageDir
}

foreach ($pattern in @("libssl*.dll", "libcrypto*.dll", "ssleay32.dll", "libeay32.dll", "zlib*.dll", "icu*.dll")) {
    Get-ChildItem -Path (Join-Path $QtBin $pattern) -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $StageDir -Force }
}

$qwindows = Join-Path $QtPlugins "platforms/qwindows.dll"
if (-not (Test-Path $qwindows)) {
    throw "Required Qt platform plugin missing: $qwindows"
}
Copy-Item $qwindows (Join-Path $StageDir "plugins/platforms")

$fontDir = Join-Path $ProjectDir "third_party/fonts"
if (Test-Path $fontDir) {
    Get-ChildItem -Path $fontDir -Filter "*.ttf" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $StageDir "fonts") -Force }
}

Set-Content -Encoding ASCII -Path (Join-Path $StageDir "qt.conf") -Value @"
[Paths]
Prefix = .
Plugins = plugins
"@

Remove-Item -Force $ZipPath -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -Force

Write-Host "Package: $PackageId"
Write-Host "Version: $Version"
Write-Host "Arch: $Arch"
Write-Host "Qt root: $QtRoot"
Write-Host "Built Windows package: $ZipPath"
