param(
    [Parameter(Mandatory = $true)]
    [string]$QtBaseSourceArchive,
    [ValidateSet("windows-x86_64", "windows-x86_32", "windows-arm64")]
    [string]$Target = $(if ($env:TARGET) { $env:TARGET } else { "windows-x86_64" }),
    [string]$OpenSslRoot = $env:OPENSSL_ROOT,
    [switch]$NoOpenSsl,
    [string]$OutputRoot = $env:OUTPUT_ROOT,
    [string]$BuildDir = $env:BUILD_DIR,
    [string]$Prefix = $env:PREFIX,
    [int]$Jobs = $(if ($env:JOBS) { [int]$env:JOBS } else { [Environment]::ProcessorCount }),
    [switch]$Clean,
    [switch]$Archive,
    [switch]$Upload,
    [switch]$SetSecret,
    [string]$UploadProxy = $env:UPLOAD_PROXY,
    [string]$Repo = $(if ($env:REPO) { $env:REPO } else { "thb1314/openai-reasoning-guard" }),
    [string]$ReleaseTag = $env:RELEASE_TAG
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $ProjectDir "dist/qt-sdk-build"
}
if (-not $BuildDir) {
    $BuildDir = Join-Path $OutputRoot "build-$Target"
}
if (-not $Prefix) {
    $Prefix = Join-Path $OutputRoot "qt5-$Target"
}
if (-not $ReleaseTag) {
    $ReleaseTag = "qt-sdk-$Target"
}

if (-not (Test-Path $QtBaseSourceArchive)) {
    throw "QtBaseSourceArchive not found: $QtBaseSourceArchive"
}
if (-not $NoOpenSsl -and -not $OpenSslRoot) {
    throw "OpenSslRoot is required for HTTPS-capable Windows Qt builds. Pass -NoOpenSsl only for local experiments."
}
if ($OpenSslRoot) {
    $OpenSslRoot = (Resolve-Path $OpenSslRoot).Path
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Find-MakeTool {
    $jom = Get-Command jom -ErrorAction SilentlyContinue
    if ($jom) { return $jom.Source }
    $nmake = Get-Command nmake -ErrorAction SilentlyContinue
    if ($nmake) { return $nmake.Source }
    throw "Neither jom nor nmake was found. Run from a Visual Studio developer shell."
}

if ($Clean) {
    Remove-Item -Recurse -Force $BuildDir, $Prefix -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Prefix) | Out-Null

$sourceDir = Join-Path $BuildDir "qtbase-src"
$qtBuild = Join-Path $BuildDir "qtbase-build"
Remove-Item -Recurse -Force $sourceDir, $qtBuild -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $sourceDir, $qtBuild | Out-Null

tar -xf $QtBaseSourceArchive -C $sourceDir --strip-components=1
if ($LASTEXITCODE -ne 0) {
    throw "failed to extract Qt source archive"
}

$configure = Join-Path $sourceDir "configure.bat"
if (-not (Test-Path $configure)) {
    throw "configure.bat not found in Qt source: $configure"
}

$configureArgs = @(
    "-prefix", $Prefix,
    "-opensource",
    "-confirm-license",
    "-release",
    "-shared",
    "-nomake", "examples",
    "-nomake", "tests",
    "-make", "libs",
    "-make", "tools",
    "-opengl", "desktop",
    "-no-icu"
)

if ($NoOpenSsl) {
    $configureArgs += "-no-openssl"
} else {
    $configureArgs += @(
        "-openssl-runtime",
        "-I", (Join-Path $OpenSslRoot "include"),
        "-L", (Join-Path $OpenSslRoot "lib")
    )
}

Push-Location $qtBuild
try {
    Invoke-Checked $configure $configureArgs
    $make = Find-MakeTool
    if ((Split-Path -Leaf $make) -ieq "jom.exe") {
        Invoke-Checked $make @("-j", "$Jobs")
    } else {
        Invoke-Checked $make @()
    }
    Invoke-Checked $make @("install")
} finally {
    Pop-Location
}

if ($OpenSslRoot) {
    Get-ChildItem -Path (Join-Path $OpenSslRoot "bin") -Include "libssl*.dll", "libcrypto*.dll", "ssleay32.dll", "libeay32.dll" -File -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $Prefix "bin") -Force }
}

$required = @(
    "bin/moc.exe",
    "bin/Qt5Core.dll",
    "bin/Qt5Network.dll",
    "bin/Qt5Gui.dll",
    "bin/Qt5Widgets.dll",
    "bin/Qt5Test.dll",
    "plugins/platforms/qwindows.dll"
)
foreach ($item in $required) {
    $path = Join-Path $Prefix $item
    if (-not (Test-Path $path)) {
        throw "built SDK is missing required artifact: $path"
    }
}

if ($Archive -or $Upload -or $SetSecret) {
    $archiveArgs = @(
        "-QtRoot", $Prefix,
        "-Target", $Target,
        "-Repo", $Repo,
        "-ReleaseTag", $ReleaseTag
    )
    if ($Upload) { $archiveArgs += "-Upload" }
    if ($SetSecret) { $archiveArgs += "-SetSecret" }
    if ($UploadProxy) { $archiveArgs += @("-UploadProxy", $UploadProxy) }
    & (Join-Path $ScriptDir "archive-qt-sdk.ps1") @archiveArgs
    if ($LASTEXITCODE -ne 0) {
        throw "archive-qt-sdk.ps1 failed"
    }
}

Write-Host "Built Qt SDK: $Prefix"
