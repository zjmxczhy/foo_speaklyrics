$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$VcRoot = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"
$TolkSrc = Join-Path $Root "third_party\tolk\src"
$TolkPatchRoot = Join-Path $Root "third_party\tolk-with-zdsr"
$Proj = Join-Path $Root "third_party\foobar2000-sdk\foobar2000\foo_speaklyrics\foo_speaklyrics.vcxproj"
$DownloaderProj = Join-Path $Root "tools\LrcDownloader\LrcDownloader.csproj"
$SdkArchive = Join-Path $Root "third_party\SDK-2025-03-07.7z"
$SdkRoot = Join-Path $Root "third_party\foobar2000-sdk"

function Assert-NativeExitCode($Description) {
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE." }
}

function Ensure-FoobarSharedLibraries() {
    $Win32Lib = Join-Path $SdkRoot "foobar2000\shared\shared-Win32.lib"
    $X64Lib = Join-Path $SdkRoot "foobar2000\shared\shared-x64.lib"
    if ((Test-Path $Win32Lib) -and (Test-Path $X64Lib)) { return }
    if (-not (Test-Path $SdkArchive)) { throw "Missing SDK archive: $SdkArchive" }

    Write-Host "Extracting foobar2000 shared libraries from the bundled SDK archive..."
    & tar -xf $SdkArchive -C $SdkRoot "foobar2000/shared/shared-Win32.lib" "foobar2000/shared/shared-x64.lib"
    Assert-NativeExitCode "SDK library extraction"
    if (-not (Test-Path $Win32Lib)) { throw "Missing extracted library: $Win32Lib" }
    if (-not (Test-Path $X64Lib)) { throw "Missing extracted library: $X64Lib" }
}

function Get-Vcvars($Name) {
    $Vcvars = Join-Path $VcRoot $Name
    if (-not (Test-Path $Vcvars)) { throw "Missing Visual Studio env script: $Vcvars" }
    return $Vcvars
}

function Build-Tolk($Arch, $VcvarsName) {
    $Vcvars = Get-Vcvars $VcvarsName
    $TolkOut = Join-Path $Root "build\tolk\$Arch"
    New-Item -ItemType Directory -Force -Path $TolkOut | Out-Null
    Write-Host "Building Tolk $Arch..."
    cmd /c "`"$Vcvars`" && cd /d `"$TolkSrc`" && rc /nologo /fo `"$TolkOut\Tolk.res`" Tolk.rc && cl /nologo /O2 /EHsc /LD /Gw /W4 /D_EXPORTING /DUNICODE /Fe:`"$TolkOut\Tolk.dll`" Tolk.cpp ScreenReaderDriverBOY.cpp ScreenReaderDriverJAWS.cpp ScreenReaderDriverNVDA.cpp ScreenReaderDriverSA.cpp ScreenReaderDriverSNova.cpp ScreenReaderDriverWE.cpp ScreenReaderDriverZDSR.cpp ScreenReaderDriverZT.cpp ScreenReaderDriverSAPI.cpp fsapi.c wineyes.c zt.c `"$TolkOut\Tolk.res`" User32.Lib Ole32.Lib OleAut32.Lib"
    Assert-NativeExitCode "Tolk $Arch build"
    Use-Patched-Tolk $Arch
}


function Use-Patched-Tolk($Arch) {
    $PatchDir = Join-Path $TolkPatchRoot $Arch
    if (-not (Test-Path $PatchDir)) { return }
    $TolkOut = Join-Path $Root "build\tolk\$Arch"
    $TolkDll = Join-Path $PatchDir "Tolk.dll"
    if (-not (Test-Path $TolkDll)) { throw "Missing patched Tolk.dll: $TolkDll" }
    Copy-Item $TolkDll $TolkOut -Force

    $TolkLibDir = Join-Path $Root "third_party\tolk\libs\$Arch"
    if (Test-Path $TolkLibDir) {
        Remove-Item -LiteralPath $TolkLibDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $TolkLibDir | Out-Null
    Get-ChildItem $PatchDir -File | Where-Object { $_.Extension -in ".dll", ".ini", ".conf" -and $_.Name -ne "Tolk.dll" } | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $TolkLibDir -Force
    }

    # ZDSRAPI reads its INI through the legacy ANSI code page. Keep the source
    # template in UTF-8 for maintainability, then emit CP936/GBK for ZDSR.
    $ZdsrLyricsChannelConfig = Join-Path $TolkPatchRoot "ZDSRAPI-lyrics-channel.ini"
    $ZdsrTargetConfig = Join-Path $TolkLibDir "ZDSRAPI.ini"
    $ZdsrText = [IO.File]::ReadAllText($ZdsrLyricsChannelConfig, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText($ZdsrTargetConfig, $ZdsrText, [Text.Encoding]::GetEncoding(936))
    Write-Host "Using patched Tolk with ZDSR ${Arch}: $PatchDir"
}

function Build-Component($Platform, $VcvarsName) {
    $Vcvars = Get-Vcvars $VcvarsName
    Write-Host "Building foo_speaklyrics $Platform..."
    cmd /c "`"$Vcvars`" && msbuild `"$Proj`" /p:Configuration=Release /p:Platform=$Platform /p:PlatformToolset=v143 /m"
    Assert-NativeExitCode "foo_speaklyrics $Platform build"
}

function Build-Downloader() {
    $Vcvars = Get-Vcvars "vcvars64.bat"
    if (-not (Test-Path $DownloaderProj)) { throw "Missing downloader project: $DownloaderProj" }
    Write-Host "Building LrcDownloader (.NET Framework 4.8)..."
    cmd /c "`"$Vcvars`" && msbuild `"$DownloaderProj`" /p:Configuration=Release /p:Platform=AnyCPU /m"
    Assert-NativeExitCode "LrcDownloader build"

    $DownloaderExe = Join-Path $Root "build\LrcDownloader\Release\LrcDownloader.exe"
    & $DownloaderExe --self-test
    Assert-NativeExitCode "LrcDownloader self-test"
}

Ensure-FoobarSharedLibraries
Build-Downloader
Build-Tolk "x64" "vcvars64.bat"
Build-Component "x64" "vcvars64.bat"
Build-Tolk "x86" "vcvars32.bat"
Build-Component "Win32" "vcvars32.bat"

& (Join-Path $Root "package.ps1")
