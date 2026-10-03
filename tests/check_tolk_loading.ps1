$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bridge = Join-Path $Root "third_party\foobar2000-sdk\foobar2000\foo_speaklyrics\tolk_bridge.cpp"
$TolkSrc = Join-Path $Root "third_party\tolk\src"

function Read-Source($Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing source file: $Path" }
    return [IO.File]::ReadAllText($Path)
}

$bridgeText = Read-Source $Bridge
if ($bridgeText -match "SetDllDirectoryW|GetDllDirectoryW|scoped_tolk_load_environment") {
    throw "Tolk component still changes the process-wide DLL search path."
}
if ($bridgeText -notmatch "LoadLibraryExW\([^;]+LOAD_WITH_ALTERED_SEARCH_PATH") {
    throw "Tolk component does not use an isolated absolute-path loader."
}

$tolkText = Read-Source (Join-Path $TolkSrc "Tolk.cpp")
if ($tolkText -notmatch "GetModuleFileNameW") { throw "Tolk module directory lookup is missing." }
if ($tolkText -notmatch "HMODULE LoadDriverLibrary") { throw "Tolk private driver loader is missing." }
if ($tolkText -notmatch "LoadLibraryExW\([^;]+LOAD_WITH_ALTERED_SEARCH_PATH") { throw "Tolk private driver loader does not use altered search path loading." }

$drivers = @(
    "ScreenReaderDriverBOY.cpp",
    "ScreenReaderDriverNVDA.cpp",
    "ScreenReaderDriverSA.cpp",
    "ScreenReaderDriverSNova.cpp",
    "ScreenReaderDriverZDSR.cpp"
)
foreach ($driver in $drivers) {
    $text = Read-Source (Join-Path $TolkSrc $driver)
    if ($text -notmatch "LoadDriverLibrary") { throw "$driver does not use the private driver loader." }
    if ($text -match 'LoadLibrary(?:W|ExW)?\(L"(?:byctrl|BoyCtrl|nvdaController|SAAPI|dolapi|ZDSRAPI)') {
        throw "$driver still contains a bare screen reader dependency load."
    }
}

Write-Host "Tolk DLL loading isolation check passed."
