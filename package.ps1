$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$DistRoot = Join-Path $Root "dist"
$DownloaderOut = Join-Path $Root "build\LrcDownloader\Release"

function Require-File($Path) {
    if (-not (Test-Path $Path)) { throw "Missing file: $Path" }
}

function Copy-Downloader($TargetDir) {
    $DownloaderTarget = Join-Path $TargetDir "downloader"
    $Exe = Join-Path $DownloaderOut "LrcDownloader.exe"
    Require-File $Exe
    New-Item -ItemType Directory -Force -Path $DownloaderTarget | Out-Null
    Copy-Item $Exe $DownloaderTarget -Force
    $Config = Join-Path $DownloaderOut "LrcDownloader.exe.config"
    if (Test-Path $Config) { Copy-Item $Config $DownloaderTarget -Force }
}

function Copy-Payload($TargetDir, $ArchName, $ComponentPlatform) {
    $ComponentDll = Join-Path $Root "build\foo_speaklyrics\Release\$ComponentPlatform\foo_speaklyrics.dll"
    $TolkDll = Join-Path $Root "build\tolk\$ArchName\Tolk.dll"
    $TolkLibDir = Join-Path $Root "third_party\tolk\libs\$ArchName"
    Require-File $ComponentDll
    Require-File $TolkDll
    if (-not (Test-Path $TolkLibDir)) { throw "Missing directory: $TolkLibDir" }

    $TolkTarget = Join-Path $TargetDir "tolk"
    New-Item -ItemType Directory -Force -Path $TargetDir,$TolkTarget | Out-Null
    Copy-Item $ComponentDll $TargetDir -Force
    Copy-Item $TolkDll $TolkTarget -Force
    Get-ChildItem $TolkLibDir -File | Where-Object { $_.Extension -in ".dll", ".ini", ".conf" } | ForEach-Object {
        Copy-Item $_.FullName $TolkTarget -Force
    }
    Copy-Downloader $TargetDir
}

New-Item -ItemType Directory -Force -Path $DistRoot | Out-Null
Remove-Item -Recurse -Force `
    (Join-Path $DistRoot "foo_speaklyrics"), `
    (Join-Path $DistRoot "foo_speaklyrics-fb2k1x-x86"), `
    (Join-Path $DistRoot "foo_speaklyrics-x64"), `
    (Join-Path $DistRoot "foo_speaklyrics-x86"), `
    (Join-Path $DistRoot "foo_speaklyrics-package") `
    -ErrorAction SilentlyContinue
Remove-Item -Force `
    (Join-Path $DistRoot "foo_speaklyrics.fb2k-component"), `
    (Join-Path $DistRoot "foo_speaklyrics.zip"), `
    (Join-Path $DistRoot "foo_speaklyrics-fb2k1x-x86.fb2k-component"), `
    (Join-Path $DistRoot "foo_speaklyrics-fb2k1x-x86.zip"), `
    (Join-Path $DistRoot "foo_speaklyrics-x64.fb2k-component"), `
    (Join-Path $DistRoot "foo_speaklyrics-x86.fb2k-component"), `
    (Join-Path $DistRoot "foo_speaklyrics-x64.zip"), `
    (Join-Path $DistRoot "foo_speaklyrics-x86.zip") `
    -ErrorAction SilentlyContinue

# Single-architecture packages keep files at package root.
$ManualX64 = Join-Path $DistRoot "foo_speaklyrics-x64"
Copy-Payload $ManualX64 "x64" "x64"
Compress-Archive -Path (Join-Path $ManualX64 "*") -DestinationPath (Join-Path $DistRoot "foo_speaklyrics-x64.zip")
Move-Item (Join-Path $DistRoot "foo_speaklyrics-x64.zip") (Join-Path $DistRoot "foo_speaklyrics-x64.fb2k-component") -Force

$ManualX86 = Join-Path $DistRoot "foo_speaklyrics-x86"
Copy-Payload $ManualX86 "x86" "Win32"
Compress-Archive -Path (Join-Path $ManualX86 "*") -DestinationPath (Join-Path $DistRoot "foo_speaklyrics-x86.zip")
Move-Item (Join-Path $DistRoot "foo_speaklyrics-x86.zip") (Join-Path $DistRoot "foo_speaklyrics-x86.fb2k-component") -Force

# foobar2000 1.5 / 1.6 are x86-only in this packaging flow.
# Combined foobar2000 package: root is x86, x64 folder is x64 override.
$PackageDir = Join-Path $DistRoot "foo_speaklyrics-package"
Copy-Payload $PackageDir "x86" "Win32"
Copy-Payload (Join-Path $PackageDir "x64") "x64" "x64"
Compress-Archive -Path (Join-Path $PackageDir "*") -DestinationPath (Join-Path $DistRoot "foo_speaklyrics.zip")
Move-Item (Join-Path $DistRoot "foo_speaklyrics.zip") (Join-Path $DistRoot "foo_speaklyrics.fb2k-component") -Force

Write-Host "Manual x64 folder: $ManualX64"
Write-Host "Manual x86 folder: $ManualX86"
Write-Host "x64 package: $(Join-Path $DistRoot 'foo_speaklyrics-x64.fb2k-component')"
Write-Host "x86 package: $(Join-Path $DistRoot 'foo_speaklyrics-x86.fb2k-component')"
Write-Host "combined package: $(Join-Path $DistRoot 'foo_speaklyrics.fb2k-component')"
