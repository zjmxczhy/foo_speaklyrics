$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$DistRoot = Join-Path $Root "dist"
$DownloaderOut = Join-Path $Root "build\LrcDownloader\Release"
$MainSource = Get-Content (Join-Path $Root "third_party\foobar2000-sdk\foobar2000\foo_speaklyrics\main.cpp") -Raw -Encoding UTF8
$VersionMatch = [regex]::Match($MainSource, 'DECLARE_COMPONENT_VERSION\([^,]+,\s*"([^"]+)"')
if (-not $VersionMatch.Success) { throw "Unable to read component version from main.cpp." }
$Version = $VersionMatch.Groups[1].Value

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
    $ZdsrLyricsChannelConfig = Join-Path $Root "third_party\tolk-with-zdsr\ZDSRAPI-lyrics-channel.ini"
    Require-File $ZdsrLyricsChannelConfig
    # ZDSRAPI parses this legacy configuration as ANSI. Writing UTF-8 here
    # turns “朗读歌词通道” into mojibake such as “鏈楄姝岃瘝閫氶亾”.
    $ZdsrText = [IO.File]::ReadAllText($ZdsrLyricsChannelConfig, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText((Join-Path $TolkTarget "ZDSRAPI.ini"), $ZdsrText, [Text.Encoding]::GetEncoding(936))
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
Get-ChildItem -LiteralPath $DistRoot -Filter "foo_speaklyrics*.fb2k-component" -File | Remove-Item -Force

# Single-architecture packages keep files at package root.
$ManualX64 = Join-Path $DistRoot "foo_speaklyrics-x64"
Copy-Payload $ManualX64 "x64" "x64"
$X64Zip = Join-Path $DistRoot "foo_speaklyrics-$Version-x64.zip"
$X64Package = Join-Path $DistRoot "foo_speaklyrics-$Version-x64.fb2k-component"
Compress-Archive -Path (Join-Path $ManualX64 "*") -DestinationPath $X64Zip
Move-Item $X64Zip $X64Package -Force

$ManualX86 = Join-Path $DistRoot "foo_speaklyrics-x86"
Copy-Payload $ManualX86 "x86" "Win32"
$X86Zip = Join-Path $DistRoot "foo_speaklyrics-$Version-x86.zip"
$X86Package = Join-Path $DistRoot "foo_speaklyrics-$Version-x86.fb2k-component"
Compress-Archive -Path (Join-Path $ManualX86 "*") -DestinationPath $X86Zip
Move-Item $X86Zip $X86Package -Force

Remove-Item -LiteralPath $ManualX64,$ManualX86 -Recurse -Force

Write-Host "x64 package: $X64Package"
Write-Host "x86 package: $X86Package"
