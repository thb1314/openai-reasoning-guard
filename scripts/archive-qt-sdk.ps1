param(
    [Parameter(Mandatory = $true)]
    [string]$QtRoot,
    [ValidateSet("windows-x86_64", "windows-x86_32", "windows-arm64")]
    [string]$Target = $(if ($env:TARGET) { $env:TARGET } else { "windows-x86_64" }),
    [string]$Repo = $(if ($env:REPO) { $env:REPO } else { "thb1314/openai-reasoning-guard" }),
    [string]$ReleaseTag = $env:RELEASE_TAG,
    [string]$SecretName = $env:SECRET_NAME,
    [string]$DistDir = $env:DIST_DIR,
    [string]$UploadProxy = $env:UPLOAD_PROXY,
    [switch]$Upload,
    [switch]$SetSecret,
    [switch]$Clean
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $DistDir) {
    $DistDir = Join-Path $ProjectDir "dist/qt-sdk"
}
if (-not $ReleaseTag) {
    $ReleaseTag = "qt-sdk-$Target"
}
if (-not $SecretName) {
    switch ($Target) {
        "windows-x86_64" { $SecretName = "QT_WINDOWS_X86_64_URL" }
        "windows-x86_32" { $SecretName = "QT_WINDOWS_X86_32_URL" }
        "windows-arm64" { $SecretName = "QT_WINDOWS_ARM64_URL" }
    }
}

$QtRoot = (Resolve-Path $QtRoot).Path
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
    $path = Join-Path $QtRoot $item
    if (-not (Test-Path $path)) {
        throw "Required Qt artifact missing: $path"
    }
}

New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
$archive = Join-Path $DistDir "qt5-$Target.zip"
if ($Clean) {
    Remove-Item -Force $archive -ErrorAction SilentlyContinue
}

$parent = Split-Path -Parent $QtRoot
$base = Split-Path -Leaf $QtRoot
Push-Location $parent
try {
    Remove-Item -Force $archive -ErrorAction SilentlyContinue
    Compress-Archive -Path $base -DestinationPath $archive -Force
} finally {
    Pop-Location
}

Write-Host "Built Qt SDK archive: $archive"
Get-Item $archive | Format-List FullName,Length

if ($Upload -or $SetSecret) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "gh CLI is required for -Upload or -SetSecret"
    }
    if ($UploadProxy) {
        $env:HTTP_PROXY = $UploadProxy
        $env:HTTPS_PROXY = $UploadProxy
        $env:ALL_PROXY = $UploadProxy
    }
}

if ($Upload) {
    gh release view $ReleaseTag -R $Repo *> $null
    if ($LASTEXITCODE -eq 0) {
        gh release upload $ReleaseTag $archive -R $Repo --clobber
    } else {
        gh release create $ReleaseTag $archive `
            -R $Repo `
            --title "Qt SDK $Target" `
            --notes "Qt SDK archive for $Target GitHub Actions builds." `
            --prerelease
    }
    if ($LASTEXITCODE -ne 0) {
        throw "gh release upload/create failed"
    }
}

if ($SetSecret) {
    $assetUrl = "https://github.com/$Repo/releases/download/$ReleaseTag/$(Split-Path -Leaf $archive)"
    $assetUrl | gh secret set $SecretName -R $Repo
    if ($LASTEXITCODE -ne 0) {
        throw "gh secret set failed"
    }
    Write-Host "$SecretName=$assetUrl"
}
